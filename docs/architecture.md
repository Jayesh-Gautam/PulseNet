# PulseNet — System Architecture

## Overview

PulseNet is a real-time patient vitals monitoring system designed for hospital environments. 
It uses a distributed IoT architecture with ESP32 microcontrollers communicating via ESP-NOW, 
and a Python-based dashboard with ML-powered anomaly detection running on a connected PC.

---

## System Diagram

```
   ┌──────────────────────────────────────────────────────────────────────┐
   │                        HOSPITAL NETWORK                             │
   │                                                                      │
   │  ┌─────────────┐   ┌─────────────┐   ┌─────────────┐               │
   │  │  Edge Node 1 │   │  Edge Node 2 │   │  Edge Node N │               │
   │  │   (ESP32)    │   │   (ESP32)    │   │   (ESP32)    │               │
   │  │              │   │              │   │              │               │
   │  │  MAX30102    │   │  MAX30102    │   │  MAX30102    │               │
   │  │  NTC Sensor  │   │  NTC Sensor  │   │  NTC Sensor  │               │
   │  │  OLED Display│   │  OLED Display│   │  OLED Display│               │
   │  └──────┬───────┘   └──────┬───────┘   └──────┬───────┘               │
   │         │                  │                   │                      │
   │         │       ESP-NOW (Wireless P2P)         │                      │
   │         └──────────────────┼───────────────────┘                      │
   │                            │                                          │
   │                    ┌───────▼────────┐                                 │
   │                    │   Main Node    │                                 │
   │                    │   (ESP32)      │                                 │
   │                    │                │                                 │
   │                    │  • Receives    │                                 │
   │                    │    all data    │                                 │
   │                    │  • Buzzer      │                                 │
   │                    │    alarm       │                                 │
   │                    └───────┬────────┘                                 │
   │                            │ USB Serial (JSON)                        │
   │                    ┌───────▼────────┐                                 │
   │                    │   PC/Laptop    │                                 │
   │                    │                │                                 │
   │                    │  ┌──────────┐  │                                 │
   │                    │  │ FastAPI  │  │                                 │
   │                    │  │ Server   │  │                                 │
   │                    │  └────┬─────┘  │                                 │
   │                    │       │        │                                 │
   │                    │  ┌────▼─────┐  │                                 │
   │                    │  │ SQLite   │  │                                 │
   │                    │  │ Database │  │                                 │
   │                    │  └────┬─────┘  │                                 │
   │                    │       │        │                                 │
   │                    │  ┌────▼─────┐  │                                 │
   │                    │  │ ML Engine│  │                                 │
   │                    │  │ (sklearn)│  │                                 │
   │                    │  └──────────┘  │                                 │
   │                    └────────────────┘                                 │
   └──────────────────────────────────────────────────────────────────────┘
```

---

## Communication Protocol

### ESP-NOW Payload (Edge → Main)

```c
typedef struct __attribute__((packed)) {
    uint8_t   node_id;       // 1 byte  — Unique edge node ID
    float     heart_rate;    // 4 bytes — BPM
    float     spo2;          // 4 bytes — SpO2 percentage
    float     temperature;   // 4 bytes — Celsius
} SensorData;               // Total: 13 bytes
```

### Serial Output (Main → PC)

JSON format, one object per line:
```json
{"node_id":1,"heart_rate":72.5,"spo2":98.0,"temperature":36.8}
```

---

## Hardware Components

### Edge Node
| Component      | Model         | Interface | Purpose                    |
|---------------|---------------|-----------|----------------------------|
| MCU           | ESP32 DevKit  | —         | Processing + ESP-NOW TX    |
| Pulse Oximeter| MAX30102      | I2C       | Heart rate + SpO2          |
| Temp Sensor   | NTC 10K       | ADC       | Body temperature           |
| Display       | SSD1306 OLED  | I2C       | Local vitals display       |

### Main Node
| Component     | Model         | Interface | Purpose                    |
|--------------|---------------|-----------|----------------------------|
| MCU          | ESP32 DevKit  | —         | ESP-NOW RX + Serial TX     |
| Buzzer       | Passive Buzzer| GPIO      | Staff alert alarm          |

---

## Software Stack (Dashboard)

| Layer              | Technology              | Purpose                        |
|-------------------|-------------------------|--------------------------------|
| Web Server        | FastAPI + Uvicorn       | REST API + WebSocket server    |
| Real-time Comm    | WebSocket               | Push vitals to browser         |
| Serial Reader     | PySerial                | Read ESP32 data via USB        |
| Database          | SQLite                  | Persist all vitals + alerts    |
| ML Engine         | scikit-learn            | Isolation Forest anomaly det.  |
| Frontend          | HTML + CSS + Chart.js   | Real-time dashboard UI         |
| Templating        | Jinja2                  | Server-side HTML rendering     |

---

## ML Anomaly Detection

**Algorithm**: Isolation Forest (unsupervised)

**Features used**:
- `heart_rate` (BPM)
- `spo2` (%)
- `temperature` (°C)

**Behavior**:
1. Collects initial data (min 50 readings)
2. Trains Isolation Forest model automatically
3. Predicts on every incoming reading
4. Retrains every 300 new data points
5. Saves model to disk for persistence

**Output per prediction**:
- `is_anomaly`: boolean
- `anomaly_score`: float (-1 to 1, lower = more anomalous)
- `confidence`: "high" / "medium" / "low"

---

## Alert System

### Threshold-based (immediate)
Triggers buzzer on Main Node + logs in dashboard when vitals exceed safe ranges.

### ML-based (pattern detection)
Detects subtle multi-variable anomalies that threshold checks alone would miss, 
such as a gradual decline pattern or unusual combinations of vital signs.
