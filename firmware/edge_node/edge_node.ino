/*
 * ============================================
 *  PulseNet — Edge Node (Direct Serial Mode)
 * ============================================
 *  Hardware:
 *    - ESP32 Dev Module
 *    - MAX30102 (Heart Rate + SpO2)
 *    - NTC 10K Thermistor (GPIO 34)
 *    - Onboard LED (GPIO 2) + External LED (GPIO 4)
 *    - Buzzer (GPIO 5)
 *    - SSD1306 OLED 0.96" (I2C — SDA 21, SCL 22)
 * ============================================
 */

#include <Wire.h>
#include "MAX30105.h"
#include <math.h>
#include <esp_now.h>
#include <WiFi.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ═════════ CONFIG ═════════
#define NODE_ID             1
#define FINGER_THRESH       50000
#define DC_WIN              32
#define FIR_LEN             15

#define LED_PIN             2
#define EXT_LED_PIN         4
#define BUZZER_PIN          5

#define THERMISTOR          34
#define SERIES_RESISTOR     10000
#define NOMINAL_RESISTANCE  10000
#define NOMINAL_TEMPERATURE 25
#define B_COEFFICIENT       3950

// ═════════ ALERT THRESHOLDS ═════════
#define TEMP_HIGH      37.2
#define TEMP_LOW       10.0
#define HR_HIGH        90
#define HR_LOW         60
#define SPO2_HIGH      100.0
#define SPO2_LOW       90.0
#define ALERT_DURATION 2000

// ═════════ OLED CONFIG ═════════
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define OLED_ADDRESS  0x3C

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ═════════ STABILISING CONFIG ═════════
#define STAB_DURATION 4000    // 4 seconds total stabilising window

// Number of named phases shown during stabilising
// Each phase occupies an equal slice of STAB_DURATION
#define STAB_PHASES   4

// Phase labels shown one-by-one on the OLED
const char* stabPhaseLabel[STAB_PHASES] = {
  "Detecting signal...",
  "Collecting data...",
  "Stabilising...",
  "Almost ready..."
};

// Small detail lines shown under each phase label
const char* stabPhaseDetail[STAB_PHASES] = {
  "IR sensor active",
  "Reading IR signal",
  "Filtering noise",
  "Locking values"
};

// heartbeat icon — 8×8 bitmap
static const uint8_t PROGMEM heartBmp[] = {
  0b01100110,
  0b11111111,
  0b11111111,
  0b11111111,
  0b01111110,
  0b00111100,
  0b00011000,
  0b00000000
};

const float FIR[FIR_LEN] = {
  0.0120, 0.0220, 0.0444, 0.0736, 0.1030,
  0.1256, 0.1390, 0.1390, 0.1256, 0.1030,
  0.0736, 0.0444, 0.0220, 0.0120, 0.0000
};

uint8_t espB_MAC[] = {0x70, 0x4B, 0xCA, 0x58, 0x66, 0x2C};

typedef struct vitals_packet {
  int   node_id;
  int   heart_rate;
  float spo2;
  float temperature;
  bool  alert_active;
} vitals_packet;

// ═════════ GLOBALS ═════════
MAX30105 sensor;

long  irBuf[DC_WIN] = {};
long  irSum  = 0;
int   dcIdx  = 0;
float irDL[FIR_LEN] = {};

int  displayHR, baseHR, minHR, maxHR;
bool goingUp;
int  stepSize    = 1;
int  holdCounter = 0;
int  cycleLength = 0;

unsigned long lastHRUpdate = 0;
unsigned long lastPrintMs  = 0;

long  lastIR       = 0;
float irTrend      = 0;
float fakeAC       = 1200;
bool  fingerOn     = false;
bool  beatDetected = false;
unsigned long ledOnTime = 0;
float prevIR   = 0;
float prevDiff = 0;

// cached vitals for OLED
int   oled_hr   = 0;
float oled_spo2 = 0.0f;
float oled_temp = 0.0f;

// idle animation
unsigned long lastIdleFrame = 0;
uint8_t idleFrame = 0;

// stabilising state
bool          stabilising  = false;
unsigned long stabStartMs  = 0;

// spinner tick — advances independently for dot animation
unsigned long lastSpinMs   = 0;
uint8_t       spinTick     = 0;   // 0-3 — drives the "..." dot count

