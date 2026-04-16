/*
 * ============================================
 * PulseNet — Main Node (ESP B)  [FIXED]
 * ============================================
 */
#include <esp_now.h>
#include <WiFi.h>

typedef struct vitals_packet {
  int   node_id;
  int   heart_rate;
  float spo2;
  float temperature;
  bool  alert_active;
} vitals_packet;

vitals_packet inPacket;

// ── Works with both old and new ESP32 Arduino core ──────────────────
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
void onDataReceive(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
#else
void onDataReceive(const uint8_t *mac_addr, const uint8_t *data, int len) {
#endif
  if (len != sizeof(vitals_packet)) {
    Serial.printf("{\"error\":\"bad_packet_len\",\"got\":%d,\"want\":%d}\n",
                  len, (int)sizeof(vitals_packet));
    return;
  }
  memcpy(&inPacket, data, len);

  Serial.print(F("{\"node_id\":"));        Serial.print(inPacket.node_id);
  Serial.print(F(",\"heart_rate\":"));     Serial.print(inPacket.heart_rate);
  Serial.print(F(",\"spo2\":"));           Serial.print(inPacket.spo2, 1);
  Serial.print(F(",\"temperature\":"));    Serial.print(inPacket.temperature, 1);
  Serial.print(F(",\"alert\":"));          Serial.print(inPacket.alert_active ? "true" : "false");
  Serial.println(F("}"));
}

void setup() {
  Serial.begin(115200);
  delay(500);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  // ★ Print this node's MAC so you can paste it into ESP A
  Serial.print(F("{\"status\":\"mac\",\"espB_MAC\":\""));
  Serial.print(WiFi.macAddress());
  Serial.println(F("\"}"));

  if (esp_now_init() != ESP_OK) {
    Serial.println(F("{\"error\":\"ESP-NOW init failed\"}"));
    while (1);
  }

  esp_now_register_recv_cb(onDataReceive);
  Serial.println(F("{\"status\":\"ready\",\"message\":\"Waiting for ESP A...\"}"));
}

void loop() { /* all work in callback */ }
