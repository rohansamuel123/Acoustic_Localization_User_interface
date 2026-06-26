"""
ATLN core engine — shared by live (serial) and simulated data paths.

Pure standard-library (no numpy / no extra pip deps) so it runs on the
embeddable Python that ships with this machine.

Responsibilities
----------------
1. localize()  -> estimate the acoustic source (x, y) from the four node
                  energies using an inverse-distance attenuation model and a
                  grid search.  This is the fix for "every node peaks the same".
2. classify()  -> map the loudest node energy to a LOW / MEDIUM / HIGH threat.
3. Simulator   -> generate physically-plausible events (inverse-square law +
                  noise) so the whole pipeline can be demonstrated with no
                  hardware attached.
4. FirebaseClient -> mirror the latest reading + event log to a Firebase
                  Realtime Database over the REST API, in a background thread,
                  so it can never block or crash the acquisition loop.
"""

import os
import json
import math
import time
import random
import threading
import urllib.request
import urllib.error


# ──────────────────────────────────────────────────────────────────────────
# Sensor geometry — four nodes at the corners of a 4 x 4 m square.
#   A(0,0) ---- B(4,0)
#     |           |
#   C(0,4) ---- D(4,4)
# ──────────────────────────────────────────────────────────────────────────
NODES = {
    "A": (0.0, 0.0),
    "B": (4.0, 0.0),
    "C": (0.0, 4.0),
    "D": (4.0, 4.0),
}
NODE_KEYS = ["A", "B", "C", "D"]
GRID_SIZE = 4.0          # metres (square side)
ADC_MAX = 4095.0         # 12-bit ADC full scale

# Near-field softening distance for the 1/(d+D0) amplitude model.  Keeps the
# model finite right on top of a node and matches real mic behaviour better
# than a pure 1/d singularity.
D0 = 0.55

# Speed of sound — used only for the cosmetic propagation timing in the UI,
# exposed here so the contract is shared.
SPEED_OF_SOUND = 343.0

# Threat thresholds on the loudest node energy (RMS-scale, 0..4095).
# Tunable via environment so they can be calibrated to real hardware gain.
THREAT_HIGH = float(os.environ.get("ATLN_THREAT_HIGH", 2600))
THREAT_MEDIUM = float(os.environ.get("ATLN_THREAT_MEDIUM", 1500))


def _amplitude_pattern(x, y):
    """Predicted *normalised* energy each node would see for a source at (x,y).

    Far-field acoustic pressure amplitude falls off ~1/distance, so a node's
    reading is proportional to 1/(d+D0).  We normalise to a unit-sum pattern so
    the (unknown) source loudness cancels out — only the *shape* matters for
    localization.
    """
    inv = []
    for k in NODE_KEYS:
        nx, ny = NODES[k]
        d = math.hypot(x - nx, y - ny)
        inv.append(1.0 / (d + D0))
    s = sum(inv)
    return [v / s for v in inv]


