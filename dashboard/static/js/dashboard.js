/**
 * PulseNet Dashboard — Frontend Logic
 * Updated for Antigravity Premium Dark UI
 */

// ─── CONFIG ─────────────────────────────────────────
const MAX_DATA_POINTS = 60;
const WS_RECONNECT_DELAY = 3000; // ms

// ─── STATE ──────────────────────────────────────────
let ws = null;
let activeNodes = new Set();
let alertCount = 0;

// ─── CHART DATA ─────────────────────────────────────
const chartData = {
    labels: [],
    hr: [],
    spo2: [],
    temp: [],
};

// ─── CHART CONFIG ───────────────────────────────────
const chartOptions = (label, color, min, max) => ({
    responsive: true,
    maintainAspectRatio: false,
    animation: { duration: 300 },
    plugins: {
        legend: { display: false },
        tooltip: {
            backgroundColor: '#ffffff',
            titleColor: '#121317',
            bodyColor: '#45474D',
            borderColor: 'rgba(18, 19, 23, 0.1)',
            borderWidth: 1,
            cornerRadius: 12,
            padding: 12,
            titleFont: { family: 'Google Sans Flex', size: 14, weight: '500' },
            bodyFont: { family: 'Google Sans Flex', size: 13 },
        },
    },
    scales: {
        x: {
            display: false,
        },
        y: {
            min: min,
            max: max,
            grid: {
                color: 'rgba(18, 19, 23, 0.05)',
                drawBorder: false,
            },
            ticks: {
                color: '#45474D',
                font: { size: 12, family: 'Google Sans Flex' },
                padding: 10,
            },
        },
    },
    elements: {
        line: {
            tension: 0.4,
            borderWidth: 3,
            borderColor: color,
            fill: true,
            backgroundColor: (context) => {
                const ctx = context.chart.ctx;
                const gradient = ctx.createLinearGradient(0, 0, 0, 300);
                gradient.addColorStop(0, color + '60'); // 40% opacity
                gradient.addColorStop(1, color + '00'); // 0% opacity
                return gradient;
            },
        },
        point: {
            radius: 0,
            hoverRadius: 6,
            hoverBackgroundColor: '#ffffff',
            hoverBorderColor: color,
            hoverBorderWidth: 3,
        },
    },
});

// ─── INIT CHARTS ────────────────────────────────────
const hrChart = new Chart(document.getElementById('chart-hr'), {
    type: 'line',
    data: {
        labels: chartData.labels,
        datasets: [{ data: chartData.hr }],
    },
    options: chartOptions('Heart Rate', '#FC413D', 40, 140),
});

const spo2Chart = new Chart(document.getElementById('chart-spo2'), {
    type: 'line',
    data: {
        labels: chartData.labels,
        datasets: [{ data: chartData.spo2 }],
    },
    options: chartOptions('SpO2', '#3186FF', 80, 105),
});

const tempChart = new Chart(document.getElementById('chart-temp'), {
    type: 'line',
    data: {
        labels: chartData.labels,
        datasets: [{ data: chartData.temp }],
    },
    options: chartOptions('Temperature', '#FBBC04', 20, 50),
});

// ─── UPDATE CHARTS ──────────────────────────────────
function updateCharts(data) {
    const time = new Date(data.timestamp).toLocaleTimeString('en-US', {
        hour12: false, hour: '2-digit', minute: '2-digit', second: '2-digit'
    });

    chartData.labels.push(time);
    chartData.hr.push(data.heart_rate);
    chartData.spo2.push(data.spo2);
    chartData.temp.push(data.temperature);

    // Trim to max data points
    if (chartData.labels.length > MAX_DATA_POINTS) {
        chartData.labels.shift();
        chartData.hr.shift();
        chartData.spo2.shift();
        chartData.temp.shift();
    }

    hrChart.update('none');
    spo2Chart.update('none');
    tempChart.update('none');
}

// ─── UPDATE STAT CARDS ──────────────────────────────
function updateStatCards(data) {
    // Heart Rate
    const hrEl = document.getElementById('value-hr');
    hrEl.textContent = data.heart_rate.toFixed(1);
    const hrCard = document.getElementById('card-hr');
    hrCard.classList.toggle('alert', data.heart_rate < 50 || data.heart_rate > 120);

    // SpO2
    const spo2El = document.getElementById('value-spo2');
    spo2El.textContent = data.spo2.toFixed(1);
    const spo2Card = document.getElementById('card-spo2');
    spo2Card.classList.toggle('alert', data.spo2 < 90);

    // Temperature
    const tempEl = document.getElementById('value-temp');
    tempEl.textContent = data.temperature.toFixed(1);
    const tempCard = document.getElementById('card-temp');
    tempCard.classList.toggle('alert', data.temperature > 37.2);

    // ML Anomaly
    if (data.ml) {
        const anomalyEl = document.getElementById('value-anomaly');
        anomalyEl.textContent = data.ml.is_anomaly ? 'ANOMALY' : 'Normal';
        document.getElementById('anomaly-confidence').textContent =
            data.ml.confidence !== 'none' ? `Confidence: ${data.ml.confidence}` : '';
        document.getElementById('anomaly-score').textContent =
            `Score: ${data.ml.anomaly_score}`;

        const anomalyCard = document.getElementById('card-anomaly');
        anomalyCard.classList.toggle('alert', data.ml.is_anomaly);
    }
}

