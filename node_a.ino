#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <math.h>

// ==========================================
// 1. WIFI AND GATEWAY
// ==========================================

const char* SSID = "FloodGateway";
const char* PASS = "Flood12345";

const char* GATEWAY = "http://192.168.4.1/api/sensors/water";

// ==========================================
// 2. HARDWARE
// ==========================================

#define TRIG_PIN 4
#define ECHO_PIN 5

#define LED_PIN 18
#define LED_COUNT 8

const float SENSOR_HEIGHT_CM = 10.5;

// ==========================================
// 3. FLOOD THRESHOLDS
// ==========================================

const float WARNING_LEVEL = 5.0;
const float CRITICAL_LEVEL = 8.0;

// ==========================================
// 4. OBJECTS AND TIMERS
// ==========================================

Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

unsigned long lastSend = 0;
unsigned long lastWiFiRetry = 0;

const unsigned long SEND_INTERVAL = 2000;

float lastDepth = 0;

// ==========================================
// 5. LED FUNCTIONS
// ==========================================

void showColor(uint32_t color) {
  for (int i = 0; i < LED_COUNT; i++) {
    strip.setPixelColor(i, color);
  }

  strip.show();
}

void updateFloodLED(float depth) {

  if (depth >= CRITICAL_LEVEL) {

    // RED: CRITICAL
    showColor(strip.Color(255, 0, 0));

    Serial.println("LED: RED - CRITICAL");

  } else if (depth >= WARNING_LEVEL) {

    // YELLOW: WARNING
    showColor(strip.Color(255, 170, 0));

    Serial.println("LED: YELLOW - WARNING");

  } else {

    // GREEN: NORMAL
    showColor(strip.Color(0, 255, 0));

    Serial.println("LED: GREEN - NORMAL");
  }
}

// ==========================================
// 6. ULTRASONIC SENSOR
// ==========================================

float readDistance() {

  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);

  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  unsigned long duration = pulseIn(ECHO_PIN, HIGH, 30000UL);

  if (duration == 0)
    return -1;

  return duration * 0.0343 / 2.0;
}

// ==========================================
// 7. WIFI CONNECTION
// ==========================================

void connectWiFi() {

  if (WiFi.status() == WL_CONNECTED)
    return;

  Serial.println("Connecting to gateway...");

  WiFi.begin(SSID, PASS);
}

// ==========================================
// 8. SETUP
// ==========================================

void setup() {

  Serial.begin(115200);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  digitalWrite(TRIG_PIN, LOW);

  // NeoPixel initialization
  strip.begin();
  strip.setBrightness(25);
  strip.clear();
  strip.show();

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  connectWiFi();

  Serial.println("Node A Started");
  Serial.println("Ultrasonic + NeoPixel Enabled");
}

// ==========================================
// 9. MAIN LOOP
// ==========================================

void loop() {

  // Wi-Fi reconnection
  if (WiFi.status() != WL_CONNECTED) {

    if (millis() - lastWiFiRetry >= 5000) {
      lastWiFiRetry = millis();
      connectWiFi();
    }
  }

  if (millis() - lastSend < SEND_INTERVAL)
    return;

  lastSend = millis();

  float distance = readDistance();

  // Invalid sensor reading
  if (distance < 0 || distance > SENSOR_HEIGHT_CM || !isfinite(distance)) {
    Serial.println("Invalid ultrasonic reading");

    // OFF indicates sensor measurement unavailable
    strip.clear();
    strip.show();

    return;
  }

  float depth = SENSOR_HEIGHT_CM - distance;

  if (depth < 0)
    depth = 0;

  lastDepth = depth;

  // Update LEDs locally, even if gateway is offline
  updateFloodLED(depth);

  Serial.print("Water depth: ");
  Serial.print(depth);
  Serial.println(" cm");

  // Send measurement to gateway
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected - LED still active");
    return;
  }

  JsonDocument doc;

  doc["node_id"] = "node_a";
  doc["water_depth_cm"] = depth;

  String payload;
  serializeJson(doc, payload);

  HTTPClient http;

  http.begin(GATEWAY);
  http.setTimeout(3000);
  http.setConnectTimeout(1500);

  http.addHeader("Content-Type", "application/json");

  int code = http.POST(payload);

  Serial.print("HTTP: ");
  Serial.println(code);

  http.end();
}