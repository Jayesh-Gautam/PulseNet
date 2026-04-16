"""
PulseNet Dashboard — FastAPI Server
Main entry point for the dashboard application.
Handles WebSocket connections, REST API, and serves the UI.
"""

import asyncio
import json
from datetime import datetime
from contextlib import asynccontextmanager
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.staticfiles import StaticFiles
from fastapi.templating import Jinja2Templates
from fastapi.requests import Request
from fastapi.responses import JSONResponse

from config import HOST, PORT, THRESHOLDS, ML_RETRAIN_INTERVAL
from database import Database
from serial_reader import SerialReader
from ml_engine import MLEngine


# ─── GLOBAL STATE ────────────────────────────────────
db = Database()
serial_reader = SerialReader()
ml_engine = MLEngine()
connected_clients: list[WebSocket] = []
loop: asyncio.AbstractEventLoop = None  # Store event loop reference


# ─── WEBSOCKET BROADCAST ────────────────────────────
async def broadcast(data: dict):
    """Send data to all connected WebSocket clients."""
    disconnected = []
    for ws in list(connected_clients):
        try:
            await ws.send_json(data)
        except Exception as e:
            print(f"[WS] Broadcast error: {e}")
            disconnected.append(ws)
    for ws in disconnected:
        if ws in connected_clients:
            connected_clients.remove(ws)


# ─── SERIAL DATA CALLBACK ───────────────────────────
def on_serial_data(data: dict):
    """
    Called by SerialReader whenever new data arrives from ESP32.
    Logs to DB, runs ML prediction, and broadcasts to WebSocket clients.
    """
    node_id = data["node_id"]
    hr = data["heart_rate"]
    spo2 = data["spo2"]
    temp = data["temperature"]

    # --- ML Prediction ---
    ml_result = ml_engine.predict(hr, spo2, temp)
    is_anomaly = ml_result["is_anomaly"]

    # --- Log to Database ---
    db.insert_vitals(node_id, hr, spo2, temp, is_anomaly)

    # --- Threshold-based Alerts ---
    alerts = []
    if hr < THRESHOLDS["heart_rate"]["low"] or hr > THRESHOLDS["heart_rate"]["high"]:
        alert = f"Heart Rate {hr} BPM out of range"
        alerts.append(alert)
        db.insert_alert(node_id, "heart_rate", alert, hr)

    if spo2 < THRESHOLDS["spo2"]["low"]:
        alert = f"SpO2 {spo2}% critically low"
        alerts.append(alert)
        db.insert_alert(node_id, "spo2", alert, spo2)

    if temp < THRESHOLDS["temperature"]["low"] or temp > THRESHOLDS["temperature"]["high"]:
        alert = f"Temperature {temp}°C out of range"
        alerts.append(alert)
        db.insert_alert(node_id, "temperature", alert, temp)

    # --- Prepare broadcast payload ---
    payload = {
        "type": "vitals",
        "timestamp": datetime.now().isoformat(),
        "node_id": node_id,
        "heart_rate": hr,
        "spo2": spo2,
        "temperature": temp,
        "ml": ml_result,
        "alerts": alerts,
    }

    # --- Broadcast to all WebSocket clients (thread-safe) ---
    if loop:
        asyncio.run_coroutine_threadsafe(broadcast(payload), loop)

    # --- Retrain ML if enough new data ---
    stats = db.get_stats()
    if ml_engine.should_retrain(stats["total_readings"], ML_RETRAIN_INTERVAL):
        training_data = db.get_vitals_for_ml()
        ml_engine.train(training_data)


# ─── APP LIFESPAN ───────────────────────────────────
@asynccontextmanager
async def lifespan(app: FastAPI):
    """Start serial reader on startup, stop on shutdown."""
    global loop
    loop = asyncio.get_running_loop()

    serial_reader.set_callback(on_serial_data)
    serial_reader.start()

    # Initial ML training if enough data exists
    training_data = db.get_vitals_for_ml()
    if len(training_data) >= 50:
        ml_engine.train(training_data)

    print(f"[PulseNet] Dashboard running at http://localhost:{PORT}")
    yield

    serial_reader.stop()
    print("[PulseNet] Dashboard stopped.")


# ─── FASTAPI APP ────────────────────────────────────
app = FastAPI(
    title="PulseNet Dashboard",
    description="Real-time patient vitals monitoring with ML anomaly detection",
    version="1.0.0",
    lifespan=lifespan
)

app.mount("/static", StaticFiles(directory="static"), name="static")
templates = Jinja2Templates(directory="templates")


# ─── ROUTES ─────────────────────────────────────────

@app.get("/")
async def dashboard(request: Request):
    """Serve the main dashboard page."""
    return templates.TemplateResponse(request, "index.html")


@app.get("/api/vitals")
async def get_vitals(node_id: int = None, limit: int = 100):
    """Get latest vitals readings."""
    data = db.get_latest_vitals(node_id=node_id, limit=limit)
    return JSONResponse(content=data)


@app.get("/api/alerts")
async def get_alerts(limit: int = 50):
    """Get recent alerts."""
    alerts = db.get_recent_alerts(limit=limit)
    return JSONResponse(content=alerts)


@app.get("/api/stats")
async def get_stats():
    """Get summary statistics."""
    stats = db.get_stats()
    stats["serial_connected"] = serial_reader.is_connected
    stats["ml_trained"] = ml_engine.is_trained
    return JSONResponse(content=stats)


@app.websocket("/ws")
async def websocket_endpoint(ws: WebSocket):
    """WebSocket endpoint for real-time data streaming."""
    await ws.accept()
    connected_clients.append(ws)
    print(f"[WS] Client connected. Total: {len(connected_clients)}")
    try:
        while True:
            # Keep connection alive — listen for pings
            await ws.receive_text()
    except WebSocketDisconnect:
        connected_clients.remove(ws)
        print(f"[WS] Client disconnected. Total: {len(connected_clients)}")


# ─── ENTRY POINT ────────────────────────────────────
if __name__ == "__main__":
    import uvicorn
    uvicorn.run("main:app", host=HOST, port=PORT, reload=True)
