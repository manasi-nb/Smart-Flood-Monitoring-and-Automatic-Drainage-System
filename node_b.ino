#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ArduinoJson.h>

// ==========================================
// 1. WIFI
// ==========================================

const char* SSID = "FloodGateway";
const char* PASS = "Flood12345";

const char* GATEWAY_URL = "http://192.168.4.1/api/sensors/moisture";

IPAddress localIP(192,168,4,20);
IPAddress gatewayIP(192,168,4,1);
IPAddress subnet(255,255,255,0);

// ==========================================
// 2. HARDWARE
// ==========================================

#define MOISTURE_PIN 6
#define RELAY_PIN 7

const bool RELAY_ACTIVE_LOW = true;
const bool ENABLE_PHYSICAL_RELAY = true;

// ==========================================
// 3. MOISTURE CALIBRATION
// ==========================================

const int DRY_ADC = 2500;
const int WET_ADC = 2200;

// ==========================================
// 4. TIMING
// ==========================================

const unsigned long MAX_RUNTIME = 20000;
const unsigned long SEND_INTERVAL = 2000;
const unsigned long GATEWAY_TIMEOUT = 8000;

// ==========================================
// 5. OBJECTS AND STATE
// ==========================================

WebServer server(80);

enum PumpMode { MANUAL_MODE, AUTOMATIC_MODE };
PumpMode pumpMode = MANUAL_MODE;

bool pumpOn = false;
bool timedOut = false;

int moistureRaw = 0;
int moisturePercent = 0;

String groundCondition = "UNKNOWN";

unsigned long pumpStart = 0;
unsigned long lastSend = 0;
unsigned long lastRetry = 0;
unsigned long lastGatewayOK = 0;

String lastCommandID = "";
String lastResult = "READY";

// ==========================================
// 6. MOISTURE CALCULATION
// ==========================================

int calculateMoisturePercent(int raw) {

  if (DRY_ADC == WET_ADC)
    return 0;

  float percentage = 100.0f * (raw - DRY_ADC) / (float)(WET_ADC - DRY_ADC);

  return constrain((int)roundf(percentage), 0, 100);
}

String calculateGroundCondition(int percent) {

  if (percent >= 70)
    return "SATURATED";

  if (percent >= 40)
    return "DAMP";

  return "DRY";
}

// ==========================================
// 7. RELAY CONTROL
// ==========================================

void driveRelay(bool on) {

  if (!ENABLE_PHYSICAL_RELAY)
    return;

  int level;

  if (RELAY_ACTIVE_LOW)
    level = on ? LOW : HIGH;
  else
    level = on ? HIGH : LOW;

  digitalWrite(RELAY_PIN, level);
}

void setPump(bool on) {

  driveRelay(on);
  pumpOn = on;

  if (on) {
    pumpStart = millis();
    Serial.println("PUMP ON COMMAND");
  } else {
    Serial.println("PUMP OFF COMMAND");
  }
}

// ==========================================
// 8. JSON HELPER
// ==========================================

void sendJSON(int status, JsonDocument& doc) {

  String output;
  serializeJson(doc, output);

  server.send(status, "application/json", output);
}

// ==========================================
// 9. GET PUMP STATUS
// ==========================================

void getPumpStatus() {

  JsonDocument doc;

  doc["node_id"] = "node_b";
  doc["pump_on"] = pumpOn;
  doc["pump_mode"] = pumpMode == AUTOMATIC_MODE ? "AUTO" : "MANUAL";

  // Legacy compatibility with current Gateway C
  doc["simulation"] = true;

  doc["relay_enabled"] = ENABLE_PHYSICAL_RELAY;
  doc["timed_out"] = timedOut;

  doc["last_result"] = lastResult;
  doc["last_command_id"] = lastCommandID;

  doc["moisture_raw"] = moistureRaw;
  doc["moisture_pct"] = moisturePercent;
  doc["ground_condition"] = groundCondition;

  doc["runtime_ms"] = pumpOn ? millis() - pumpStart : 0;

  sendJSON(200, doc);
}

// ==========================================
// 10. RECEIVE PUMP COMMAND
// ==========================================

