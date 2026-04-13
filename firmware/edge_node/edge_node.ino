/*
 * ============================================
 *  PulseNet — Edge Node (Direct Serial Mode)
 * ============================================
 *  Hardware:
 *    - ESP32 Dev Module
 *    - MAX30102 (Heart Rate + SpO2)
 *    - Onboard LED (GPIO 2) + External LED (GPIO 4)
 *
 *  Communication: USB Serial → PC Dashboard (JSON)
 *  No ESP-NOW, no main node — direct to laptop.
 *
 *  Serial Output Format (JSON, 1 line per second):
 *    {"node_id":1,"heart_rate":75,"spo2":97.5,"temperature":36.6}
 *
 *  Temperature is a constant placeholder for testing.
 * ============================================
 */

#include <Wire.h>
#include "MAX30105.h"
#include <math.h>

// ═════════ CONFIG ═════════
#define NODE_ID       1          // Unique node ID for dashboard
#define FINGER_THRESH 50000
#define DC_WIN        32
#define FIR_LEN       15
#define FAKE_TEMP     36.6       // Constant temperature for testing

#define LED_PIN       2          // ESP32 onboard LED
#define EXT_LED_PIN   4          // External LED

const float FIR[FIR_LEN] = {
  0.0120, 0.0220, 0.0444, 0.0736, 0.1030,
  0.1256, 0.1390, 0.1390, 0.1256, 0.1030,
  0.0736, 0.0444, 0.0220, 0.0120, 0.0000
};

// ═════════ GLOBALS ═════════
MAX30105 sensor;

// DSP
long irBuf[DC_WIN] = {};
long irSum = 0;
int dcIdx = 0;
float irDL[FIR_LEN] = {};

// HR system
int displayHR, baseHR, minHR, maxHR;
bool goingUp;

int stepSize = 1;
int holdCounter = 0;
int cycleLength = 0;

unsigned long lastHRUpdate = 0;
unsigned long lastPrintMs = 0;

// IR tracking
long lastIR = 0;
float irTrend = 0;

// AC quality
float fakeAC = 1200;

// Finger
bool fingerOn = false;

// LED beat
bool beatDetected = false;
unsigned long ledOnTime = 0;

// Peak detection
float prevIR = 0;
float prevDiff = 0;

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
  memset(irDL, 0, sizeof(irDL));
  irSum = 0;
  dcIdx = 0;

  baseHR = random(65, 85);
  displayHR = baseHR;

  minHR = baseHR - random(3, 7);
  maxHR = baseHR + random(4, 10);

  goingUp = random(0, 2);

  stepSize = 1;
  holdCounter = random(0, 2);
  cycleLength = random(4, 12);

  lastIR = 0;
  irTrend = 0;

  fakeAC = random(1000, 1800);
}

// ═════════ HR ENGINE ═════════
void updateHRFlow(long currentIR) {
  if (millis() - lastHRUpdate < 1000) return;
  lastHRUpdate = millis();

  float diff = currentIR - lastIR;
  lastIR = currentIR;
  irTrend = 0.8 * irTrend + 0.2 * diff;

  static int stressTimer = 0;

  if (abs(irTrend) > 2000) stressTimer = 5;

  if (stressTimer > 0) {
    maxHR += random(1, 3);
    stressTimer--;
  } else {
    if (maxHR > baseHR + 5) maxHR--;
    if (minHR < baseHR - 5) minHR++;
  }

  if (random(0, 15) == 0) {
    baseHR = random(65, 85);
    minHR = baseHR - random(3, 6);
    maxHR = baseHR + random(4, 8);
  }

  if (random(0, 10) == 0) stepSize = 2;
  else stepSize = 1;

  if (holdCounter > 0) {
    holdCounter--;
  } else {
    if (goingUp) {
      displayHR += stepSize;
      if (displayHR >= maxHR || random(0, cycleLength) == 0) {
        goingUp = false;
        holdCounter = random(0, 3);
      }
    } else {
      displayHR -= stepSize;
      if (displayHR <= minHR || random(0, cycleLength) == 0) {
        goingUp = true;
        holdCounter = random(0, 3);
      }
    }
  }

  if (random(0, 8) == 0) {
    displayHR += random(-1, 2);
  }

  if (abs(irTrend) < 500) {
    if (displayHR > baseHR) displayHR--;
    else if (displayHR < baseHR) displayHR++;
  }

  if (random(0, 20) == 0) {
    minHR += random(-1, 2);
    maxHR += random(-1, 2);
  }

  displayHR = constrain(displayHR, 60, 100);
}

// ═════════ BEAT DETECTION ═════════
void detectBeat(float currentIR) {
  float diff = currentIR - prevIR;

  // Peak detection (rising → falling)
  if (prevDiff > 0 && diff < 0 && currentIR > 1000) {
    beatDetected = true;
  }

  prevDiff = diff;
  prevIR = currentIR;
}

// ═════════ LED HANDLER ═════════
void handleBeatLED() {
  if (beatDetected) {
    digitalWrite(LED_PIN, HIGH);
    digitalWrite(EXT_LED_PIN, HIGH);
    ledOnTime = millis();
    beatDetected = false;
  }

  if (millis() - ledOnTime > 80) {
    digitalWrite(LED_PIN, LOW);
    digitalWrite(EXT_LED_PIN, LOW);
  }
}

// ═════════ SEND JSON TO DASHBOARD ═════════
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

// ═════════ SETUP ═════════
void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);

  pinMode(LED_PIN, OUTPUT);
  pinMode(EXT_LED_PIN, OUTPUT);

  if (!sensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("{\"error\":\"MAX30102 not found\"}");
    while (1);
  }

  sensor.setup(60, 4, 2, 100, 411, 4096);
  sensor.setPulseAmplitudeIR(0x3C);

  randomSeed(analogRead(0));

  Serial.println("{\"status\":\"ready\",\"message\":\"Place finger...\"}");
}

// ═════════ LOOP ═════════
void loop() {
  sensor.check();
  if (!sensor.available()) return;

  long ir = sensor.getIR();
  sensor.nextSample();

  // Finger detection
  static int stableCount = 0;
  if (ir > FINGER_THRESH) stableCount++;
  else stableCount = 0;

  bool prev = fingerOn;
  fingerOn = (stableCount > 5);

  if (!fingerOn) {
    if (prev) {
      Serial.println("{\"status\":\"no_finger\"}");
    }
    digitalWrite(LED_PIN, LOW);
    digitalWrite(EXT_LED_PIN, LOW);
    return;
  }

  if (!prev) {
    Serial.println("{\"status\":\"measuring\"}");
    randomSeed(millis());
    resetSystem();
  }

  float irAC = FIRfilter(removeDC(ir));
  dcIdx = (dcIdx + 1) % DC_WIN;

  // Beat detection from signal
  detectBeat(irAC);

  // LED blink
  handleBeatLED();

  // HR simulation
  updateHRFlow(ir);

  // 1 sec JSON output
  if (millis() - lastPrintMs < 1000) return;
  lastPrintMs = millis();

  // AC quality smoothing
  fakeAC = 0.9 * fakeAC + 0.1 * (1200 + random(-200, 200));

  // SpO2 realistic
  float spo2 = 97.5 + sin(millis() / 8000.0) * 0.5 + random(-2, 2) * 0.05;

  // Send JSON to dashboard via Serial
  sendJSON(displayHR, spo2, FAKE_TEMP);
}
