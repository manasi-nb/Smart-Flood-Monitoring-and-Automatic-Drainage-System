#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <math.h>
#include <esp_system.h>

// ==========================================
// 1. WIFI AND THINGSPEAK CONFIGURATION
// ==========================================

const char* AP_SSID = "FloodGateway";
const char* AP_PASS = "Flood12345";

const char* STA_SSID = "__";
const char* STA_PASS = "__";

const char* MQTT_HOST = "mqtt3.thingspeak.com";
const int MQTT_PORT = 1883; 

const char* MQTT_CLIENT_ID = "__";
const char* MQTT_USERNAME = "__";
const char* MQTT_PASSWORD = "__";

const char* CHANNEL_ID = "__FLOOD_MONITORING_CHANNEL_ID__";

const char* CONTROL_CHANNEL_ID = "__PUMP_CONTROL_CHANNEL_ID__";

// ==========================================
// 2. NETWORK OBJECTS
// ==========================================

WebServer server(80);

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

const char* NODE_B_URL = "http://192.168.4.20/api/pump";

const char* NODE_B_RESET_URL = "http://192.168.4.20/api/pump/reset";

String publishTopic;
String subscribeTopic;
String feedbackTopic;

// ==========================================
// 3. SENSOR STATE
// ==========================================

float waterDepth = 0;

int moistureRaw = 0;
int moisturePercent = 0;

String groundCondition = "UNKNOWN";

bool waterReceived = false;
bool moistureReceived = false;

unsigned long lastWater = 0;
unsigned long lastMoisture = 0;

const unsigned long NODE_TIMEOUT = 10000;

const float WARNING_LEVEL = 5.0;
const float CRITICAL_LEVEL = 8.0;
const float STOP_LEVEL = 4.0;

// ==========================================
// 4. PUMP STATE
// ==========================================

enum ControlMode {MANUAL, AUTOMATIC};

ControlMode mode = MANUAL;

bool pumpOn = false;
bool pumpKnown = false;
bool pumpTimedOut = false;

bool autoArmed = true;

String pumpResult = "READY";
String lastCommandID = "";

uint32_t bootID;
unsigned long commandCounter = 0;

// ==========================================
// 5. TIMERS
// ==========================================

unsigned long lastPumpPoll = 0;
unsigned long lastAutoAction = 0;
unsigned long lastPublish = 0;
unsigned long lastMQTTRetry = 0;
unsigned long lastWiFiRetry = 0;

const unsigned long MQTT_INTERVAL = 20000;
const unsigned long AUTO_INTERVAL = 1000;

// Channel 2 feedback
const unsigned long FEEDBACK_INTERVAL = 30000;
const unsigned long COMMAND_SETTLE_MS = 20000;

unsigned long lastFeedback = 0;
unsigned long lastControlUpdate = 0;

bool feedbackEverSent = false;
bool controlUpdateSeen = false;
bool feedbackPending = true;

// Latest command execution outcome
// -1 = none yet, 0 = rejected, 1 = accepted
int executionResult = -1;

// ==========================================
// 6. REMOTE COMMAND STATE
// ==========================================

bool remotePending = false;
int remoteCommand = -1;

int lastRemoteCommand = -1;
int lastRemoteResult = -1;

unsigned long lastRemoteCommandTime = 0;

// ==========================================
// 7. HELPERS
// ==========================================

bool nodeAOnline() {
  return waterReceived &&
    millis() - lastWater < NODE_TIMEOUT;
}

bool nodeBOnline() {
  return moistureReceived &&
    millis() - lastMoisture < NODE_TIMEOUT;
}

String getRisk() {
  if (!nodeAOnline())
    return "UNKNOWN";

  if (waterDepth >= CRITICAL_LEVEL)
    return "CRITICAL";

  if (waterDepth >= WARNING_LEVEL)
    return "WARNING";

  return "NORMAL";
}

String modeName() {
  return mode == AUTOMATIC ? "AUTO" : "MANUAL";
}

String newCommandID() {
  return "gw-" + String(bootID, HEX) + "-" + String(++commandCounter);
}

bool safeToStart() {
  return nodeAOnline() && nodeBOnline() && pumpKnown && waterDepth > STOP_LEVEL;
}

