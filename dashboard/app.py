"""
ATLN dashboard backend.

Acquisition (live serial OR physics simulation) -> shared localization engine
-> /data + /history API -> Firebase mirror.

The data contract served at /data keeps the original fields
(A, B, C, D, x, y, threat) and adds: confidence, energies, event_id, source, ts.
A discrete "event" (rising edge of acoustic energy) bumps event_id; the
cinematic dashboard uses that to trigger its detect->triangulate->lock sequence.
"""

from flask import Flask, render_template, jsonify, request
import os
import json
import time
import threading

import serial

import engine

app = Flask(__name__)
# Keep templates hot-reloading even with the debugger off.
app.config["TEMPLATES_AUTO_RELOAD"] = True

# ── Shared state ───────────────────────────────────────────────────────────
_lock = threading.Lock()
_event_counter = 0
_last_firebase_latest = 0.0

simulator = engine.Simulator()
firebase = engine.FirebaseClient()

# Idle / ambient reading shown before anything happens.
latest_data = engine.build_reading({k: 0 for k in engine.NODE_KEYS}, source="idle")
event_history = []          # newest appended last, capped at 30

# Live event-detection state (rising-edge with refractory period).
TRIGGER_ENERGY = engine.THREAT_MEDIUM      # energy that counts as an event
REARM_ENERGY = engine.THREAT_MEDIUM * 0.55 # must fall below this to re-arm
REFRACTORY_S = 1.2
_armed = True
_last_event_t = 0.0

# ── Serial port (optional — runs fine with no ESP32 attached) ──────────────
SERIAL_PORT = "COM4"
try:
    ser = serial.Serial(SERIAL_PORT, 115200)
except serial.SerialException as e:
    ser = None
    print(f"[WARN] Could not open {SERIAL_PORT}: {e}")
    print("[WARN] No ESP32 detected -> starting in SIMULATION mode so the "
          "full pipeline (localization, Firebase, dashboard) can be demoed.")

HARDWARE = ser is not None


# ── Core state mutation ────────────────────────────────────────────────────
def _commit(reading, is_event):
    """Atomically publish a reading; log + mirror to Firebase if it's an event."""
    global latest_data, _last_firebase_latest
    with _lock:
        latest_data = reading
        if is_event and reading["threat"] != "LOW":
            event_history.append({
                "time": time.strftime("%H:%M:%S"),
                "threat": reading["threat"],
                "x": reading["x"],
                "y": reading["y"],
                "confidence": reading["confidence"],
                "source": reading["source"],
            })
            del event_history[:-30]

    # Firebase: always mirror discrete events; throttle the live "latest" feed.
    now = time.time()
    if is_event:
        firebase.push_latest(reading)
        firebase.push_event(reading)
    elif now - _last_firebase_latest > 0.5:
        _last_firebase_latest = now
        firebase.push_latest(reading)


def inject_event(energies, source, kind=None, true_pos=None):
    """Register a brand-new discrete acoustic event (bumps event_id)."""
    global _event_counter
    with _lock:
        _event_counter += 1
        eid = _event_counter
    reading = engine.build_reading(energies, source=source, event_id=eid)
    if kind:
        reading["kind"] = kind
    if true_pos:
        reading["true_x"], reading["true_y"] = true_pos
    _commit(reading, is_event=True)
    return reading


def update_live(energies, source):
    """Publish a continuous live reading without bumping event_id."""
    with _lock:
        eid = latest_data.get("event_id", 0)
    reading = engine.build_reading(energies, source=source, event_id=eid)
    _commit(reading, is_event=False)
    return reading


# ── Live serial acquisition ────────────────────────────────────────────────
def serial_reader():
    global _armed, _last_event_t
    while True:
        try:
            line = ser.readline().decode(errors="ignore").strip()
            if not line.startswith("{"):
                continue
            raw = json.loads(line)
            energies = {k: raw.get(k, 0) for k in engine.NODE_KEYS}

            # Continuous live update (HUD stays alive every frame).
            update_live(energies, source="live")

            peak = max(float(energies[k]) for k in engine.NODE_KEYS)
            now = time.time()
            if peak < REARM_ENERGY:
                _armed = True
            if _armed and peak > TRIGGER_ENERGY and (now - _last_event_t) > REFRACTORY_S:
                _armed = False
                _last_event_t = now
                inject_event(energies, source="live")
        except Exception as e:                         # noqa: BLE001
            print("Serial error:", e)
            time.sleep(0.05)


