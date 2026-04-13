"""
PulseNet Dashboard — SQLite Database Handler
Handles all data logging and retrieval operations.
"""

import sqlite3
import threading
from datetime import datetime
from config import DB_PATH


class Database:
    """Thread-safe SQLite database handler for vitals logging."""

    def __init__(self, db_path: str = DB_PATH):
        self.db_path = db_path
        self._lock = threading.Lock()
        self._init_db()

    def _get_conn(self) -> sqlite3.Connection:
        """Create a new connection (SQLite connections are not thread-safe)."""
        conn = sqlite3.connect(self.db_path)
        conn.row_factory = sqlite3.Row
        return conn

    def _init_db(self):
        """Create tables if they don't exist."""
        with self._lock:
            conn = self._get_conn()
            cursor = conn.cursor()
            cursor.execute("""
                CREATE TABLE IF NOT EXISTS vitals (
                    id          INTEGER PRIMARY KEY AUTOINCREMENT,
                    timestamp   TEXT NOT NULL,
                    node_id     INTEGER NOT NULL,
                    heart_rate  REAL NOT NULL,
                    spo2        REAL NOT NULL,
                    temperature REAL NOT NULL,
                    is_anomaly  INTEGER DEFAULT 0
                )
            """)
            cursor.execute("""
                CREATE TABLE IF NOT EXISTS alerts (
                    id          INTEGER PRIMARY KEY AUTOINCREMENT,
                    timestamp   TEXT NOT NULL,
                    node_id     INTEGER NOT NULL,
                    alert_type  TEXT NOT NULL,
                    message     TEXT NOT NULL,
                    value       REAL NOT NULL
                )
            """)
            conn.commit()
            conn.close()

    def insert_vitals(self, node_id: int, heart_rate: float, spo2: float,
                      temperature: float, is_anomaly: bool = False) -> int:
        """Insert a new vitals reading. Returns the row ID."""
        with self._lock:
            conn = self._get_conn()
            cursor = conn.cursor()
            cursor.execute("""
                INSERT INTO vitals (timestamp, node_id, heart_rate, spo2, temperature, is_anomaly)
                VALUES (?, ?, ?, ?, ?, ?)
            """, (
                datetime.now().isoformat(),
                node_id,
                heart_rate,
                spo2,
                temperature,
                int(is_anomaly)
            ))
            conn.commit()
            row_id = cursor.lastrowid
            conn.close()
            return row_id

    def insert_alert(self, node_id: int, alert_type: str, message: str, value: float):
        """Log an alert event."""
        with self._lock:
            conn = self._get_conn()
            cursor = conn.cursor()
            cursor.execute("""
                INSERT INTO alerts (timestamp, node_id, alert_type, message, value)
                VALUES (?, ?, ?, ?, ?)
            """, (
                datetime.now().isoformat(),
                node_id,
                alert_type,
                message,
                value
            ))
            conn.commit()
            conn.close()

    def get_latest_vitals(self, node_id: int = None, limit: int = 100) -> list:
        """Get latest vitals readings, optionally filtered by node_id."""
        with self._lock:
            conn = self._get_conn()
            cursor = conn.cursor()
            if node_id is not None:
                cursor.execute("""
                    SELECT * FROM vitals WHERE node_id = ?
                    ORDER BY id DESC LIMIT ?
                """, (node_id, limit))
            else:
                cursor.execute("""
                    SELECT * FROM vitals ORDER BY id DESC LIMIT ?
                """, (limit,))
            rows = [dict(row) for row in cursor.fetchall()]
            conn.close()
            return rows

    def get_recent_alerts(self, limit: int = 50) -> list:
        """Get recent alerts."""
        with self._lock:
            conn = self._get_conn()
            cursor = conn.cursor()
            cursor.execute("""
                SELECT * FROM alerts ORDER BY id DESC LIMIT ?
            """, (limit,))
            rows = [dict(row) for row in cursor.fetchall()]
            conn.close()
            return rows

    def get_vitals_for_ml(self, limit: int = 500) -> list:
        """Get vitals data formatted for ML training."""
        with self._lock:
            conn = self._get_conn()
            cursor = conn.cursor()
            cursor.execute("""
                SELECT heart_rate, spo2, temperature FROM vitals
                ORDER BY id DESC LIMIT ?
            """, (limit,))
            rows = [dict(row) for row in cursor.fetchall()]
            conn.close()
            return rows

    def get_stats(self) -> dict:
        """Get summary statistics."""
        with self._lock:
            conn = self._get_conn()
            cursor = conn.cursor()
            cursor.execute("SELECT COUNT(*) as total FROM vitals")
            total = cursor.fetchone()["total"]
            cursor.execute("SELECT COUNT(*) as anomalies FROM vitals WHERE is_anomaly = 1")
            anomalies = cursor.fetchone()["anomalies"]
            cursor.execute("SELECT COUNT(*) as alerts FROM alerts")
            alerts = cursor.fetchone()["alerts"]
            conn.close()
            return {
                "total_readings": total,
                "total_anomalies": anomalies,
                "total_alerts": alerts,
            }