void sendJSON(int status, JsonDocument& doc) {
  String output;
  serializeJson(doc, output);

  server.send(status, "application/json", output);
}

void markFeedback(bool success) {
  executionResult = success ? 1 : 0;
  feedbackPending = true;
}

// ==========================================
// 8. SEND PUMP COMMAND TO NODE B
// ==========================================

bool setPump(bool requested) {

  if (requested && !safeToStart()) {
    pumpResult = "REJECTED_SAFETY";
    Serial.println("Pump ON rejected: safety");
    return false;
  }

  if (!nodeBOnline()) {
    pumpKnown = false;
    pumpResult = "NODE_B_OFFLINE";
    return false;
  }

  String id = newCommandID();

  JsonDocument request;
  request["command_id"] = id;
  request["on"] = requested;
  request["mode"] = modeName();

  String payload;
  serializeJson(request, payload);

  HTTPClient http;

  http.begin(NODE_B_URL);
  http.setTimeout(1200);

  http.addHeader("Content-Type", "application/json");

  int code = http.POST(payload);
  String response = http.getString();

  http.end();

  JsonDocument ack;

  bool valid = code == 200 && 
    deserializeJson(ack, response) ==
      DeserializationError::Ok &&
    ack["success"].is<bool>() &&
    ack["success"].as<bool>() &&
    ack["command_id"].as<String>() == id &&
    ack["pump_on"].is<bool>() &&
    ack["pump_on"].as<bool>() == requested &&
    ack["simulation"].is<bool>() &&
    ack["simulation"].as<bool>();

  if (!valid) {

    pumpKnown = false;
    pumpResult = "HTTP_COMMAND_FAILED";

    Serial.print("Pump command failed: ");
    Serial.println(code);

    return false;
  }

  pumpOn = ack["pump_on"].as<bool>();
  pumpKnown = true;

  pumpTimedOut = ack["timed_out"] | false;

  pumpResult = ack["message"].as<String>();
  lastCommandID = id;

  feedbackPending = true;

  Serial.print("Pump acknowledged: ");
  Serial.println(pumpResult);

  return true;
}

// ==========================================
// 9. POLL NODE B
// ==========================================

void pollPump() {

  if (!nodeBOnline()) {
    if (pumpKnown) {
      pumpKnown = false;
      feedbackPending = true;
    }
    return;
  }

  HTTPClient http;

  http.begin(NODE_B_URL);
  http.setTimeout(1000);

  int code = http.GET();
  String response = http.getString();

  http.end();

  JsonDocument doc;

  if (code != 200 || deserializeJson(doc, response) || !doc["pump_on"].is<bool>() || !doc["simulation"].is<bool>() || !doc["simulation"].as<bool>()) {

    if (pumpKnown) {
      pumpKnown = false;
      feedbackPending = true;
    }
    return;
  }

  bool previousOn = pumpOn;
  bool previousKnown = pumpKnown;

  pumpOn = doc["pump_on"].as<bool>();
  pumpKnown = true;

  pumpTimedOut = doc["timed_out"] | false;

  if (!previousKnown || previousOn != pumpOn) {
    feedbackPending = true;
  }

  if (pumpTimedOut && previousOn && !pumpOn) {
    pumpResult = "TIMEOUT";
    executionResult = 0;
    feedbackPending = true;

    Serial.println("Node B pump timed out");
  }
}

// ==========================================
// 10. RESET PUMP
// ==========================================

bool resetSimulation() {

  Serial.println("===== RESET REQUEST =====");

  HTTPClient http;

  http.begin(NODE_B_RESET_URL);
  http.setTimeout(2000);

  http.addHeader("Content-Type", "application/json");

  int code = http.POST("{}");
  String response = http.getString();

  http.end();

  Serial.print("Reset HTTP code: ");
  Serial.println(code);

  Serial.print("Reset response: ");
  Serial.println(response);

  JsonDocument ack;

  bool valid =
    code == 200 &&
    deserializeJson(ack, response) ==
      DeserializationError::Ok &&
    ack["success"].is<bool>() &&
    ack["success"].as<bool>() &&
    ack["pump_on"].is<bool>() &&
    !ack["pump_on"].as<bool>() &&
    ack["timed_out"].is<bool>() &&
    !ack["timed_out"].as<bool>();

  if (!valid) {
    pumpResult = "RESET_FAILED";
    Serial.println("RESET FAILED");
    return false;
  }

  pumpOn = false;
  pumpKnown = true;
  pumpTimedOut = false;

  mode = MANUAL;
  autoArmed = false;

  pumpResult = "RESET_COMPLETE";

  feedbackPending = true;

  Serial.println("RESET SUCCESSFUL");

  return true;
}

