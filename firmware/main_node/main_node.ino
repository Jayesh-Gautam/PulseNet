/* 
 * ============================================
 * PulseNet — Main Node (ESP B)
 * ============================================
 * Hardware:
 * - ESP32 Dev Module
 * Communication: ESP-NOW receive from ESP A
 * Output: Serial Monitor (JSON)
 * ============================================
 */

#include <esp_now.h>
#include <WiFi.h>

// ═════════ PACKET STRUCTURE ═════════
// must exactly match ESP A's struct
typedef struct vitals_packet {
  int node_id;
  int heart_rate;
  float spo2;
  float temperature;
  bool alert_active;
} vitals_packet;

vitals_packet inPacket;

// ═════════ CALLBACK — fires on every received packet ═════════
void onDataReceive(const esp_now_recv_info_t *info, const uint8_t *data, int len) {

  if (len != sizeof(vitals_packet)) return; // wrong packet size, ignore

  memcpy(&inPacket, data, sizeof(inPacket));

  // print as JSON to Serial Monitor
  Serial.print("{\"node_id\":");
  Serial.print(inPacket.node_id);

  Serial.print(",\"heart_rate\":");
  Serial.print(inPacket.heart_rate);

  Serial.print(",\"spo2\":");
  Serial.print(inPacket.spo2, 1);

  Serial.print(",\"temperature\":");
  Serial.print(inPacket.temperature, 1);

  Serial.print(",\"alert\":");
  Serial.print(inPacket.alert_active ? "true" : "false");

  Serial.println("}");
}

// ═════════ SETUP ═════════
void setup() {

  Serial.begin(115200);

  WiFi.mode(WIFI_STA);

  if (esp_now_init() != ESP_OK) {
    Serial.println("{\"error\":\"ESP-NOW init failed\"}");
    while (1);
  }

  esp_now_register_recv_cb(onDataReceive);

  Serial.println("{\"status\":\"ready\",\"message\":\"Waiting for ESP A...\"}");
}

// ═════════ LOOP ═════════
void loop() {
  // nothing here — all work done in onDataReceive callback
}
