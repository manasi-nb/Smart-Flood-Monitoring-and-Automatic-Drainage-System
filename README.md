# Smart Flood Monitoring and Automatic Drainage System

**EE4216 Mini-Project - Bidirectional IoT Network**

A three-ESP32-S3 prototype that monitors water depth and soil moisture, displays flood warnings, and controls a miniature drainage pump. Two local sensor nodes communicate with an ESP32-S3 gateway using **HTTP**. The gateway provides a live local dashboard and uses **MQTT** to publish measurements to ThingSpeak, receive remote pump commands, and publish execution feedback.

## Features

- **Water monitoring:** HC-SR04 measures water depth; an 8-pixel NeoPixel strip indicates normal (green), warning (yellow), or critical (red).
- **Ground monitoring:** Capacitive moisture sensor reports raw ADC, calibrated percentage, and DRY/DAMP/SATURATED classification.
- **Automatic drainage:** Relay-controlled submersible pump starts at critical water depth and stops at the lower cutoff.
- **Manual control:** Local dashboard or ThingSpeak commands can start/stop the pump, select automatic mode, or reset it.
- **Local monitoring:** Gateway-hosted dashboard refreshes with JavaScript `fetch()` every 2 seconds, showing readings, connectivity, risk, and pump status.
- **Cloud integration:** MQTT telemetry, remote control, and best-effort execution feedback through two ThingSpeak channels.
- **Basic fault handling:** Sensor freshness checks, reconnect attempts, a Node B communication watchdog, and a manual pump runtime limit.

## Architecture

```text
Node A: HC-SR04 + NeoPixel ── HTTP POST ──┐
                                          │
Node B: Moisture + Relay + Pump ── HTTP ──┤
                                          ▼
                                 ESP32-S3 Gateway C
                                 Wi-Fi AP + STA
                                 HTTP dashboard :80
                                          │
                                Internet Wi-Fi hotspot
                                          │ MQTT/TCP :1883
                                          ▼
                                ThingSpeak MQTT broker
                                  ├─ Channel 1: telemetry
                                  └─ Channel 2: commands/feedback

Remote command: ThingSpeak → MQTT subscribe → Gateway C
                → HTTP POST → Node B → HTTP JSON acknowledgment
                → Gateway C → MQTT publish → ThingSpeak
```

Gateway C performs **application-layer forwarding**. The local sensor nodes do not connect directly to ThingSpeak, and the hotspot does not route the local sensor network.

## Materials

| Item | Quantity | Purpose |
|---|---:|---|
| ESP32-S3-DevKitC-1 (ESP32-S3-WROOM-2) | 3 | Nodes A, B and gateway C |
| HC-SR04 ultrasonic sensor | 1 | Water-level measurement |
| Capacitive soil moisture sensor | 1 | Ground wetness measurement |
| WS2812/NeoPixel RGB LED strip (8 LEDs) | 1 | Local flood warnings |
| CW-021 two-channel 5 V relay module | 1 | Pump switching (one channel used) |
| DC 3–5 V micro-submersible pump | 1 | Drainage |
| Water containers | 2 | Flood basin and collection reservoir |
| Pump tubing, jumper wires, breadboard/mounting materials | As needed | Assembly |
| Suitable USB power connections and verified 5 V power path | As needed | ESP32, relay and pump power |
| Laptop with Arduino IDE; Internet-connected Wi-Fi hotspot | 1 each | Programming and ThingSpeak access |

**Electrical safety:** Verify relay input compatibility with 3.3 V GPIO, motor startup current, USB supply capacity, and the development board's 5 V power path before connecting the motor. The HC-SR04 ECHO signal must be limited to a safe 3.3 V GPIO level if the sensor is operated at 5 V. A voltage divider can be used for ECHO; it does **not** solve motor-current limitations. Keep electronics dry and provide appropriate motor transient suppression. Do not operate the pump dry or unattended.

## Wiring reference

### Node A — Water sensor and LEDs

| ESP32-S3 A | Device connection |
|---|---|
| GPIO 4 | HC-SR04 TRIG |
| GPIO 5 | HC-SR04 ECHO **through suitable level reduction if 5 V** |
| GPIO 18 | NeoPixel DIN (use a compatible data interface for a 5 V strip) |
| GND | Sensor/LED common ground |
| Appropriate supply rail | Sensor and LED power, according to module ratings |

### Node B — Moisture sensor, relay and pump

| ESP32-S3 B | Device connection |
|---|---|
| GPIO 6 | Moisture sensor analog output |
| 3V3, GND | Moisture sensor power and ground (if sensor supports 3.3 V) |
| GPIO 7 | Relay IN1 |
| Verified 5 V and GND | Relay power (per module configuration) |

