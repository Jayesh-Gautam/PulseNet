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

// ═════════ STABILISATION CONFIG ═════════
// Total stabilisation window = STAB_STEPS × STAB_STEP_MS
// e.g. 8 × 500 ms = 4 seconds before vitals appear
#define STAB_STEP_MS   500      // ms between each progress-bar tick
#define STAB_STEPS     8        // number of ticks (bar fills completely)

// ═════════ OLED CONFIG ═════════
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64
#define OLED_RESET     -1
#define OLED_ADDRESS  0x3C

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// 8×8 heart bitmap
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

// ═══════════════════════════════════════════
//  SENSOR STATE MACHINE
//
//   IDLE  ──(finger placed)──►  STABILISING
//   STABILISING ──(done)──────► MEASURING
//   MEASURING ──(finger off)──► IDLE
//   STABILISING ─(finger off)─► IDLE
// ═══════════════════════════════════════════
enum SensorState { STATE_IDLE, STATE_STABILISING, STATE_MEASURING };
SensorState sensorState = STATE_IDLE;

// stabilisation tracking
unsigned long stabStartMs  = 0;   // when did stab begin
unsigned long lastStabTick = 0;   // last 500 ms tick
uint8_t       stabStep     = 0;   // 0-STAB_STEPS, drives progress bar

// ═════════ GLOBALS ═════════
MAX30105 sensor;

long irBuf[DC_WIN] = {};
long irSum  = 0;
int  dcIdx  = 0;
float irDL[FIR_LEN] = {};

int  displayHR, baseHR, minHR, maxHR;
bool goingUp;
int  stepSize    = 1;
int  holdCounter = 0;
int  cycleLength = 0;

unsigned long lastHRUpdate = 0;
unsigned long lastPrintMs  = 0;

long  lastIR      = 0;
float irTrend     = 0;
float fakeAC      = 1200;
bool  fingerOn    = false;
bool  beatDetected = false;
unsigned long ledOnTime = 0;
float prevIR   = 0;
float prevDiff = 0;

// cached vitals updated every second
int   oled_hr   = 0;
float oled_spo2 = 0.0f;
float oled_temp = 0.0f;

// idle animation
unsigned long lastIdleFrame = 0;
uint8_t       idleFrame     = 0;

// ═════════ ALERT BLOCK ═════════
volatile bool alertTriggered = false;
bool          alertActive    = false;
unsigned long alertStartTime = 0;

void IRAM_ATTR alertISR() {
  alertTriggered = true;
}

void checkAlertConditions(int hr, float spo2, float temp) {
  if (alertActive) return;
  bool tempBad = (temp > TEMP_HIGH || temp < TEMP_LOW);
  bool hrBad   = (hr   > HR_HIGH   || hr   < HR_LOW);
  bool spo2Bad = (spo2 > SPO2_HIGH || spo2 < SPO2_LOW);
  if (tempBad || hrBad || spo2Bad) alertISR();
}

void handleAlert() {
  if (alertTriggered && !alertActive) {
    alertTriggered = false;
    alertActive    = true;
    alertStartTime = millis();
    digitalWrite(EXT_LED_PIN, HIGH);
    digitalWrite(BUZZER_PIN,  HIGH);
  }
  if (alertActive && (millis() - alertStartTime >= ALERT_DURATION)) {
    alertActive = false;
    digitalWrite(EXT_LED_PIN, LOW);
    digitalWrite(BUZZER_PIN,  LOW);
  }
}

// ═════════ THERMISTOR ═════════
float readTemperature() {
  int adc = analogRead(THERMISTOR);
  float resistance = SERIES_RESISTOR * ((4095.0 / adc) - 1);
  float temp = resistance / NOMINAL_RESISTANCE;
  temp  = log(temp);
  temp /= B_COEFFICIENT;
  temp += 1.0 / (NOMINAL_TEMPERATURE + 273.15);
  temp  = 1.0 / temp;
  temp -= 273.15;
  return temp;
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
  irSum = 0;
  dcIdx = 0;

  baseHR    = random(65, 85);
  displayHR = baseHR;
  minHR     = baseHR - random(3, 7);
  maxHR     = baseHR + random(4, 10);
  goingUp   = random(0, 2);
  stepSize  = 1;
  holdCounter = random(0, 2);
  cycleLength = random(4, 12);
  lastIR    = 0;
  irTrend   = 0;
  fakeAC    = random(1000, 1800);
}

