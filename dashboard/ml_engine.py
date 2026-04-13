"""
PulseNet Dashboard — ML Anomaly Detection Engine
Uses Isolation Forest to detect abnormal vital sign patterns.
"""

import os
import numpy as np
import joblib
from typing import Optional
from sklearn.ensemble import IsolationForest
from config import ML_MODEL_PATH, ML_CONTAMINATION, ML_MIN_SAMPLES


class MLEngine:
    """
    Anomaly detection engine using Isolation Forest.
    Automatically trains on collected data and predicts anomalies in real-time.
    """

    def __init__(self):
        self.model: Optional[IsolationForest] = None
        self.is_trained = False
        self._sample_count = 0

        # Attempt to load a previously saved model
        self._load_model()

    def _load_model(self):
        """Load a saved model from disk if available."""
        if os.path.exists(ML_MODEL_PATH):
            try:
                self.model = joblib.load(ML_MODEL_PATH)
                self.is_trained = True
                print(f"[ML] Loaded model from {ML_MODEL_PATH}")
            except Exception as e:
                print(f"[ML] Failed to load model: {e}")
                self.model = None
                self.is_trained = False

    def _save_model(self):
        """Save the trained model to disk."""
        if self.model:
            os.makedirs(os.path.dirname(ML_MODEL_PATH), exist_ok=True)
            joblib.dump(self.model, ML_MODEL_PATH)
            print(f"[ML] Model saved to {ML_MODEL_PATH}")

    def train(self, data: list):
        """
        Train the Isolation Forest model on vitals data.
        
        Args:
            data: List of dicts with keys: heart_rate, spo2, temperature
        """
        if len(data) < ML_MIN_SAMPLES:
            print(f"[ML] Not enough data to train ({len(data)}/{ML_MIN_SAMPLES})")
            return

        # Prepare feature matrix
        X = np.array([
            [d["heart_rate"], d["spo2"], d["temperature"]]
            for d in data
        ])

        # Train Isolation Forest
        self.model = IsolationForest(
            contamination=ML_CONTAMINATION,
            n_estimators=100,
            random_state=42,
            n_jobs=-1
        )
        self.model.fit(X)
        self.is_trained = True
        self._sample_count = len(data)

        self._save_model()
        print(f"[ML] Model trained on {len(data)} samples")

    def predict(self, heart_rate: float, spo2: float, temperature: float) -> dict:
        """
        Predict if a reading is anomalous.
        
        Returns:
            dict with keys:
                - is_anomaly (bool): True if the reading is anomalous
                - anomaly_score (float): Lower = more anomalous (-1 to 1 range)
                - confidence (str): "high", "medium", "low"
        """
        if not self.is_trained or self.model is None:
            return {
                "is_anomaly": False,
                "anomaly_score": 0.0,
                "confidence": "none",
                "message": "Model not yet trained"
            }

        X = np.array([[heart_rate, spo2, temperature]])
        prediction = self.model.predict(X)[0]       # 1 = normal, -1 = anomaly
        score = self.model.score_samples(X)[0]       # anomaly score

        is_anomaly = prediction == -1

        # Determine confidence level based on score
        if score < -0.5:
            confidence = "high"
        elif score < -0.3:
            confidence = "medium"
        else:
            confidence = "low"

        result = {
            "is_anomaly": is_anomaly,
            "anomaly_score": round(float(score), 4),
            "confidence": confidence,
            "message": "Anomaly detected!" if is_anomaly else "Normal"
        }

        return result

    def should_retrain(self, current_count: int, retrain_interval: int) -> bool:
        """Check if the model should be retrained based on new data count."""
        return current_count - self._sample_count >= retrain_interval