// ==========================================
// 11. MANUAL / AUTOMATIC CONTROL
// ==========================================

bool applyControl(int command) {

  Serial.print("Control command: ");
  Serial.println(command);

  switch (command) {

    case 0: // MANUAL OFF

      mode = MANUAL;

      if (pumpKnown && !pumpOn) {
        pumpResult = "ALREADY_OFF";
        return true;
      }

      return setPump(false);

    case 1: // MANUAL ON

      mode = MANUAL;

      if (!safeToStart()) {
        pumpResult = "REJECTED_SAFETY";
        return false;
      }

      if (pumpOn) {
        // Synchronize Node B's mode without restarting the timer.
        bool ok = setPump(true);
        if (ok) pumpResult = "ALREADY_ON";
        return ok;
      }

      return setPump(true);

    case 2: { // AUTOMATIC

      if (!nodeAOnline() || !nodeBOnline() || !pumpKnown) {

        pumpResult = "SENSORS_NOT_READY";
        return false;
      }

      ControlMode previousMode = mode;
      mode = AUTOMATIC;
      autoArmed = !pumpOn;

      // If pump is already ON in MANUAL, tell Node B that it is now AUTO.
      // Node B changes mode without resetting its run timer.
      if (pumpOn && !setPump(true)) {
        mode = previousMode;
        return false;
      }

      pumpResult = "AUTO_SELECTED";
      return true;
    }
    
    case 3: // RESET

      return resetSimulation();

    default:

      pumpResult = "INVALID_COMMAND";
      return false;
  }
}

// ==========================================
// 12. AUTOMATIC CONTROL
// ==========================================

void automaticControl() {

  if (millis() - lastAutoAction < AUTO_INTERVAL)
    return;

  lastAutoAction = millis();

  // Safety stop in both modes
  if (pumpKnown && pumpOn && (!nodeAOnline() || !nodeBOnline() || waterDepth <= STOP_LEVEL)) {

    if (nodeBOnline()) {
      bool ok = setPump(false);
      markFeedback(ok);
    }

    return;
  }

  if (mode != AUTOMATIC)
    return;

  // Rearm only when water is low
  if (nodeAOnline() && waterDepth <= STOP_LEVEL) {

    autoArmed = true;
  }

  if (!pumpKnown || !nodeAOnline() || !nodeBOnline()) {

    return;
  }

  if (!pumpOn && autoArmed && waterDepth >= CRITICAL_LEVEL && safeToStart()) {

    autoArmed = false;

    Serial.println("AUTO: Critical water level");

    bool ok = setPump(true);
    markFeedback(ok);
  }
}

// ==========================================
// 13. RECEIVE WATER DATA
// ==========================================

void receiveWater() {

  JsonDocument doc;

  if (deserializeJson(doc, server.arg("plain")) || (!doc["water_depth_cm"].is<float>() && !doc["water_depth_cm"].is<int>())) {

    server.send(400, "application/json", "{\"error\":\"Invalid water reading\"}");

    return;
  }
  
  if (doc["node_id"].as<String>() != "node_a") {
  server.send(400, "application/json", "{\"error\":\"Invalid node ID\"}");
  return;
  }

  float value = doc["water_depth_cm"].as<float>();

  if (!isfinite(value) || value < 0 || value > 100) {

    server.send(400, "application/json", "{\"error\":\"Out of range\"}");

    return;
  }

  waterDepth = value;
  waterReceived = true;
  lastWater = millis();

  server.send(200, "application/json", "{\"status\":\"received\"}");
}

// ==========================================
// 14. RECEIVE MOISTURE DATA
// ==========================================