def localize(energies):
    """Estimate (x, y, confidence) from a dict of node energies {A,B,C,D}.

    Strategy: find the source position whose predicted attenuation *shape*
    best matches the measured shape (scale-invariant least squares), via a
    coarse grid search followed by local refinement.  Confidence blends the
    fit quality with the contrast of the reading (a single dominant node gives
    a sharp, trustworthy fix; four equal readings do not).
    """
    vals = [max(0.0, float(energies.get(k, 0))) for k in NODE_KEYS]
    total = sum(vals)
    if total <= 1e-6:
        return GRID_SIZE / 2.0, GRID_SIZE / 2.0, 0.0

    meas = [v / total for v in vals]

    def objective(x, y):
        p = _amplitude_pattern(x, y)
        return sum((meas[i] - p[i]) ** 2 for i in range(4))

    # Coarse grid (41 x 41 over the 4 m square -> 0.1 m resolution).
    best = (GRID_SIZE / 2.0, GRID_SIZE / 2.0)
    best_obj = float("inf")
    steps = 41
    for i in range(steps):
        x = GRID_SIZE * i / (steps - 1)
        for j in range(steps):
            y = GRID_SIZE * j / (steps - 1)
            o = objective(x, y)
            if o < best_obj:
                best_obj = o
                best = (x, y)

    # Local refinement — shrink a search window around the best cell.
    bx, by = best
    win = 0.1
    for _ in range(4):
        improved = False
        for dx in (-win, -win / 2, 0, win / 2, win):
            for dy in (-win, -win / 2, 0, win / 2, win):
                x = min(GRID_SIZE, max(0.0, bx + dx))
                y = min(GRID_SIZE, max(0.0, by + dy))
                o = objective(x, y)
                if o < best_obj:
                    best_obj = o
                    bx, by = x, y
                    improved = True
        win *= 0.5
        if not improved:
            win *= 0.5

    # Confidence: fit quality (how well the shape matched) x contrast (how
    # un-uniform the reading was; 0.25 each == perfectly ambiguous centre).
    fit = math.exp(-best_obj * 9.0)               # 0..1, 1 == perfect match
    contrast = (max(meas) - 0.25) / 0.75          # 0 when uniform, ->1 sharp
    contrast = max(0.0, min(1.0, contrast))
    confidence = round(max(0.0, min(1.0, 0.35 * fit + 0.65 * contrast)), 3)

    return round(bx, 3), round(by, 3), confidence


def classify(energies):
    """LOW / MEDIUM / HIGH from the loudest node energy."""
    peak = max(float(energies.get(k, 0)) for k in NODE_KEYS)
    if peak > THREAT_HIGH:
        return "HIGH"
    if peak > THREAT_MEDIUM:
        return "MEDIUM"
    return "LOW"


def build_reading(energies, source="live", event_id=0, ts=None):
    """Turn raw node energies into the full enriched reading the API serves.

    Keeps the original contract (A,B,C,D,x,y,threat) and adds the fields the
    new cinematic dashboard uses (confidence, energies, event_id, source, ts).
    """
    e = {k: int(round(max(0.0, min(ADC_MAX, float(energies.get(k, 0)))))) for k in NODE_KEYS}
    x, y, confidence = localize(e)
    threat = classify(e)
    return {
        "A": e["A"], "B": e["B"], "C": e["C"], "D": e["D"],
        "x": x, "y": y,
        "threat": threat,
        "confidence": confidence,
        "energies": e,
        "event_id": event_id,
        "source": source,
        "ts": ts if ts is not None else int(time.time() * 1000),
    }


# ──────────────────────────────────────────────────────────────────────────
# Physics-based event simulator
# ──────────────────────────────────────────────────────────────────────────
EVENT_PROFILES = {
    # label        target loudest-node energy (RMS-scale)
    "gunshot":  3550,
    "blast":    3850,
    "impact":   2350,
    "footfall": 1650,
}


class Simulator:
    """Generate physically-plausible node energies for a random source.

    A source at (sx, sy) radiates; each node receives an amplitude that falls
    off as 1/(d+D0), so nodes at different distances genuinely read different
    values — exactly the differentiation real hardware should produce once the
    firmware measures RMS energy instead of a railed peak.
    """

    def __init__(self, seed=None):
        self._rng = random.Random(seed)

    def event(self, kind=None, sx=None, sy=None):
        rng = self._rng
        if kind is None:
            kind = rng.choices(
                ["gunshot", "blast", "impact", "footfall"],
                weights=[0.4, 0.15, 0.3, 0.15],
            )[0]
        if sx is None:
            sx = rng.uniform(0.3, GRID_SIZE - 0.3)
        if sy is None:
            sy = rng.uniform(0.3, GRID_SIZE - 0.3)

        target_peak = EVENT_PROFILES.get(kind, 2500)

        # Distances from the (true) source to each node.
        dists = {k: math.hypot(sx - NODES[k][0], sy - NODES[k][1]) for k in NODE_KEYS}
        nearest = min(dists.values())

        # Choose source strength S so the *nearest* node hits target_peak:
        #   energy_i = S / (d_i + D0)
        S = target_peak * (nearest + D0)

        energies = {}
        for k in NODE_KEYS:
            base = S / (dists[k] + D0)
            noise = rng.uniform(-0.04, 0.04) * base + rng.uniform(-25, 25)
            energies[k] = max(0.0, min(ADC_MAX, base + noise))

        return {
            "kind": kind,
            "true_x": round(sx, 3),
            "true_y": round(sy, 3),
            "energies": energies,
        }