**Pump switching:** Verified pump-supply positive → relay Channel 1 **COM**; relay Channel 1 **NO** → pump positive; pump negative → pump-supply negative. NC is unused. A shared ground may be needed for the chosen relay driver. Confirm the CW-021 terminal labels on the actual board. Do not route unverified pump current through an ESP32 power pin.

### Gateway C

No external sensor pins are required. Connect it to USB power; it creates the local `FloodGateway` access point and connects as a station to the upstream Internet hotspot.

## Software requirements

- Arduino IDE with **ESP32 by Espressif Systems** board package; select the matching ESP32-S3-DevKitC-1 board/USB settings.
- Arduino libraries: **ArduinoJson v7**, **Adafruit NeoPixel**, **PubSubClient**.
- ESP32 core libraries: `WiFi.h`, `HTTPClient.h`, `WebServer.h`.
- ThingSpeak account, two channels, and MQTT credentials with appropriate channel permissions.

## Repository files

```text
.
├── node_a.ino    # Ultrasonic measurement, NeoPixel warnings, HTTP sensor upload
├── node_b.ino    # Moisture measurement, relay control, HTTP pump API
├── gateway.ino   # AP+STA gateway, local dashboard, MQTT and pump logic
└── README.md
```

If Arduino IDE requires a sketch folder, put each `.ino` file in a same-named folder (for example `node_a/node_a.ino`).

## Configuration and setup

1. **Create ThingSpeak channels:** Channel 1 for monitoring and Channel 2 for pump control. Create MQTT credentials and authorize access to both channels.
2. **Configure `gateway.ino`:** Set `STA_SSID`, `STA_PASS`, `MQTT_CLIENT_ID`, `MQTT_USERNAME`, `MQTT_PASSWORD`, `CHANNEL_ID` and `CONTROL_CHANNEL_ID`. The broker is `mqtt3.thingspeak.com`, TCP port **1883** (unencrypted, as in the prototype).
3. **Configure local Wi-Fi:** The supplied sketches use SSID `FloodGateway` and password `Flood12345`. Change this default password in all three sketches before public deployment.
4. **Check calibration:** Node A uses `SENSOR_HEIGHT_CM = 10.5` (the actual sensor-reference-to-bottom distance). Node B uses `DRY_ADC = 2500` and `WET_ADC = 2200` in the supplied code; measure and replace these values for your soil/probe.
5. **Check hardware settings:** Node B has `RELAY_ACTIVE_LOW = true` and `ENABLE_PHYSICAL_RELAY = true`. Confirm these match the verified relay interface. For initial testing, disconnect the motor or set physical relay control disabled.
6. **Upload firmware:** Flash `gateway.ino` to Gateway C, `node_a.ino` to Node A, and `node_b.ino` to Node B. Open Serial Monitor at **115200 baud** if debugging.
7. **Connect and verify:** Gateway C should expose `FloodGateway` at `192.168.4.1`. Node B uses static IP `192.168.4.20`; Node A obtains an address by DHCP. Confirm both report `HTTP: 200`.
8. **Open dashboard:** Connect a phone/laptop to `FloodGateway`, then visit **http://192.168.4.1/**. Verify sensor readings and node status before testing the pump.

**Never commit real Wi-Fi passwords, MQTT credentials, or ThingSpeak API keys to a public repository.** Use placeholders in the committed sketches and set private credentials locally. Rotate any credentials that were previously exposed.

## Flood thresholds and pump behavior

| Water depth | Flood state | NeoPixel color |
|---|---|---|
| Below 5 cm | NORMAL | Green |
| 5 cm to below 8 cm | WARNING | Yellow |
| 8 cm or higher | CRITICAL | Red |

Water depth is calculated as `SENSOR_HEIGHT_CM - measured_distance_cm`.

| Pump mode | Behavior |
|---|---|
| MANUAL | Manual ON/OFF; 20-second maximum continuous manual run |
| AUTO | Start at **8 cm** when armed; stop at **4 cm**; no 20-second automatic timeout |
| Both | Local safety checks and Node B's 8-second gateway-communication watchdog remain active |

The 8 cm ON / 4 cm OFF thresholds create **hysteresis**, reducing rapid relay switching. The gateway tracks node freshness with a 10-second timeout. The moisture percentage is supplementary monitoring; it is not an automatic pump trigger.

## HTTP API reference

### Gateway C — `http://192.168.4.1` (TCP port 80)

| Method | Path | Purpose |
|---|---|---|
| `GET` | `/` | HTML dashboard |
| `GET` | `/api/status` | Sensor, pump, node and MQTT status (JSON) |
| `POST` | `/api/sensors/water` | Receive Node A water measurement |
| `POST` | `/api/sensors/moisture` | Receive Node B moisture measurement |
| `POST` | `/api/pump/control` | Dashboard pump command |

Node A example:

```json
{"node_id":"node_a","water_depth_cm":6.5}
```

Node B example:

```json
{"node_id":"node_b","moisture_raw":2350,"moisture_pct":50,"ground_condition":"DAMP"}
```

Gateway C validates the explicit node IDs (`node_a`, `node_b`) and measurement ranges before storing readings. Accepted sensor POSTs return HTTP 200 with `{"status":"received"}`; invalid readings receive HTTP 400.

Local pump control body (POST `/api/pump/control`):

```json
{"command":1}
```

### Node B — `http://192.168.4.20` (TCP port 80)

| Method | Path | Purpose |
|---|---|---|
| `GET` | `/api/pump` | Report pump state, mode, moisture and last result |
| `POST` | `/api/pump` | Execute ON/OFF and synchronize pump mode |
| `POST` | `/api/pump/reset` | Stop pump and reset to MANUAL |
| `GET` | `/api/health` | Health check |

Gateway-to-Node-B command example:

```json
{"command_id":"gw-example-1","on":true,"mode":"MANUAL"}
```

Node B responds with an HTTP JSON acknowledgment containing `success`, `command_id`, `pump_on`, `pump_mode`, and `message` (plus compatibility/status fields). Gateway C validates the acknowledgment before treating the command as successful. A legacy `simulation:true` field remains in the firmware for compatibility even when the physical relay is enabled; it **does not mean** the pump is simulated.

**Pump commands can physically start the motor.** Only send them when the setup is safe.

## MQTT / ThingSpeak reference

- Broker: `mqtt3.thingspeak.com`
- Transport: **MQTT over TCP port 1883** (both publish and subscribe on the same MQTT connection)
- Sensor telemetry topic: `channels/<CHANNEL_ID>/publish`
- Remote command subscription: `channels/<CONTROL_CHANNEL_ID>/subscribe/fields/field1`
- Pump feedback topic: `channels/<CONTROL_CHANNEL_ID>/publish`
- Sensor telemetry interval: approximately **20 seconds**
- Pump feedback: change/periodic updates, subject to delays and ThingSpeak channel rate limits

### Channel 1 — Flood Monitoring

| Field | Meaning |
|---|---|
| 1 | Water level (cm) |
| 2 | Soil moisture (%) |
| 3 | Flood risk: 0=NORMAL, 1=WARNING, 2=CRITICAL, -1=UNKNOWN |
| 4 | Node A online: 0/1 |
| 5 | Node B online: 0/1 |

### Channel 2 — Pump Control

| Field | Meaning |
|---|---|
| 1 | Remote pump command (input) |
| 2 | Latest execution result: 1=accepted, 0=rejected, -1=none yet |
| 3 | Reported pump state: 1=ON, 0=OFF, -1=unknown |

| Command | Action |
|---|---|
| `0` | Manual OFF |
| `1` | Manual ON (subject to safety checks) |
| `2` | Select AUTO mode |
| `3` | Reset (OFF, return to MANUAL) |

Send a command to Channel 2 Field 1 using the ThingSpeak Write API (this **can activate the real pump**):

```text
https://api.thingspeak.com/update?api_key=YOUR_CHANNEL_2_WRITE_API_KEY&field1=1
```

A successful ThingSpeak write means the cloud stored the command, **not** that the actuator executed it. The gateway receives the command via MQTT subscription, forwards it by local HTTP, validates Node B's JSON acknowledgment, and later publishes Fields 2–3 using MQTT. Feedback is **best-effort/latest-state**, not a guaranteed per-command audit log. Field 1 is not automatically populated by local dashboard commands.

## Quick demo / verification

1. Power all three boards; confirm both nodes ONLINE at **http://192.168.4.1/**.
2. Show water level and soil moisture updating; check NeoPixel colors below 5 cm, between 5–8 cm, and at/above 8 cm.
3. Select AUTO; raise water depth to 8 cm; verify drainage starts and stops at 4 cm.
4. At a safe water level, test local Manual ON/OFF and Reset. Manual mode stops after 20 seconds if left ON.
5. Open ThingSpeak Channel 1; confirm MQTT telemetry graphs update.
6. Send a Channel 2 Field 1 command; observe the physical response and execution feedback in Fields 2–3. Respect ThingSpeak update intervals.
7. With pump OFF, disconnect/reconnect a sensor node and observe ONLINE/OFFLINE recovery. Optionally interrupt upstream Internet and confirm local monitoring remains available.

### Useful local links

- Dashboard: http://192.168.4.1/
- Gateway status: http://192.168.4.1/api/status
- Node B pump status: http://192.168.4.20/api/pump
- Node B health: http://192.168.4.20/api/health
- ThingSpeak: https://thingspeak.com/
Add your team member names, group number, contribution summary, and chosen license before publishing. No license is implied by this repository unless you add one.