void receiveMoisture() {

  JsonDocument doc;

  if (deserializeJson(doc, server.arg("plain")) || !doc["moisture_raw"].is<int>()) {

    server.send(400, "application/json", "{\"error\":\"Invalid moisture reading\"}");

    return;
  }

  if (doc["node_id"].as<String>() != "node_b") {
  server.send(400, "application/json", "{\"error\":\"Invalid node ID\"}");
  return;
}

  int raw = doc["moisture_raw"].as<int>();

  if (raw < 0 || raw > 4095) {

    server.send(400, "application/json", "{\"error\":\"Out of range\"}");

    return;
  }

  moistureRaw = raw;

  // New calibrated fields, if supplied by Node B
  if (doc["moisture_pct"].is<int>()) {

    moisturePercent = constrain(doc["moisture_pct"].as<int>(), 0, 100);

    groundCondition = doc["ground_condition"] | "UNKNOWN";
  } else {
    groundCondition = "UNKNOWN";
  }

  moistureReceived = true;
  lastMoisture = millis();

  server.send(200, "application/json", "{\"status\":\"received\"}");
}

// ==========================================
// 15. STATUS API
// ==========================================

void getStatus() {

  JsonDocument doc;

  doc["water_depth_cm"] = waterDepth;

  doc["moisture_raw"] = moistureRaw;
  doc["moisture_pct"] = moisturePercent;
  doc["ground_condition"] = groundCondition;

  doc["water_received"] = waterReceived;
  doc["moisture_received"] = moistureReceived;

  doc["node_a_online"] = nodeAOnline();
  doc["node_b_online"] = nodeBOnline();

  doc["water_age_s"] = waterReceived ? (millis() - lastWater) / 1000 : 0;

  doc["moisture_age_s"] = moistureReceived ? (millis() - lastMoisture) / 1000 : 0;

  doc["flood_risk"] = getRisk();

  doc["pump_mode"] = modeName();
  doc["pump_on"] = pumpOn;
  doc["pump_state_known"] = pumpKnown;
  doc["pump_timed_out"] = pumpTimedOut;
  doc["pump_result"] = pumpResult;
  doc["auto_armed"] = autoArmed;

  // Legacy protocol flag; relay is physical
  doc["pump_simulation"] = true;

  doc["internet_connected"] = WiFi.status() == WL_CONNECTED;

  doc["mqtt_connected"] = mqtt.connected();

  doc["cloud_execution_result"] = executionResult;

  sendJSON(200, doc);
}

// ==========================================
// 16. DASHBOARD CONTROL API
// ==========================================

void handleLocalControl() {

  JsonDocument doc;

  if (deserializeJson(doc, server.arg("plain")) || !doc["command"].is<int>()) {

    server.send(400, "application/json", "{\"success\":false,\"message\":\"INVALID_JSON\"}");

    return;
  }

  int command = doc["command"].as<int>();

  bool success = applyControl(command);

  markFeedback(success);

  JsonDocument response;

  response["success"] = success;
  response["message"] = pumpResult;
  response["pump_on"] = pumpOn;
  response["pump_mode"] = modeName();
  response["pump_state_known"] = pumpKnown;

  sendJSON(success ? 200 : 409, response);
}

// ==========================================
// 17. DASHBOARD HTML
// ==========================================