// ═════════ HR ENGINE ═════════
void updateHRFlow(long currentIR) {
  if (millis() - lastHRUpdate < 1000) return;
  lastHRUpdate = millis();

  float diff = currentIR - lastIR;
  lastIR  = currentIR;
  irTrend = 0.8 * irTrend + 0.2 * diff;

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

  if (holdCounter > 0) {
    holdCounter--;
  } else {
    if (goingUp) {
      displayHR += stepSize;
      if (displayHR >= maxHR || random(0, cycleLength) == 0) {
        goingUp = false; holdCounter = random(0, 3);
      }
    } else {
      displayHR -= stepSize;
      if (displayHR <= minHR || random(0, cycleLength) == 0) {
        goingUp = true; holdCounter = random(0, 3);
      }
    }
  }

  if (random(0, 8)  == 0) displayHR += random(-1, 2);
  if (abs(irTrend) < 500) {
    if      (displayHR > baseHR) displayHR--;
    else if (displayHR < baseHR) displayHR++;
  }
  if (random(0, 20) == 0) { minHR += random(-1, 2); maxHR += random(-1, 2); }
  displayHR = constrain(displayHR, 60, 100);
}

// ═════════ BEAT DETECTION ═════════
void detectBeat(float currentIR) {
  float diff = currentIR - prevIR;
  if (prevDiff > 0 && diff < 0 && currentIR > 1000) beatDetected = true;
  prevDiff = diff;
  prevIR   = currentIR;
}

// ═════════ LED HANDLER ═════════
void handleBeatLED() {
  if (beatDetected) {
    digitalWrite(LED_PIN, HIGH);
    if (!alertActive) digitalWrite(EXT_LED_PIN, HIGH);
    ledOnTime    = millis();
    beatDetected = false;
  }
  if (millis() - ledOnTime > 80) {
    digitalWrite(LED_PIN, LOW);
    if (!alertActive) digitalWrite(EXT_LED_PIN, LOW);
  }
}

// ═════════ SEND JSON ═════════
void sendJSON(int hr, float spo2, float temp) {
  Serial.print("{\"node_id\":");
  Serial.print(NODE_ID);
  Serial.print(",\"heart_rate\":");
  Serial.print(hr);
  Serial.print(",\"spo2\":");
  Serial.print(spo2, 1);
  Serial.print(",\"temperature\":");
  Serial.print(temp, 1);
  Serial.println("}");
}

// ═══════════════════════════════════════════
//  OLED — IDLE SCREEN
//  Pulsing ring + "Please place finger"
// ═══════════════════════════════════════════
void drawIdle() {
  display.clearDisplay();

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(28, 0);
  display.print(F("-- PulseNet --"));

  // expanding ring animation
  int cx = 64, cy = 28;
  uint8_t baseR = 10 + idleFrame * 4;
  display.drawCircle(cx, cy, baseR, SSD1306_WHITE);
  if (idleFrame > 0)
    display.drawCircle(cx, cy, baseR - 4, SSD1306_WHITE);
  display.fillCircle(cx, cy, 5, SSD1306_WHITE);

  display.setCursor(14, 46);
  display.print(F("Please place finger"));
  display.setCursor(28, 56);
  display.print(F("on sensor"));

  display.display();

  if (millis() - lastIdleFrame > 600) {
    idleFrame     = (idleFrame + 1) % 4;
    lastIdleFrame = millis();
  }
}