# ── Simulation ─────────────────────────────────────────────────────────────
_sim_running = (not HARDWARE)               # auto-on when no hardware
_env_sim = os.environ.get("ATLN_SIM")
if _env_sim is not None:
    _sim_running = _env_sim.strip() not in ("0", "", "false", "False")

SIM_PERIOD_S = 8.5                          # seconds between auto sim events
                                            # (> cinematic length so it never cuts off)
AMBIENT = 240.0                             # quiescent node energy floor


def sim_loop():
    """Drive the pipeline with simulated events and decay between them."""
    next_event = time.time() + 2.5
    cur = {k: AMBIENT for k in engine.NODE_KEYS}
    while True:
        time.sleep(0.15)
        if not _sim_running:
            next_event = time.time() + SIM_PERIOD_S
            continue

        now = time.time()
        if now >= next_event:
            next_event = now + SIM_PERIOD_S
            ev = simulator.event()
            cur = dict(ev["energies"])
            inject_event(cur, source="sim", kind=ev["kind"],
                         true_pos=(ev["true_x"], ev["true_y"]))
        else:
            # Relax toward ambient so the scene calms between events.
            decayed = {}
            for k in engine.NODE_KEYS:
                decayed[k] = AMBIENT + (cur[k] - AMBIENT) * 0.75
            cur = decayed
            if max(cur.values()) > AMBIENT + 30:
                update_live(cur, source="sim")


# ── Routes ─────────────────────────────────────────────────────────────────
@app.route("/")
def home():
    return render_template("pitch.html")


@app.route("/dashboard")
def dashboard():
    return render_template("dashboard.html")


@app.route("/classic")
def classic():
    return render_template("index.html")


@app.route("/data")
def data():
    with _lock:
        return jsonify(latest_data)


@app.route("/history")
def history():
    with _lock:
        return jsonify(list(event_history))


@app.route("/status")
def status():
    return jsonify({
        "mode": "LIVE" if HARDWARE else "SIM",
        "hardware": HARDWARE,
        "sim_running": _sim_running,
        "event_count": _event_counter,
        "firebase": firebase.status(),
        "thresholds": {"high": engine.THREAT_HIGH, "medium": engine.THREAT_MEDIUM},
    })


@app.route("/config")
def config():
    """Geometry + constants so the dashboard renders an accurate grid."""
    return jsonify({
        "nodes": engine.NODES,
        "grid_size": engine.GRID_SIZE,
        "adc_max": engine.ADC_MAX,
        "speed_of_sound": engine.SPEED_OF_SOUND,
        "thresholds": {"high": engine.THREAT_HIGH, "medium": engine.THREAT_MEDIUM},
    })


@app.route("/sim/trigger", methods=["POST"])
def sim_trigger():
    """Inject one simulated event through the real pipeline (manual demo)."""
    body = request.get_json(silent=True) or {}
    args = request.args
    kind = body.get("kind") or args.get("kind")
    sx = body.get("x", args.get("x"))
    sy = body.get("y", args.get("y"))
    sx = float(sx) if sx is not None else None
    sy = float(sy) if sy is not None else None
    ev = simulator.event(kind=kind, sx=sx, sy=sy)
    reading = inject_event(ev["energies"], source="sim", kind=ev["kind"],
                           true_pos=(ev["true_x"], ev["true_y"]))
    return jsonify(reading)


@app.route("/sim/on", methods=["POST"])
def sim_on():
    global _sim_running
    _sim_running = True
    return jsonify({"sim_running": True})


@app.route("/sim/off", methods=["POST"])
def sim_off():
    global _sim_running
    _sim_running = False
    return jsonify({"sim_running": False})


# ── Background threads ─────────────────────────────────────────────────────
if HARDWARE:
    threading.Thread(target=serial_reader, daemon=True).start()
threading.Thread(target=sim_loop, daemon=True).start()


if __name__ == "__main__":
    mode = "LIVE (serial)" if HARDWARE else "SIMULATION"
    fb = firebase.status()
    print(f"[ATLN] mode={mode}  firebase={'on ' + fb['db_url'] if fb['enabled'] else 'disabled'}")
    # Debugger OFF by default (the Werkzeug debugger allows code execution).
    # Set ATLN_DEBUG=1 only on a trusted dev machine.
    debug = os.environ.get("ATLN_DEBUG", "0").strip() in ("1", "true", "True")
    app.run(host="127.0.0.1", debug=debug, use_reloader=False)