# ──────────────────────────────────────────────────────────────────────────
# Firebase Realtime Database mirror (REST, background, non-blocking)
# ──────────────────────────────────────────────────────────────────────────
def _load_firebase_config():
    """Resolve Firebase settings from env vars or a firebase_config.json file.

    env:  FIREBASE_DB_URL, FIREBASE_AUTH
    file: firebase_config.json  -> {"db_url": "...", "auth": "..."}
    """
    db_url = os.environ.get("FIREBASE_DB_URL", "").strip()
    auth = os.environ.get("FIREBASE_AUTH", "").strip()

    cfg_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "firebase_config.json")
    if os.path.exists(cfg_path):
        try:
            with open(cfg_path, "r", encoding="utf-8") as fh:
                data = json.load(fh)
            db_url = db_url or str(data.get("db_url", "")).strip()
            auth = auth or str(data.get("auth", "")).strip()
        except Exception:
            pass

    return db_url, auth


class FirebaseClient:
    """Mirror state to Firebase RTDB over REST, off the acquisition thread."""

    def __init__(self):
        self.db_url, self.auth = _load_firebase_config()
        self.enabled = bool(self.db_url)
        self._lock = threading.Lock()
        self.last_status = "disabled" if not self.enabled else "init"
        self.last_error = ""
        self.last_sync_ts = 0
        self.ok_count = 0
        self.err_count = 0

    def _url(self, path):
        base = self.db_url.rstrip("/")
        url = f"{base}/{path}.json"
        if self.auth:
            url += f"?auth={self.auth}"
        return url

    def _request(self, method, path, payload):
        data = json.dumps(payload).encode("utf-8")
        req = urllib.request.Request(
            self._url(path), data=data, method=method,
            headers={"Content-Type": "application/json"},
        )
        try:
            with urllib.request.urlopen(req, timeout=4) as resp:
                resp.read()
            with self._lock:
                self.last_status = "ok"
                self.last_error = ""
                self.last_sync_ts = int(time.time() * 1000)
                self.ok_count += 1
            return True
        except urllib.error.HTTPError as exc:
            msg = f"HTTP {exc.code}"
            try:
                msg += ": " + exc.read().decode("utf-8", "ignore")[:160]
            except Exception:
                pass
            self._fail(msg)
        except Exception as exc:                       # noqa: BLE001
            self._fail(str(exc)[:160])
        return False

    def _fail(self, message):
        with self._lock:
            self.last_status = "error"
            self.last_error = message
            self.err_count += 1

    def push_latest(self, reading):
        if not self.enabled:
            return
        threading.Thread(
            target=self._request, args=("PUT", "acoustic/latest", reading), daemon=True
        ).start()

    def push_event(self, reading):
        if not self.enabled:
            return
        threading.Thread(
            target=self._request, args=("POST", "acoustic/events", reading), daemon=True
        ).start()

    def status(self):
        with self._lock:
            return {
                "enabled": self.enabled,
                "db_url": self.db_url,
                "status": self.last_status,
                "error": self.last_error,
                "last_sync_ts": self.last_sync_ts,
                "ok_count": self.ok_count,
                "err_count": self.err_count,
            }