const char DASHBOARD[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport"
 content="width=device-width,initial-scale=1">

<title>Flood Monitoring</title>

<style>
*{box-sizing:border-box}

body{
 font-family:Arial,sans-serif;
 background:#f1f5f9;
 color:#1e293b;
 margin:0;
 padding:18px
}

.container{
 max-width:950px;
 margin:auto
}

h1{text-align:center}

.subtitle{
 text-align:center;
 color:#64748b
}

.grid{
 display:grid;
 grid-template-columns:
 repeat(auto-fit,minmax(230px,1fr));
 gap:16px;
 margin-top:22px
}

.card{
 background:white;
 padding:22px;
 border-radius:12px;
 box-shadow:0 2px 8px #00000012
}

.value{
 font-size:28px;
 font-weight:bold
}

.normal,.online{color:#16a34a}
.warning{color:#d97706}
.critical,.offline{color:#dc2626}

button{
 border:0;
 border-radius:8px;
 padding:12px;
 color:white;
 font-weight:bold;
 cursor:pointer;
 margin:4px
}

.on{background:#16a34a}
.off{background:#dc2626}
.auto{background:#2563eb}
.reset{background:#64748b}

.info{
 color:#64748b;
 font-size:14px
}

#commandResult{font-weight:bold}
</style>
</head>

<body>
<div class="container">

<h1>Flood Monitoring System</h1>

<p class="subtitle">
ESP32-S3 Gateway Dashboard
</p>

<div class="grid">

<div class="card">
<h3>Water Depth - Node A</h3>
<div class="value" id="water">--</div>
<p id="nodeA">Waiting</p>
</div>

<div class="card">
<h3>Soil Moisture - Node B</h3>
<div class="value" id="moisture">--</div>
<p id="nodeB">Waiting</p>
<p>Ground condition:
<b id="ground">--</b></p>
<p>Raw ADC:
<span id="raw">--</span></p>
</div>

<div class="card">
<h3>Flood Risk</h3>
<div class="value" id="risk">UNKNOWN</div>
<p class="info">
Normal: below 5 cm<br>
Warning: 5 to below 8 cm<br>
Critical: 8 cm and above
</p>
</div>

<div class="card">
<h3>Network</h3>
<p id="internet">Internet: --</p>
<p id="mqtt">MQTT: --</p>
</div>

<div class="card">
<h3>Pump Control</h3>

<div class="value" id="pump">UNKNOWN</div>

<p>Mode: <b id="mode">--</b></p>
<p>Result: <b id="result">--</b></p>
<p>Timeout: <b id="timeout">--</b></p>
<p>Auto armed: <b id="armed">--</b></p>

<button type="button" class="on"
 onclick="sendCommand(1)">
Manual ON
</button>

<button type="button" class="off"
 onclick="sendCommand(0)">
Manual OFF
</button>

<button type="button" class="auto"
 onclick="sendCommand(2)">
Automatic
</button>

<button type="button" class="reset"
 onclick="sendCommand(3)">
Reset
</button>

<p id="commandResult" class="info"></p>

</div>

<div class="card">
<h3>ThingSpeak Feedback</h3>
<p>Channel 1: Monitoring</p>
<p>Channel 2: Pump commands and feedback</p>
<p>Last execution result:
<b id="execution">--</b></p>
</div>

</div>

<p class="subtitle">
Dashboard updates every 2 seconds.
Pump status is the reported relay-command state.
</p>

</div>

<script>
let busy=false;

window.sendCommand=async function(value){

 if(busy)return;

 busy=true;

 const box=
  document.getElementById("commandResult");

 box.textContent="Sending command...";
 box.style.color="#64748b";

 try{

  const response=await fetch(
   "/api/pump/control",{
    method:"POST",
    headers:{
     "Content-Type":"application/json"
    },
    body:JSON.stringify({command:value}),
    cache:"no-store"
   }
  );

  const data=await response.json();

  box.textContent=
   (data.success?"SUCCESS: ":"REJECTED: ")+
   data.message;

  box.style.color=
   data.success?"green":"red";

  await updateDashboard();

 }catch(error){

  box.textContent="ERROR: "+error.message;
  box.style.color="red";

 }finally{
  busy=false;
 }
};

async function updateDashboard(){

 try{

  const r=await fetch("/api/status",{
   cache:"no-store"
  });

  if(!r.ok)
   throw new Error("Gateway offline");

  const d=await r.json();

  document.getElementById("water").textContent=
   d.node_a_online
   ?Number(d.water_depth_cm).toFixed(1)+" cm"
   :"--";

  document.getElementById("moisture").textContent=
   d.node_b_online && d.ground_condition!=="UNKNOWN"
   ?d.moisture_pct+"%"
   :"--";

  document.getElementById("raw").textContent=
   d.node_b_online?d.moisture_raw:"--";

  document.getElementById("ground").textContent=
   d.node_b_online?d.ground_condition:"UNKNOWN";

  document.getElementById("nodeA").textContent=
   d.node_a_online?"ONLINE":"OFFLINE";

  document.getElementById("nodeB").textContent=
   d.node_b_online?"ONLINE":"OFFLINE";

  const risk=document.getElementById("risk");

  risk.textContent=d.flood_risk;
  risk.className=
   "value "+d.flood_risk.toLowerCase();

  document.getElementById("pump").textContent=
   d.pump_state_known
   ?(d.pump_on?"ON":"OFF"):"UNKNOWN";

  document.getElementById("mode").textContent=
   d.pump_mode;

  document.getElementById("result").textContent=
   d.pump_result;

  document.getElementById("timeout").textContent=
   d.pump_timed_out?"YES":"NO";

  document.getElementById("armed").textContent=
   d.auto_armed?"YES":"NO";

  document.getElementById("internet").textContent=
   "Internet: "+
   (d.internet_connected?"Connected":"Offline");

  document.getElementById("mqtt").textContent=
   "MQTT: "+
   (d.mqtt_connected?"Connected":"Offline");

  const execution=
   d.cloud_execution_result;

  document.getElementById("execution").textContent=
   execution===1?"ACCEPTED":
   execution===0?"REJECTED":"NONE";

 }catch(e){

  document.getElementById("mqtt").textContent=
   "Gateway connection lost";
 }
}

updateDashboard();
setInterval(updateDashboard,2000);
</script>

</body>
</html>
)HTML";

// ==========================================
// 18. MQTT CALLBACK
// ==========================================

void mqttCallback(char* topic, byte* payload, unsigned int length) {

  if (String(topic) != subscribeTopic)
    return;

  // Remember when the control channel was updated.
  // Feedback must not be sent immediately.
  lastControlUpdate = millis();
  controlUpdateSeen = true;

  if (length != 1)
    return;

  char value = (char)payload[0];

  if (value < '0' || value > '3')
    return;

  remoteCommand = value - '0';
  remotePending = true;

  Serial.print("MQTT command queued: ");
  Serial.println(remoteCommand);
}

// ==========================================
// 19. MQTT CONNECTION
// ==========================================

void connectMQTT() {

  if (mqtt.connected() || WiFi.status() != WL_CONNECTED)
    return;

  if (mqtt.connect(MQTT_CLIENT_ID, MQTT_USERNAME, MQTT_PASSWORD)) {

    Serial.println("MQTT CONNECTED");

    if (mqtt.subscribe(subscribeTopic.c_str())) {

      Serial.println("MQTT SUBSCRIBED");

    } else {

      Serial.println("MQTT SUBSCRIBE FAILED");
    }

    // Refresh cloud state after reconnect.
    feedbackPending = true;

  } else {

    Serial.print("MQTT error: ");
    Serial.println(mqtt.state());
  }
}

// ==========================================
// 20. MONITORING CHANNEL TELEMETRY
// ==========================================

void publishTelemetry() {

  if (!mqtt.connected() || !waterReceived || !moistureReceived)
    return;

  String risk = getRisk();

  int riskCode = -1;

  if (risk == "NORMAL")
    riskCode = 0;

  if (risk == "WARNING")
    riskCode = 1;

  if (risk == "CRITICAL")
    riskCode = 2;

  String payload =
    "field1=" + String(waterDepth, 2) +
    "&field2=" + String(
      nodeBOnline() ? moisturePercent : -1
    ) +
    "&field3=" + String(riskCode) +
    "&field4=" + String(nodeAOnline() ? 1 : 0) +
    "&field5=" + String(nodeBOnline() ? 1 : 0);

  bool sent = mqtt.publish(publishTopic.c_str(), payload.c_str());

  Serial.print("ThingSpeak Channel 1: ");
  Serial.println(sent ? "Sent" : "Failed");

  Serial.println(payload);
}

// ==========================================
// 21. PUMP CONTROL CHANNEL FEEDBACK
// ==========================================

void publishPumpFeedback() {

  if (!mqtt.connected())
    return;

  unsigned long now = millis();

  // Respect interval between gateway writes.
  if (feedbackEverSent && now - lastFeedback < FEEDBACK_INTERVAL)
    return;

  // Allow time after a remote command update.
  if (controlUpdateSeen && now - lastControlUpdate < COMMAND_SETTLE_MS)
    return;

  // Publish on change or periodic state heartbeat.
  if (!feedbackPending && feedbackEverSent && now - lastFeedback < 60000)
    return;

  // Do not publish an unknown state as OFF.
  int state = pumpKnown ? (pumpOn ? 1 : 0) : -1;

  String payload = "field2=" + String(executionResult) + "&field3=" + String(state);

  bool sent = mqtt.publish(feedbackTopic.c_str(), payload.c_str()
  );

  Serial.print("ThingSpeak Channel 2: ");
  Serial.println(sent?"Sent":"Failed");

  Serial.println(payload);

  if (sent) {
    lastFeedback = now;
    feedbackEverSent = true;
    feedbackPending = false;
  }
}

// ==========================================
// 22. SETUP
// ==========================================

void setup() {

  Serial.begin(115200);

  bootID = esp_random();

  WiFi.mode(WIFI_AP_STA);

  WiFi.softAP(
  AP_SSID, AP_PASS, 6, 0, 4);

  WiFi.begin(STA_SSID, STA_PASS);

  Serial.print("Gateway AP IP: ");
  Serial.println(WiFi.softAPIP());

  server.on("/", HTTP_GET, []() {server.send_P(200, "text/html", DASHBOARD);});

  server.on("/api/status", HTTP_GET, getStatus);

  server.on("/api/sensors/water", HTTP_POST, receiveWater);

  server.on("/api/sensors/moisture", HTTP_POST, receiveMoisture);

  server.on("/api/pump/control", HTTP_POST, handleLocalControl);

  server.begin();

  publishTopic = "channels/" + String(CHANNEL_ID) + "/publish";

  subscribeTopic = "channels/" + String(CONTROL_CHANNEL_ID) + "/subscribe/fields/field1";

  feedbackTopic = "channels/" + String(CONTROL_CHANNEL_ID) + "/publish";

  mqtt.setServer(MQTT_HOST, MQTT_PORT);

  mqtt.setCallback(mqttCallback);
  mqtt.setSocketTimeout(2);
  mqtt.setBufferSize(1024);

  Serial.println("Gateway Started");
  Serial.println("NeoPixel + Moisture + MQTT Feedback");
}

// ==========================================
// 23. MAIN LOOP
// ==========================================

void loop() {

  server.handleClient();

  // Internet Wi-Fi reconnection
  if (WiFi.status() != WL_CONNECTED && millis() - lastWiFiRetry >= 10000) {

    lastWiFiRetry = millis();
    WiFi.reconnect();
  }

  // MQTT reconnection
  if (WiFi.status() == WL_CONNECTED && !mqtt.connected() && millis() - lastMQTTRetry >= 5000) {

    lastMQTTRetry = millis();
    connectMQTT();
  }

  if (mqtt.connected())
    mqtt.loop();

  // Process remote commands outside MQTT callback
  if (remotePending) {

    int cmd = remoteCommand;
    remotePending = false;

    Serial.println("===== REMOTE MQTT COMMAND =====");

    Serial.print("Command: ");
    Serial.println(cmd);

    bool success = applyControl(cmd);

    lastRemoteCommand = cmd;
    lastRemoteResult = success ? 1 : 0;
    lastRemoteCommandTime = millis();

    markFeedback(success);

    Serial.print("Execution result: ");
    Serial.println(pumpResult);

    Serial.print("Success: ");
    Serial.println(success?"YES":"NO");

    Serial.print("Pump state: ");
    Serial.println(pumpOn?"ON":"OFF");
  }

  // Poll Node B
  if (millis() - lastPumpPoll >= 3000) {

    lastPumpPoll = millis();
    pollPump();
  }

  // Automatic pump control
  automaticControl();

  // Monitoring Channel 1
  if (mqtt.connected() && millis() - lastPublish >= MQTT_INTERVAL) {

    lastPublish = millis();
    publishTelemetry();
  }

  // Pump Control Channel 2
  publishPumpFeedback();

  delay(2);

  static unsigned long lastDebug = 0;

  if (millis() - lastDebug >= 2000) {
    lastDebug = millis();

    Serial.print("AP clients: ");
    Serial.print(WiFi.softAPgetStationNum());

    Serial.print(" | AP channel: ");
    Serial.print(WiFi.channel());

    Serial.print(" | STA connected: ");
    Serial.print(WiFi.status() == WL_CONNECTED);

    Serial.print(" | MQTT: ");
    Serial.println(mqtt.connected());
  }
}
