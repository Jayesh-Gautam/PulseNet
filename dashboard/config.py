"""
PulseNet Dashboard — Configuration
All thresholds, serial port settings, and app config live here.
"""

# ─── SERIAL PORT ─────────────────────────────────────
SERIAL_PORT = "COM3"          # Change to your ESP32 port (e.g., COM3, /dev/ttyUSB0)
SERIAL_BAUD = 115200
SERIAL_TIMEOUT = 1            # seconds

# ─── DATABASE ────────────────────────────────────────
DB_PATH = "pulsenet.db"

# ─── VITAL SIGN THRESHOLDS ──────────────────────────
THRESHOLDS = {
    "heart_rate": {"low": 50.0, "high": 120.0, "unit": "BPM"},
    "spo2":       {"low": 90.0, "high": 100.0, "unit": "%"},
    "temperature":{"low": 35.0, "high": 38.5,  "unit": "°C"},
}

# ─── ML ENGINE ───────────────────────────────────────
ML_MODEL_PATH = "models/anomaly_model.pkl"
ML_RETRAIN_INTERVAL = 300     # Retrain every N data points
ML_CONTAMINATION = 0.05       # Expected anomaly fraction (5%)
ML_MIN_SAMPLES = 50           # Minimum samples before ML kicks in

# ─── SERVER ──────────────────────────────────────────
HOST = "0.0.0.0"
PORT = 8000