void handlePumpCommand() {

  JsonDocument doc;

  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"success\":false,\"error\":\"Invalid JSON\"}");
    return;
  }

  if (!doc["on"].is<bool>()) {
    server.send(400, "application/json", "{\"success\":false,\"error\":\"Invalid ON field\"}");
    return;
  }

  String id = doc["command_id"] | "";
  bool requested = doc["on"].as<bool>();

  // Gateway explicitly identifies the mode for every pump command.
  // OFF is always accepted, including during communication problems.
  String requestedMode = doc["mode"] | "MANUAL";
  if (requestedMode != "MANUAL" && requestedMode != "AUTO") {
    server.send(400, "application/json", "{\"success\":false,\"error\":\"Invalid pump mode\"}");
    return;
  }

  if (id.length() == 0 || id.length() > 64) {
    server.send(400, "application/json", "{\"success\":false,\"error\":\"Invalid command ID\"}");
    return;
  }

  // Reject ON when gateway communication is stale
  if (requested && (lastGatewayOK == 0 || millis() - lastGatewayOK > GATEWAY_TIMEOUT)) {
    server.send(409, "application/json", "{\"success\":false,\"error\":\"Gateway unavailable\"}");
    return;
  }

  // Update the operating mode without restarting the runtime timer.
  // An already-running pump keeps its original start timestamp.
  if (requested) {
    PumpMode newMode = requestedMode == "AUTO" ? AUTOMATIC_MODE : MANUAL_MODE;
    // When switching from AUTO to MANUAL while running, start the 20-second manual allowance at the moment of the switch.
    if (pumpOn && pumpMode == AUTOMATIC_MODE && newMode == MANUAL_MODE)
      pumpStart = millis();
    pumpMode = newMode;
  }

  // Prevent restarting the runtime timer
  if (requested && !pumpOn) {

    timedOut = false;
    setPump(true);
    lastResult = "PUMP_STARTED";

  } else if (!requested) {

    setPump(false);
    lastResult = "PUMP_STOPPED";

  } else {

    lastResult = "ALREADY_ON";
  }

  lastCommandID = id;

  JsonDocument response;

  response["success"] = true;
  response["command_id"] = id;
  response["pump_on"] = pumpOn;
  response["pump_mode"] = pumpMode == AUTOMATIC_MODE ? "AUTO" : "MANUAL";

  // Legacy gateway compatibility
  response["simulation"] = true;

  response["relay_enabled"] = ENABLE_PHYSICAL_RELAY;
  response["timed_out"] = timedOut;
  response["message"] = lastResult;

  sendJSON(200, response);
}

// ==========================================
// 11. RESET PUMP
// ==========================================

void resetPump() {

  setPump(false);
  pumpMode = MANUAL_MODE;

  timedOut = false;
  lastResult = "RESET_COMPLETE";

  JsonDocument doc;

  doc["success"] = true;
  doc["pump_on"] = false;
  doc["timed_out"] = false;

  doc["simulation"] = true;
  doc["relay_enabled"] = ENABLE_PHYSICAL_RELAY;

  doc["message"] = lastResult;

  sendJSON(200, doc);
}

// ==========================================
// 12. SEND MOISTURE TO GATEWAY
// ==========================================

void sendMoisture() {

  if (WiFi.status() != WL_CONNECTED)
    return;

  moistureRaw = analogRead(MOISTURE_PIN);

  moisturePercent = calculateMoisturePercent(moistureRaw);

  groundCondition = calculateGroundCondition(moisturePercent);

  JsonDocument doc;

  doc["node_id"] = "node_b";
  doc["moisture_raw"] = moistureRaw;

  // New fields
  doc["moisture_pct"] = moisturePercent;
  doc["ground_condition"] = groundCondition;

  String payload;
  serializeJson(doc, payload);

  HTTPClient http;

  http.begin(GATEWAY_URL);
  http.setTimeout(3000);
  http.setConnectTimeout(1500);

  http.addHeader("Content-Type", "application/json");

  int code = http.POST(payload);

  http.end();

  if (code == 200)
    lastGatewayOK = millis();

  Serial.print("Moisture ADC: ");
  Serial.print(moistureRaw);

  Serial.print(" | Moisture: ");
  Serial.print(moisturePercent);
  Serial.print("% | ");

  Serial.print(groundCondition);

  Serial.print(" | HTTP: ");
  Serial.println(code);
}

// ==========================================
// 13. PUMP SAFETY
// ==========================================

void checkSafety() {

  if (!pumpOn)
    return;

  if (pumpMode == MANUAL_MODE && millis() - pumpStart >= MAX_RUNTIME) {

    setPump(false);
    timedOut = true;
    lastResult = "TIMEOUT";

    Serial.println("PUMP STOPPED: TIMEOUT");

    return;
  }

  if (WiFi.status() != WL_CONNECTED || lastGatewayOK == 0 || millis() - lastGatewayOK > GATEWAY_TIMEOUT) {

    setPump(false);
    lastResult = "COMMUNICATION_LOST";

    Serial.println("PUMP STOPPED: COMMUNICATION LOST");
  }
}

// ==========================================
// 14. SETUP
// ==========================================

void setup() {

  Serial.begin(115200);

  int offLevel = RELAY_ACTIVE_LOW ? HIGH : LOW;

  digitalWrite(RELAY_PIN, offLevel);
  pinMode(RELAY_PIN, OUTPUT);

  driveRelay(false);

  analogReadResolution(12);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  WiFi.config(localIP, gatewayIP, subnet);

  WiFi.begin(SSID, PASS);

  server.on("/api/pump", HTTP_GET, getPumpStatus);

  server.on("/api/pump", HTTP_POST, handlePumpCommand);

  server.on("/api/pump/reset", HTTP_POST, resetPump);

  server.on("/api/health", HTTP_GET, []() {
    server.send(200, "application/json", "{\"node\":\"B\",\"status\":\"online\"}");
  });

  server.begin();

  Serial.println("Node B Started");

  Serial.println(ENABLE_PHYSICAL_RELAY ? "PHYSICAL RELAY ENABLED" : "RELAY DISABLED");
}

// ==========================================
// 15. MAIN LOOP
// ==========================================

void loop() {

  server.handleClient();

  checkSafety();

  if (WiFi.status() != WL_CONNECTED && millis() - lastRetry >= 5000) {
    lastRetry = millis();
    WiFi.reconnect();
  }

  if (millis() - lastSend >= SEND_INTERVAL) {
    lastSend = millis();
    sendMoisture();
  }

  delay(2);
}