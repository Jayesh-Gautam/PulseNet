/*
 * ============================================
 *  PulseNet — Edge Node Firmware
 * ============================================
 *  Hardware:
 *    - ESP32 Dev Module
 *    - MAX30102 (Heart Rate + SpO2)
 *    - NTC Thermistor (Temperature)
 *    - SSD1306 OLED Display (128x64)
 *
 *  Communication: ESP-NOW (TX to Main Node)
 *
 *  Data Sent:
 *    { node_id, heart_rate, spo2, temperature }
 * ============================================
 */

// ─── INCLUDES ────────────────────────────────────────
#include <WiFi.h>
#include <esp_now.h>
#include <Wire.h>
// #include "MAX30105.h"           // SparkFun MAX30102 library
// #include <Adafruit_GFX.h>       // OLED graphics
// #include <Adafruit_SSD1306.h>   // OLED driver

// ─── CONFIGURATION ──────────────────────────────────
#define NODE_ID           1           // Unique ID for this edge node
#define SEND_INTERVAL_MS  1000        // Data send interval (ms)
#define NTC_PIN           34          // Analog pin for NTC thermistor
#define OLED_WIDTH        128
#define OLED_HEIGHT       64
#define OLED_RESET        -1
#define OLED_ADDR         0x3C

// NTC Thermistor calibration constants
#define NTC_NOMINAL_R     10000       // Resistance at 25°C
#define NTC_NOMINAL_TEMP  25          // Temperature for nominal resistance
#define NTC_B_COEFFICIENT 3950        // Beta coefficient
#define NTC_SERIES_R      10000       // Series resistor value

// ─── MAIN NODE MAC ADDRESS ──────────────────────────
// TODO: Replace with your main node's MAC address
uint8_t mainNodeMAC[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ─── DATA STRUCTURE ─────────────────────────────────
typedef struct __attribute__((packed)) {
  uint8_t   node_id;
  float     heart_rate;
  float     spo2;
  float     temperature;
} SensorData;

SensorData sensorData;

// ─── SENSOR OBJECTS ─────────────────────────────────
// MAX30105 pulseSensor;
// Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RESET);

// ─── ESP-NOW CALLBACK ───────────────────────────────
void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  Serial.print("Send Status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
}

// ─── READ TEMPERATURE (NTC) ─────────────────────────
float readTemperature() {
  // TODO: Implement NTC thermistor reading
  // int adcValue = analogRead(NTC_PIN);
  // float resistance = NTC_SERIES_R / ((4095.0 / adcValue) - 1.0);
  // float steinhart = resistance / NTC_NOMINAL_R;
  // steinhart = log(steinhart);
  // steinhart /= NTC_B_COEFFICIENT;
  // steinhart += 1.0 / (NTC_NOMINAL_TEMP + 273.15);
  // steinhart = 1.0 / steinhart;
  // steinhart -= 273.15;
  // return steinhart;

  return 0.0;  // Placeholder
}

// ─── READ HEART RATE & SPO2 ─────────────────────────
void readPulseOximeter(float &hr, float &sp) {
  // TODO: Implement MAX30102 reading
  // Use the SparkFun MAX30105 library's built-in algorithms
  // or implement your own peak detection

  hr = 0.0;  // Placeholder
  sp = 0.0;  // Placeholder
}

// ─── UPDATE OLED DISPLAY ────────────────────────────
void updateDisplay(float hr, float spo2, float temp) {
  // TODO: Implement OLED display update
  // display.clearDisplay();
  // display.setTextSize(1);
  // display.setTextColor(SSD1306_WHITE);
  //
  // display.setCursor(0, 0);
  // display.print("Node: "); display.println(NODE_ID);
  //
  // display.setCursor(0, 16);
  // display.print("HR:   "); display.print(hr, 1); display.println(" bpm");
  //
  // display.setCursor(0, 32);
  // display.print("SpO2: "); display.print(spo2, 1); display.println(" %");
  //
  // display.setCursor(0, 48);
  // display.print("Temp: "); display.print(temp, 1); display.println(" C");
  //
  // display.display();
}

// ─── SETUP ──────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("\n[PulseNet] Edge Node Starting...");
  Serial.print("[PulseNet] Node ID: ");
  Serial.println(NODE_ID);

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
  esp_now_register_send_cb(onDataSent);

  // --- Register Main Node as Peer ---
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, mainNodeMAC, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("[PulseNet] Failed to add peer!");
    ESP.restart();
  }
  Serial.println("[PulseNet] Peer registered.");

  // --- Initialize Sensors ---
  // TODO: Initialize MAX30102
  // if (!pulseSensor.begin(Wire, I2C_SPEED_FAST)) {
  //   Serial.println("[PulseNet] MAX30102 not found!");
  //   while (1);
  // }
  // pulseSensor.setup();

  // TODO: Initialize OLED
  // if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
  //   Serial.println("[PulseNet] OLED init failed!");
  //   while (1);
  // }
  // display.clearDisplay();
  // display.display();

  // --- Initialize NTC Pin ---
  // analogReadResolution(12);
  // pinMode(NTC_PIN, INPUT);

  sensorData.node_id = NODE_ID;
  Serial.println("[PulseNet] Edge Node Ready!\n");
}

// ─── LOOP ───────────────────────────────────────────
void loop() {
  static unsigned long lastSend = 0;

  if (millis() - lastSend >= SEND_INTERVAL_MS) {
    lastSend = millis();

    // --- Read Sensors ---
    float hr, sp;
    readPulseOximeter(hr, sp);
    float temp = readTemperature();

    sensorData.heart_rate   = hr;
    sensorData.spo2         = sp;
    sensorData.temperature  = temp;

    // --- Update OLED ---
    updateDisplay(hr, sp, temp);

    // --- Send via ESP-NOW ---
    esp_err_t result = esp_now_send(mainNodeMAC, (uint8_t *)&sensorData, sizeof(sensorData));

    // --- Debug Print ---
    Serial.printf("[TX] Node=%d | HR=%.1f | SpO2=%.1f | Temp=%.1f | %s\n",
                  sensorData.node_id,
                  sensorData.heart_rate,
                  sensorData.spo2,
                  sensorData.temperature,
                  result == ESP_OK ? "SENT" : "FAIL");
  }
}