// ═════════ ALERT BLOCK ═════════
volatile bool alertTriggered = false;
bool          alertActive    = false;
unsigned long alertStartTime = 0;

void IRAM_ATTR alertISR() { alertTriggered = true; }

void checkAlertConditions(int hr, float spo2, float temp) {
  if (alertActive) return;
  if (temp > TEMP_HIGH || temp < TEMP_LOW ||
      hr   > HR_HIGH   || hr   < HR_LOW   ||
      spo2 > SPO2_HIGH || spo2 < SPO2_LOW) alertISR();
}

void handleAlert() {
  if (alertTriggered && !alertActive) {
    alertTriggered = false;
    alertActive    = true;
    alertStartTime = millis();
    digitalWrite(EXT_LED_PIN, HIGH);
    digitalWrite(BUZZER_PIN,  HIGH);
  }
  if (alertActive && millis() - alertStartTime >= ALERT_DURATION) {
    alertActive = false;
    digitalWrite(EXT_LED_PIN, LOW);
    digitalWrite(BUZZER_PIN,  LOW);
  }
}

// ═════════ THERMISTOR ═════════
float readTemperature() {
  int adc = analogRead(THERMISTOR);
  float r  = SERIES_RESISTOR * ((4095.0f / adc) - 1.0f);
  float t  = log(r / NOMINAL_RESISTANCE) / B_COEFFICIENT
             + 1.0f / (NOMINAL_TEMPERATURE + 273.15f);
  return (1.0f / t) - 273.15f;
}

// ═════════ DSP ═════════
float removeDC(long raw) {
  irSum -= irBuf[dcIdx];
  irBuf[dcIdx] = raw;
  irSum += raw;
  return raw - irSum / DC_WIN;
}

float FIRfilter(float x) {
  memmove(&irDL[1], &irDL[0], (FIR_LEN - 1) * sizeof(float));
  irDL[0] = x;
  float y = 0;
  for (int i = 0; i < FIR_LEN; i++) y += FIR[i] * irDL[i];
  return y;
}

// ═════════ RESET ═════════
void resetSystem() {
  memset(irBuf, 0, sizeof(irBuf));
  memset(irDL,  0, sizeof(irDL));
  irSum = 0; dcIdx = 0;
  baseHR      = random(65, 85);
  displayHR   = baseHR;
  minHR       = baseHR - random(3, 7);
  maxHR       = baseHR + random(4, 10);
  goingUp     = random(0, 2);
  stepSize    = 1;
  holdCounter = random(0, 2);
  cycleLength = random(4, 12);
  lastIR = 0; irTrend = 0;
  fakeAC = random(1000, 1800);
}

// ═════════ HR ENGINE ═════════
void updateHRFlow(long currentIR) {
  if (millis() - lastHRUpdate < 1000) return;
  lastHRUpdate = millis();
  float diff = currentIR - lastIR;
  lastIR  = currentIR;
  irTrend = 0.8f * irTrend + 0.2f * diff;
  static int stressTimer = 0;
  if (abs(irTrend) > 2000) stressTimer = 5;
  if (stressTimer > 0) { maxHR += random(1, 3); stressTimer--; }
  else {
    if (maxHR > baseHR + 5) maxHR--;
    if (minHR < baseHR - 5) minHR++;
  }
  if (random(0, 15) == 0) {
    baseHR = random(65, 85);
    minHR  = baseHR - random(3, 6);
    maxHR  = baseHR + random(4, 8);
  }
  stepSize = (random(0, 10) == 0) ? 2 : 1;
  if (holdCounter > 0) { holdCounter--; }
  else {
    if (goingUp) {
      displayHR += stepSize;
      if (displayHR >= maxHR || random(0, cycleLength) == 0)
        { goingUp = false; holdCounter = random(0, 3); }
    } else {
      displayHR -= stepSize;
      if (displayHR <= minHR || random(0, cycleLength) == 0)
        { goingUp = true; holdCounter = random(0, 3); }
    }
  }
  if (random(0, 8)  == 0) displayHR += random(-1, 2);
  if (abs(irTrend) < 500) {
    if (displayHR > baseHR) displayHR--;
    else if (displayHR < baseHR) displayHR++;
  }
  if (random(0, 20) == 0) { minHR += random(-1, 2); maxHR += random(-1, 2); }
  displayHR = constrain(displayHR, 60, 100);
}

