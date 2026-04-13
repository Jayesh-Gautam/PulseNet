/*
 * ============================================
 *  PulseNet — Main Node Firmware
 * ============================================
 *  Hardware:
 *    - ESP32 Dev Module
 *    - Passive Buzzer (alarm)
 *
 *  Communication:
 *    - ESP-NOW (RX from Edge Nodes)
 *    - USB Serial (TX to Dashboard on PC)
 *
 *  Receives:
 *    { node_id, heart_rate, spo2, temperature }
 *
 *  Serial Output Format (JSON):
 *    {"node_id":1,"heart_rate":72.5,"spo2":98.0,"temperature":36.8}
 * ============================================
 */

// ─── INCLUDES ────────────────────────────────────────
#include <WiFi.h>
#include <esp_now.h>
#include <ArduinoJson.h>  // Install: ArduinoJson by Benoit Blanchon

// ─── CONFIGURATION ──────────────────────────────────
#define BUZZER_PIN        25          // GPIO pin for buzzer
#define ALARM_DURATION_MS 3000        // Buzzer alarm duration (ms)
#define ALARM_FREQ        2000        // Buzzer tone frequency (Hz)
#define SERIAL_BAUD       115200      // Serial baud rate

// ─── VITAL SIGN THRESHOLDS ──────────────────────────
#define HR_LOW            50.0        // Heart rate low threshold (BPM)
#define HR_HIGH           120.0       // Heart rate high threshold (BPM)
#define SPO2_LOW          90.0        // SpO2 low threshold (%)
#define TEMP_LOW          35.0        // Temperature low threshold (°C)
#define TEMP_HIGH         38.5        // Temperature high threshold (°C)

// ─── DATA STRUCTURE (must match edge node) ──────────
typedef struct __attribute__((packed)) {
  uint8_t   node_id;
  float     heart_rate;
  float     spo2;
  float     temperature;
} SensorData;

// ─── STATE ──────────────────────────────────────────
volatile bool newDataReceived = false;
SensorData    latestData;
unsigned long alarmStartTime  = 0;
bool          alarmActive     = false;

// ─── CHECK THRESHOLDS & TRIGGER ALARM ───────────────
bool checkVitals(const SensorData &data) {
  bool abnormal = false;

  if (data.heart_rate < HR_LOW || data.heart_rate > HR_HIGH) {
    Serial.printf("[ALERT] Node %d: Abnormal Heart Rate = %.1f BPM\n",
                  data.node_id, data.heart_rate);
    abnormal = true;
  }

  if (data.spo2 < SPO2_LOW) {
    Serial.printf("[ALERT] Node %d: Low SpO2 = %.1f%%\n",
                  data.node_id, data.spo2);
    abnormal = true;
  }

  if (data.temperature < TEMP_LOW || data.temperature > TEMP_HIGH) {
    Serial.printf("[ALERT] Node %d: Abnormal Temperature = %.1f°C\n",
                  data.node_id, data.temperature);
    abnormal = true;
  }

  return abnormal;
}

// ─── BUZZER ALARM ───────────────────────────────────
void triggerAlarm() {
  // TODO: Implement buzzer alarm pattern
  // tone(BUZZER_PIN, ALARM_FREQ);
  alarmActive = true;
  alarmStartTime = millis();
  Serial.println("[ALARM] Buzzer activated!");
}

void stopAlarm() {
  // noTone(BUZZER_PIN);
  alarmActive = false;
  Serial.println("[ALARM] Buzzer deactivated.");
}

// ─── SEND DATA TO PC VIA SERIAL (JSON) ─────────────
void sendToPC(const SensorData &data) {
  StaticJsonDocument<256> doc;
  doc["node_id"]     = data.node_id;
  doc["heart_rate"]  = round(data.heart_rate * 10.0) / 10.0;
  doc["spo2"]        = round(data.spo2 * 10.0) / 10.0;
  doc["temperature"] = round(data.temperature * 10.0) / 10.0;

  serializeJson(doc, Serial);
  Serial.println();  // Newline delimiter for PC-side parsing
}

// ─── ESP-NOW RECEIVE CALLBACK ───────────────────────
void onDataReceived(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len == sizeof(SensorData)) {
    memcpy(&latestData, data, sizeof(SensorData));
    newDataReceived = true;
  } else {
    Serial.printf("[ERR] Unexpected payload size: %d (expected %d)\n",
                  len, sizeof(SensorData));
  }
}

// ─── SETUP ──────────────────────────────────────────
void setup() {
  Serial.begin(SERIAL_BAUD);
  Serial.println("\n[PulseNet] Main Node Starting...");

  // --- Buzzer Pin ---
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // --- WiFi (Station mode for ESP-NOW) ---
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  Serial.print("[PulseNet] MAC Address: ");
  Serial.println(WiFi.macAddress());

  // --- ESP-NOW Init ---
  if (esp_now_init() != ESP_OK) {
    Serial.println("[PulseNet] ESP-NOW Init FAILED!");
    ESP.restart();
  }
  esp_now_register_recv_cb(onDataReceived);

  Serial.println("[PulseNet] Main Node Ready! Waiting for data...\n");
}

// ─── LOOP ───────────────────────────────────────────
void loop() {
  // --- Process received data ---
  if (newDataReceived) {
    newDataReceived = false;

    Serial.printf("[RX] Node=%d | HR=%.1f | SpO2=%.1f | Temp=%.1f\n",
                  latestData.node_id,
                  latestData.heart_rate,
                  latestData.spo2,
                  latestData.temperature);

    // Send JSON to PC via Serial
    sendToPC(latestData);

    // Check thresholds and alarm
    if (checkVitals(latestData)) {
      triggerAlarm();
    }
  }

  // --- Auto-stop alarm after duration ---
  if (alarmActive && (millis() - alarmStartTime >= ALARM_DURATION_MS)) {
    stopAlarm();
  }
}