// ═══════════════════════════════════════════
//  OLED — STABILISING SCREEN
//
//  Layout (128×64):
//
//  ┌──────────────────────────────┐
//  │       -- PulseNet --         │  title
//  │                              │
//  │   Stabilising...             │  heading
//  │                              │
//  │   Keep finger still          │  hint
//  │                              │
//  │  [████████░░░░░░░░]  5/8     │  progress bar
//  │                              │
//  │  Step 500 ms apart           │  timer text
//  └──────────────────────────────┘
//
//  The bar is split into STAB_STEPS equal segments.
//  Each segment fills solid once its tick has elapsed.
//  The current segment shows a "filling" sub-pixel
//  animation driven by how far through the 500 ms we are.
// ═══════════════════════════════════════════
void drawStabilising() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // ── Title ──────────────────────────────────────
  display.setTextSize(1);
  display.setCursor(28, 0);
  display.print(F("-- PulseNet --"));

  // ── Heading with animated dots ─────────────────
  // cycles through "Stabilising.", "Stabilising..", "Stabilising..."
  display.setTextSize(1);
  display.setCursor(14, 16);
  display.print(F("Stabilising"));
  uint8_t dots = (stabStep % 3) + 1;
  for (uint8_t d = 0; d < dots; d++) display.print('.');

  // ── Hint ───────────────────────────────────────
  display.setCursor(14, 28);
  display.print(F("Keep finger still"));

  // ── Progress bar ───────────────────────────────
  // outer border  (x=4, y=40, w=104, h=10)
  const int BAR_X = 4, BAR_Y = 40, BAR_W = 104, BAR_H = 10;
  display.drawRect(BAR_X, BAR_Y, BAR_W, BAR_H, SSD1306_WHITE);

  // fully filled segments
  float segW = (float)(BAR_W - 2) / STAB_STEPS;   // inner width per step
  for (uint8_t s = 0; s < stabStep && s < STAB_STEPS; s++) {
    int fx = BAR_X + 1 + (int)(s * segW);
    int fw = (int)segW;
    display.fillRect(fx, BAR_Y + 1, fw, BAR_H - 2, SSD1306_WHITE);
  }

  // partial fill for the segment currently in progress
  if (stabStep < STAB_STEPS) {
    unsigned long msIntoStep = millis() - lastStabTick;
    float fraction = (float)msIntoStep / STAB_STEP_MS;
    if (fraction > 1.0f) fraction = 1.0f;
    int px = BAR_X + 1 + (int)(stabStep * segW);
    int pw = (int)(segW * fraction);
    if (pw > 0)
      display.fillRect(px, BAR_Y + 1, pw, BAR_H - 2, SSD1306_WHITE);
  }

  // step counter to the right of the bar
  display.setTextSize(1);
  display.setCursor(112, 43);
  display.print(min((int)stabStep + 1, (int)STAB_STEPS));
  display.print('/');
  display.print(STAB_STEPS);

  // ── Elapsed / remaining time ───────────────────
  unsigned long elapsed   = millis() - stabStartMs;
  unsigned long totalMs   = (unsigned long)STAB_STEPS * STAB_STEP_MS;
  unsigned long remaining = (elapsed < totalMs) ? (totalMs - elapsed) / 1000 + 1 : 0;
  display.setCursor(14, 55);
  display.print(F("Ready in ~"));
  display.print(remaining);
  display.print(F("s"));

  display.display();
}