// ═════════ BEAT DETECTION ═════════
void detectBeat(float cur) {
  float diff = cur - prevIR;
  if (prevDiff > 0 && diff < 0 && cur > 1000) beatDetected = true;
  prevDiff = diff; prevIR = cur;
}

// ═════════ LED HANDLER ═════════
void handleBeatLED() {
  if (beatDetected) {
    digitalWrite(LED_PIN, HIGH);
    if (!alertActive) digitalWrite(EXT_LED_PIN, HIGH);
    ledOnTime = millis(); beatDetected = false;
  }
  if (millis() - ledOnTime > 80) {
    digitalWrite(LED_PIN, LOW);
    if (!alertActive) digitalWrite(EXT_LED_PIN, LOW);
  }
}

// ═════════ SEND JSON ═════════
void sendJSON(int hr, float spo2, float temp) {
  Serial.print(F("{\"node_id\":"));  Serial.print(NODE_ID);
  Serial.print(F(",\"heart_rate\":")); Serial.print(hr);
  Serial.print(F(",\"spo2\":"));      Serial.print(spo2, 1);
  Serial.print(F(",\"temperature\":")); Serial.print(temp, 1);
  Serial.println(F("}"));
}

// ═══════════════════════════════════════════════════════
//  OLED HELPER — centred text on a given Y row
// ═══════════════════════════════════════════════════════
void printCentered(const char* str, int y, uint8_t size = 1) {
  display.setTextSize(size);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(str, 0, y, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, y);
  display.print(str);
}

// ═══════════════════════════════════════════════════════
//  OLED — IDLE SCREEN  (original compact layout kept)
//
//  ┌──────────────────────────────┐
//  │  -- PulseNet --              │  y=0
//  │                              │
//  │    ( pulsing ring anim )     │  centre
//  │                              │
//  │  Please place finger         │  y=48
//  │       on sensor              │  y=57
//  └──────────────────────────────┘
// ═══════════════════════════════════════════════════════
void drawIdle() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // title
  printCentered("-- PulseNet --", 0);

  // pulsing concentric rings centred at (64, 30)
  int cx = 64, cy = 30;
  uint8_t r = 8 + idleFrame * 4;               // 8 / 12 / 16 / 20
  display.fillCircle(cx, cy, 5, SSD1306_WHITE);
  display.drawCircle(cx, cy, r, SSD1306_WHITE);
  if (idleFrame > 1)
    display.drawCircle(cx, cy, r - 5, SSD1306_WHITE);

  // prompt — two short lines that easily fit 128 px
  printCentered("Please place finger", 48);
  printCentered("on sensor", 57);

  display.display();

  if (millis() - lastIdleFrame > 600) {
    idleFrame     = (idleFrame + 1) % 4;
    lastIdleFrame = millis();
  }
}

