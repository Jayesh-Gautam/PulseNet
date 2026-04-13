# 🫀 PulseNet — Real-Time Patient Vitals Monitoring System

A hospital-grade IoT patient monitoring system using ESP32 nodes, ESP-NOW communication, and an intelligent dashboard with ML-powered anomaly detection.

## 🏗️ Architecture

```
┌──────────────┐     ESP-NOW      ┌──────────────┐     USB Serial     ┌──────────────────────┐
│  Edge Node   │ ───────────────► │  Main Node   │ ──────────────────► │   Dashboard (PC)     │
│  (ESP32)     │                  │  (ESP32)     │                     │  FastAPI + ML Engine │
│              │                  │              │                     │                      │
│ • MAX30102   │                  │ • Buzzer     │                     │ • Real-time UI       │
│ • NTC Temp   │                  │ • Alarm      │                     │ • SQLite Logging     │
│ • OLED       │                  │              │                     │ • Anomaly Detection  │
└──────────────┘                  └──────────────┘                     └──────────────────────┘
     (x N nodes)
```

## 📁 Project Structure

```
PulseNet/
├── firmware/
│   ├── edge_node/          # ESP32 sensor node (MAX30102 + NTC + OLED)
│   │   └── edge_node.ino
│   └── main_node/          # ESP32 receiver node (ESP-NOW RX + Buzzer)
│       └── main_node.ino
├── dashboard/
│   ├── main.py             # FastAPI server entry point
│   ├── config.py           # Configuration & thresholds
│   ├── serial_reader.py    # PySerial data reader
│   ├── database.py         # SQLite data handler
│   ├── ml_engine.py        # Anomaly detection (Isolation Forest)
│   ├── requirements.txt    # Python dependencies
│   ├── static/
│   │   ├── css/style.css   # Dashboard styles
│   │   └── js/dashboard.js # Real-time dashboard logic
│   └── templates/
│       └── index.html      # Dashboard UI
└── docs/
    └── architecture.md     # Detailed system docs
```

## 🚀 Quick Start

### 1. Firmware (Arduino IDE)
- Open `firmware/edge_node/edge_node.ino` in Arduino IDE
- Select ESP32 board, upload to edge node
- Open `firmware/main_node/main_node.ino` in Arduino IDE
- Select ESP32 board, upload to main node

### 2. Dashboard
```bash
cd dashboard
pip install -r requirements.txt
python main.py
```
Open `http://localhost:8000` in your browser.

## 📡 Data Format (ESP-NOW Payload)

| Field       | Type    | Description          |
|-------------|---------|----------------------|
| node_id     | uint8   | Unique edge node ID  |
| heart_rate  | float   | BPM from MAX30102    |
| spo2        | float   | SpO2 % from MAX30102 |
| temperature | float   | °C from NTC sensor   |

## ⚠️ Vital Sign Thresholds (Default)

| Vital       | Normal Range    | Alert Trigger         |
|-------------|----------------|-----------------------|
| Heart Rate  | 60–100 BPM     | < 50 or > 120 BPM    |
| SpO2        | 95–100%        | < 90%                 |
| Temperature | 36.1–37.2 °C   | < 35 or > 38.5 °C    |

## 🛠️ Tech Stack

- **Edge Node**: ESP32, MAX30102, NTC Thermistor, SSD1306 OLED
- **Main Node**: ESP32, Passive Buzzer
- **Communication**: ESP-NOW (peer-to-peer, low latency)
- **Dashboard**: Python, FastAPI, WebSocket, Chart.js
- **Database**: SQLite
- **ML Engine**: scikit-learn (Isolation Forest)

## 📜 License

MIT License