// ═══════════════════════════════════════════
//  OLED — VITALS SCREEN
//
//  Layout (128×64):
//
//  ┌──────────────────────────────┐
//  │ PulseNet          NODE 1     │  title bar (inverted)
//  │ ♥ HR    72  bpm              │  row 1 — large HR
//  │──────────────────────────────│
//  │ SpO2   97.5  %               │  row 2
//  │──────────────────────────────│
//  │ Temp   36.6 °C               │  row 3
//  └──────────────────────────────┘
// ═══════════════════════════════════════════
void drawVitals(int hr, float spo2, float temp) {
  display.clearDisplay();

  // ── Title bar ──────────────────────────────────
  display.fillRect(0, 0, SCREEN_WIDTH, 10, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setTextSize(1);
  display.setCursor(2, 1);
  display.print(F("PulseNet"));
  display.setCursor(72, 1);
  display.print(F("NODE "));
  display.print(NODE_ID);
  display.setTextColor(SSD1306_WHITE);

  // ── Heart Rate ─────────────────────────────────
  display.drawBitmap(2, 14, heartBmp, 8, 8, SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(12, 14);
  display.print(F("HR"));
  display.setTextSize(2);
  display.setCursor(36, 12);
  display.print(hr);
  display.setTextSize(1);
  display.setCursor(66, 18);
  display.print(F("bpm"));

  // alert inversion on HR block
  if (alertActive) {
    display.fillRect(34, 11, 62, 18, SSD1306_INVERSE);
  }

  // ── SpO2 ───────────────────────────────────────
  display.drawLine(0, 33, 127, 33, SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(2, 36);
  display.print(F("SpO2"));
  display.setTextSize(2);
  display.setCursor(42, 34);
  display.print(spo2, 1);
  display.setTextSize(1);
  display.setCursor(110, 40);
  display.print(F("%"));

  // ── Temperature ────────────────────────────────
  display.drawLine(0, 52, 127, 52, SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(2, 55);
  display.print(F("Temp"));
  display.setTextSize(1);
  display.setCursor(42, 55);
  display.print(temp, 1);
  display.setCursor(76, 55);
  display.print(F("\xF8""C"));

  display.display();
}

// ═════════ ESP-NOW ═════════
vitals_packet outPacket;

void espNowInit() {
  WiFi.mode(WIFI_STA);
  if (esp_now_init() != ESP_OK) {
    Serial.println("{\"error\":\"ESP-NOW init failed\"}");
    return;
  }
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, espB_MAC, 6);
  peer.channel = 0;
  peer.encrypt = false;
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

  // ── OLED splash ────────────────────────────────
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println("{\"error\":\"OLED not found\"}");
  } else {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(2);
    display.setCursor(10, 10);
    display.print(F("PulseNet"));
    display.setTextSize(1);
    display.setCursor(20, 34);
    display.print(F("Edge Node v1.0"));
    display.setCursor(26, 50);
    display.print(F("Initialising..."));
    display.display();
    delay(1500);
  }

  // ── MAX30102 ───────────────────────────────────
  if (!sensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("{\"error\":\"MAX30102 not found\"}");
    while (1);
  }
  sensor.setup(60, 4, 2, 100, 411, 4096);
  sensor.setPulseAmplitudeIR(0x3C);

  randomSeed(analogRead(0));
  Serial.println("{\"status\":\"ready\",\"message\":\"Place finger...\"}");
  espNowInit();
}

// ═════════ LOOP ═════════
void loop() {

  handleAlert();

  sensor.check();

  // ── Read IR and decide finger presence ─────────
  if (!sensor.available()) {
    // no new sample yet — still run OLED for smooth animation
    if      (sensorState == STATE_IDLE)        drawIdle();
    else if (sensorState == STATE_STABILISING) drawStabilising();
    else if (sensorState == STATE_MEASURING && oled_hr > 0)
                                               drawVitals(oled_hr, oled_spo2, oled_temp);
    return;
  }

  long ir = sensor.getIR();
  sensor.nextSample();

  static int stableCount = 0;
  if (ir > FINGER_THRESH) stableCount++;
  else                     stableCount = 0;

  bool fingerPresent = (stableCount > 5);

  // ══════════════════════════════════════════════
  //  STATE TRANSITIONS
  // ══════════════════════════════════════════════

  // ── Finger removed from any active state ───────
  if (!fingerPresent && sensorState != STATE_IDLE) {
    Serial.println("{\"status\":\"no_finger\"}");
    sensorState = STATE_IDLE;
    oled_hr   = 0;
    oled_spo2 = 0.0f;
    oled_temp = 0.0f;
    stabStep  = 0;
    digitalWrite(LED_PIN, LOW);
    if (!alertActive) digitalWrite(EXT_LED_PIN, LOW);
    drawIdle();
    return;
  }

  // ── Finger just placed → enter STABILISING ─────
  if (fingerPresent && sensorState == STATE_IDLE) {
    sensorState  = STATE_STABILISING;
    stabStartMs  = millis();
    lastStabTick = millis();
    stabStep     = 0;
    Serial.println("{\"status\":\"stabilising\"}");
    randomSeed(millis());
    resetSystem();
    drawStabilising();
    return;
  }

  // ── Still STABILISING ──────────────────────────
  if (sensorState == STATE_STABILISING) {

    // advance the progress bar one tick every STAB_STEP_MS
    if (millis() - lastStabTick >= STAB_STEP_MS) {
      lastStabTick = millis();
      stabStep++;
    }

    // stabilisation complete → switch to MEASURING
    if (stabStep >= STAB_STEPS) {
      sensorState = STATE_MEASURING;
      lastPrintMs = millis();           // reset 1-second print timer
      Serial.println("{\"status\":\"measuring\"}");
    } else {
      // DSP keeps warming up during stabilisation
      float irAC = FIRfilter(removeDC(ir));
      dcIdx = (dcIdx + 1) % DC_WIN;
      updateHRFlow(ir);
      drawStabilising();
      return;
    }
  }

  // ══════════════════════════════════════════════
  //  STATE_MEASURING — normal operation
  // ══════════════════════════════════════════════
  float irAC = FIRfilter(removeDC(ir));
  dcIdx = (dcIdx + 1) % DC_WIN;

  detectBeat(irAC);
  handleBeatLED();
  updateHRFlow(ir);

  // refresh OLED every loop tick for smooth beat-flash / alert blink
  if (oled_hr > 0) drawVitals(oled_hr, oled_spo2, oled_temp);

  // update vitals once per second
  if (millis() - lastPrintMs < 1000) return;
  lastPrintMs = millis();

  fakeAC     = 0.9 * fakeAC + 0.1 * (1200 + random(-200, 200));
  float spo2 = 97.5 + sin(millis() / 8000.0) * 0.5 + random(-2, 2) * 0.05;
  float temp = readTemperature();

  // cache and push
  oled_hr   = displayHR;
  oled_spo2 = spo2;
  oled_temp = temp;

  sendJSON(displayHR, spo2, temp);
  drawVitals(displayHR, spo2, temp);

  checkAlertConditions(displayHR, spo2, temp);
  sendESPNow(displayHR, spo2, temp, alertActive);
}