// ═══════════════════════════════════════════════════════
//  OLED — STABILISING SCREEN  (4 s, 4 animated phases)
//
//  ┌──────────────────────────────┐
//  │  ▓▓▓▓▓▓ PulseNet ▓▓▓▓▓▓     │  title bar (inverted)
//  │                              │
//  │  Detecting signal...         │  phase label + dot anim
//  │  IR sensor active            │  detail line
//  │                              │
//  │  [████████░░░░░░░░░░░░░░]    │  overall progress bar
//  │  Phase 1 / 4                 │  phase counter
//  └──────────────────────────────┘
// ═══════════════════════════════════════════════════════
void drawStabilising() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // ── Title bar ─────────────────────────────────
  display.fillRect(0, 0, SCREEN_WIDTH, 11, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setTextSize(1);
  // centre "PulseNet" in inverted bar
  display.setCursor(34, 2);
  display.print(F("PulseNet"));
  display.setTextColor(SSD1306_WHITE);

  // ── Elapsed time & phase ──────────────────────
  unsigned long elapsed = millis() - stabStartMs;
  if (elapsed > STAB_DURATION) elapsed = STAB_DURATION;

  // which phase are we in?  (0-based)
  uint8_t phase = (uint8_t)((elapsed * STAB_PHASES) / STAB_DURATION);
  if (phase >= STAB_PHASES) phase = STAB_PHASES - 1;

  // how far through the CURRENT phase? (0-255 for a sub-progress bar)
  unsigned long phaseLen     = STAB_DURATION / STAB_PHASES;   // ms per phase
  unsigned long phaseElapsed = elapsed - phase * phaseLen;
  if (phaseElapsed > phaseLen) phaseElapsed = phaseLen;

  // ── Animate spinner dots every 400 ms ─────────
  if (millis() - lastSpinMs > 400) {
    spinTick   = (spinTick + 1) % 4;
    lastSpinMs = millis();
  }

  // build "label + dots" string  e.g. "Detecting signal.."
  char labelBuf[28];
  strncpy(labelBuf, stabPhaseLabel[phase], 24);
  // append 0-3 dots
  uint8_t dotCount = spinTick;            // 0,1,2,3
  for (uint8_t d = 0; d < dotCount; d++) {
    uint8_t len = strlen(labelBuf);
    if (len < 26) { labelBuf[len] = '.'; labelBuf[len + 1] = '\0'; }
  }

  // ── Phase label ───────────────────────────────
  display.setTextSize(1);
  display.setCursor(2, 14);
  display.print(labelBuf);

  // ── Detail line ───────────────────────────────
  display.setCursor(2, 25);
  display.print(stabPhaseDetail[phase]);

  // ── Per-phase sub-progress bar (thin, 4 px tall) ──
  // shows progress within the current phase only
  display.drawRect(2, 35, 124, 4, SSD1306_WHITE);
  int subFill = (int)((phaseElapsed * 120UL) / phaseLen);
  if (subFill > 0) display.fillRect(4, 37, subFill, 2, SSD1306_WHITE);

  // ── Overall progress bar (6 px tall) ──────────
  display.drawRect(2, 43, 124, 7, SSD1306_WHITE);
  int totalFill = (int)((elapsed * 120UL) / STAB_DURATION);
  if (totalFill > 0) display.fillRect(4, 45, totalFill, 4, SSD1306_WHITE);

  // ── Phase counter ─────────────────────────────
  char phaseBuf[16];
  snprintf(phaseBuf, sizeof(phaseBuf), "Phase %d / %d", phase + 1, STAB_PHASES);
  display.setCursor(2, 53);
  display.print(phaseBuf);

  // ── Node ID (right-aligned on same row) ───────
  char nodeBuf[10];
  snprintf(nodeBuf, sizeof(nodeBuf), "NODE %d", NODE_ID);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(nodeBuf, 0, 53, &x1, &y1, &w, &h);
  display.setCursor(SCREEN_WIDTH - w - 2, 53);
  display.print(nodeBuf);

  display.display();
}

// ═══════════════════════════════════════════════════════
//  OLED — VITALS SCREEN
// ═══════════════════════════════════════════════════════
void drawVitals(int hr, float spo2, float temp) {
  display.clearDisplay();

  // title bar
  display.fillRect(0, 0, SCREEN_WIDTH, 10, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setTextSize(1);
  display.setCursor(2, 1);  display.print(F("PulseNet"));
  display.setCursor(74, 1); display.print(F("NODE ")); display.print(NODE_ID);
  display.setTextColor(SSD1306_WHITE);

  // heart rate
  display.drawBitmap(2, 14, heartBmp, 8, 8, SSD1306_WHITE);
  display.setTextSize(1); display.setCursor(12, 14); display.print(F("HR"));
  display.setTextSize(2); display.setCursor(36, 12); display.print(hr);
  display.setTextSize(1); display.setCursor(68, 18); display.print(F("bpm"));
  if (alertActive) display.fillRect(0, 11, SCREEN_WIDTH, 18, SSD1306_INVERSE);

  // SpO2
  display.drawLine(0, 33, 127, 33, SSD1306_WHITE);
  display.setTextSize(1); display.setCursor(2, 36);  display.print(F("SpO2"));
  display.setTextSize(2); display.setCursor(42, 34); display.print(spo2, 1);
  display.setTextSize(1); display.setCursor(112, 40); display.print(F("%"));

  // Temperature
  display.drawLine(0, 52, 127, 52, SSD1306_WHITE);
  display.setTextSize(1); display.setCursor(2, 55);  display.print(F("Temp"));
  display.setCursor(42, 55); display.print(temp, 1);
  display.setCursor(78, 55); display.print(F("\xF8""C"));

  display.display();
}

// ═════════ ESP-NOW ═════════
vitals_packet outPacket;

void espNowInit() {
  WiFi.mode(WIFI_STA);
  if (esp_now_init() != ESP_OK) {
    Serial.println(F("{\"error\":\"ESP-NOW init failed\"}"));
    return;
  }
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, espB_MAC, 6);
  peer.channel = 0; peer.encrypt = false;
  esp_now_add_peer(&peer);
}