// ─── ADD ALERT ──────────────────────────────────────
function addAlert(timestamp, nodeId, message, isMl = false) {
    const alertsList = document.getElementById('alerts-list');

    // Remove placeholder
    const placeholder = alertsList.querySelector('.alert-placeholder');
    if (placeholder) placeholder.remove();

    const time = new Date(timestamp).toLocaleTimeString('en-US', {
        hour12: false, hour: '2-digit', minute: '2-digit', second: '2-digit'
    });

    const alertEl = document.createElement('div');
    alertEl.className = `alert-item${isMl ? ' ml-alert' : ''}`;
    
    // Instead of emoji, use Google Symbols for ML
    const iconSpan = `<span class="google-symbols" aria-hidden="true">${isMl ? 'robot_2' : 'sensors'}</span>`;
    
    alertEl.innerHTML = `
        ${iconSpan}
        <span class="alert-time">${time}</span>
        <span class="alert-node">Node ${nodeId}</span>
        <span class="alert-message">${message}</span>
    `;

    alertsList.prepend(alertEl);

    // Keep max 50 alerts in DOM
    while (alertsList.children.length > 50) {
        alertsList.lastChild.remove();
    }

    // Update count
    alertCount++;
    document.getElementById('alert-count').textContent = alertCount;
}

// ─── WEBSOCKET CONNECTION ───────────────────────────
function connectWebSocket() {
    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
    const wsUrl = `${protocol}//${window.location.host}/ws`;

    ws = new WebSocket(wsUrl);

    ws.onopen = () => {
        console.log('[WS] Connected');
        setConnectionStatus(true);
    };

    ws.onmessage = (event) => {
        try {
            const data = JSON.parse(event.data);

            if (data.type === 'vitals') {
                // Track active nodes
                activeNodes.add(data.node_id);
                document.getElementById('stat-nodes').textContent = activeNodes.size;

                // Update UI
                updateStatCards(data);
                updateCharts(data);

                // Process alerts
                if (data.alerts && data.alerts.length > 0) {
                    data.alerts.forEach(msg => addAlert(data.timestamp, data.node_id, msg));
                }
                if (data.ml && data.ml.is_anomaly) {
                    addAlert(data.timestamp, data.node_id, `ML: ${data.ml.message} (score: ${data.ml.anomaly_score})`, true);
                }
            }
        } catch (e) {
            console.error('[WS] Parse error:', e);
        }
    };

    ws.onclose = () => {
        console.log('[WS] Disconnected. Reconnecting...');
        setConnectionStatus(false);
        setTimeout(connectWebSocket, WS_RECONNECT_DELAY);
    };

    ws.onerror = (err) => {
        console.error('[WS] Error:', err);
        ws.close();
    };
}

// ─── CONNECTION STATUS ──────────────────────────────
function setConnectionStatus(connected) {
    // Disabled connection status UI
}

// ─── CLOCK ──────────────────────────────────────────
function updateClock() {
    const now = new Date();
    document.getElementById('clock').textContent = now.toLocaleTimeString('en-US', {
        hour12: false, hour: '2-digit', minute: '2-digit', second: '2-digit'
    });
}

// ─── FETCH INITIAL STATS ────────────────────────────
async function fetchStats() {
    try {
        const res = await fetch('/api/stats');
        const data = await res.json();
        document.getElementById('stat-total').textContent = data.total_readings || 0;
        document.getElementById('stat-anomalies').textContent = data.total_anomalies || 0;
        document.getElementById('stat-alerts').textContent = data.total_alerts || 0;

        // ML status badge
        const mlBadge = document.getElementById('ml-status');
        const mlText = mlBadge.querySelector('.status-text');
        if (data.ml_trained) {
            mlBadge.className = 'status-badge active';
            mlText.textContent = 'ML: Active';
        } else {
            mlBadge.className = 'status-badge inactive';
            mlText.textContent = 'ML: Training...';
        }
    } catch (e) {
        console.error('[API] Stats fetch failed:', e);
    }
}

// ─── INIT ───────────────────────────────────────────
document.addEventListener('DOMContentLoaded', () => {
    updateClock();
    setInterval(updateClock, 1000);

    fetchStats();
    setInterval(fetchStats, 10000); // Refresh stats every 10s

    connectWebSocket();

    console.log('PulseNet Dashboard loaded');
});
