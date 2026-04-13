"""
PulseNet Dashboard — Serial Reader
Reads JSON data from ESP32 main node via USB Serial port.
"""

import json
import serial
import threading
import time
from typing import Callable, Optional
from config import SERIAL_PORT, SERIAL_BAUD, SERIAL_TIMEOUT


class SerialReader:
    """
    Reads JSON-formatted vitals data from the ESP32 main node.
    Runs in a background thread and invokes a callback on each new reading.
    """

    def __init__(self, port: str = SERIAL_PORT, baud: int = SERIAL_BAUD,
                 timeout: float = SERIAL_TIMEOUT):
        self.port = port
        self.baud = baud
        self.timeout = timeout
        self._serial: Optional[serial.Serial] = None
        self._thread: Optional[threading.Thread] = None
        self._running = False
        self._callback: Optional[Callable] = None

    def set_callback(self, callback: Callable):
        """
        Set the callback function that gets called on each new data reading.
        Callback signature: callback(data: dict)
        where data = {"node_id": int, "heart_rate": float, "spo2": float, "temperature": float}
        """
        self._callback = callback

    def start(self):
        """Start reading serial data in a background thread."""
        if self._running:
            print("[Serial] Already running.")
            return

        self._running = True
        self._thread = threading.Thread(target=self._read_loop, daemon=True)
        self._thread.start()
        print(f"[Serial] Started on {self.port} @ {self.baud} baud")

    def stop(self):
        """Stop the serial reader."""
        self._running = False
        if self._thread:
            self._thread.join(timeout=3)
        if self._serial and self._serial.is_open:
            self._serial.close()
        print("[Serial] Stopped.")

    def _connect(self) -> bool:
        """Attempt to connect to the serial port."""
        try:
            self._serial = serial.Serial(
                port=self.port,
                baudrate=self.baud,
                timeout=self.timeout
            )
            time.sleep(2)  # Wait for ESP32 to reset after serial connection
            print(f"[Serial] Connected to {self.port}")
            return True
        except serial.SerialException as e:
            print(f"[Serial] Connection failed: {e}")
            return False

    def _read_loop(self):
        """Main read loop — runs in background thread."""
        while self._running:
            # Try to connect if not connected
            if not self._serial or not self._serial.is_open:
                if not self._connect():
                    print("[Serial] Retrying in 5 seconds...")
                    time.sleep(5)
                    continue

            try:
                line = self._serial.readline().decode("utf-8").strip()
                if not line:
                    continue

                # Parse JSON data from main node
                data = json.loads(line)

                # Validate required fields
                required = ["node_id", "heart_rate", "spo2", "temperature"]
                if all(key in data for key in required):
                    if self._callback:
                        self._callback(data)
                else:
                    print(f"[Serial] Incomplete data: {data}")

            except json.JSONDecodeError:
                # Non-JSON lines (debug prints from ESP32) — ignore
                pass
            except serial.SerialException as e:
                print(f"[Serial] Error: {e}")
                self._serial = None
                time.sleep(2)
            except Exception as e:
                print(f"[Serial] Unexpected error: {e}")
                time.sleep(1)

    @property
    def is_connected(self) -> bool:
        """Check if serial port is currently connected."""
        return self._serial is not None and self._serial.is_open