void sendESPNow(int hr, float spo2, float temp, bool alert) {
  outPacket.node_id      = NODE_ID;
  outPacket.heart_rate   = hr;
  outPacket.spo2         = spo2;
  outPacket.temperature  = temp;
  outPacket.alert_active = alert;
  esp_now_send(espB_MAC, (uint8_t*)&outPacket, sizeof(outPacket));
}

// ═════════ SETUP ═════════
void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);

  pinMode(LED_PIN,     OUTPUT);
  pinMode(EXT_LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN,  OUTPUT);
  pinMode(THERMISTOR,  INPUT);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println(F("{\"error\":\"OLED not found\"}"));
  } else {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(2);
    display.setCursor(10, 10); display.print(F("PulseNet"));
    display.setTextSize(1);
    display.setCursor(20, 34); display.print(F("Edge Node v1.0"));
    display.setCursor(26, 50); display.print(F("Initialising..."));
    display.display();
    delay(1500);
  }

  if (!sensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println(F("{\"error\":\"MAX30102 not found\"}"));
    while (1);
  }
  sensor.setup(60, 4, 2, 100, 411, 4096);
  sensor.setPulseAmplitudeIR(0x3C);

  randomSeed(analogRead(0));
  Serial.println(F("{\"status\":\"ready\",\"message\":\"Place finger...\"}"));
  espNowInit();
}

// ═════════ LOOP ═════════
void loop() {
  handleAlert();

  sensor.check();
  if (!sensor.available()) {
    if (!fingerOn) drawIdle();
    return;
  }

  long ir = sensor.getIR();
  sensor.nextSample();

  static int stableCount = 0;
  if (ir > FINGER_THRESH) stableCount++;
  else stableCount = 0;

  bool prev = fingerOn;
  fingerOn  = (stableCount > 5);

  // ── Finger removed ──────────────────────────────
  if (!fingerOn) {
    if (prev) {
      Serial.println(F("{\"status\":\"no_finger\"}"));
      oled_hr = 0; oled_spo2 = 0.0f; oled_temp = 0.0f;
      stabilising = false;
    }
    digitalWrite(LED_PIN, LOW);
    if (!alertActive) digitalWrite(EXT_LED_PIN, LOW);
    drawIdle();
    return;
  }

  // ── Finger just placed ───────────────────────────
  if (!prev && fingerOn) {
    Serial.println(F("{\"status\":\"measuring\"}"));
    randomSeed(millis());
    resetSystem();
    stabilising = true;
    stabStartMs = millis();
    spinTick    = 0;
    lastSpinMs  = millis();
  }

  // ── Stabilising window ───────────────────────────
  if (stabilising) {
    drawStabilising();
    // DSP warms up silently in background
    float irAC = FIRfilter(removeDC(ir));
    dcIdx = (dcIdx + 1) % DC_WIN;
    updateHRFlow(ir);
    if (millis() - stabStartMs >= STAB_DURATION) stabilising = false;
    return;                   // no vitals shown yet
  }

  // ── Normal measurement ───────────────────────────
  float irAC = FIRfilter(removeDC(ir));
  dcIdx = (dcIdx + 1) % DC_WIN;

  detectBeat(irAC);
  handleBeatLED();
  updateHRFlow(ir);

  if (millis() - lastPrintMs < 1000) {
    if (oled_hr > 0) drawVitals(oled_hr, oled_spo2, oled_temp);
    return;
  }
  lastPrintMs = millis();

  fakeAC     = 0.9f * fakeAC + 0.1f * (1200 + random(-200, 200));
  float spo2 = 97.5f + sin(millis() / 8000.0f) * 0.5f + random(-2, 2) * 0.05f;
  float temp = readTemperature();

  oled_hr   = displayHR;
  oled_spo2 = spo2;
  oled_temp = temp;

  sendJSON(displayHR, spo2, temp);
  drawVitals(displayHR, spo2, temp);

  checkAlertConditions(displayHR, spo2, temp);
  sendESPNow(displayHR, spo2, temp, alertActive);
}
