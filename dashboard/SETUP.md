# ATLN Dashboard — Setup & Operation

Real-time acoustic threat localization: ESP-NOW sensor mesh → Flask backend
→ inverse-distance localization engine → cinematic 3D dashboard → Firebase.

## 1. Install

```bash
cd dashboard
pip install -r requirements.txt          # flask, pyserial
python app.py                            # serves http://127.0.0.1:5000
```

Routes:

| Route        | What it is                                              |
|--------------|---------------------------------------------------------|
| `/`          | Business pitch deck (ECHO) with **TRY LIVE DEMO**       |
| `/dashboard` | Cinematic 3D live threat map                            |
| `/classic`   | Original minimal dashboard                              |
| `/data`      | Latest reading (JSON)                                   |
| `/history`   | Recent events (JSON)                                    |
| `/status`    | Mode, simulation state, Firebase health                 |
| `/config`    | Node geometry + constants                               |
| `/sim/trigger` `/sim/on` `/sim/off` | Drive / toggle the simulator      |

## 2. Modes

- **LIVE** — auto-selected when an ESP32 master is detected on the serial port
  (`SERIAL_PORT` in `app.py`, default `COM4`). The firmware streams raw RMS
  energies; the backend localizes and classifies.
- **SIMULATION** — auto-selected when no hardware is present (or force with
  `ATLN_SIM=1`). A physics-based engine generates realistic, *differentiated*
  node energies for a random source so the full pipeline — localization,
  Firebase, and the cinematic UI — can be demonstrated with no hardware.

In the dashboard, **⚡ SIMULATE EVENT** injects one event through the real
pipeline, and **AUTO SIM** toggles continuous events.

## 3. Firebase (cloud visibility)

1. Firebase console → create/open a project → **Realtime Database** → Create.
2. Copy the database URL, e.g. `https://YOUR-PROJECT-default-rtdb.firebaseio.com`.
3. Put it in `firebase_config.json` (`db_url`), **or** set env var
   `FIREBASE_DB_URL`. For a quick demo set the DB Rules to test mode and leave
   `auth` empty; for locked rules add a secret/token to `auth`.
4. Restart and open `/status` — `firebase.status` should read `ok`.

Writes land at `/acoustic/latest` (live reading) and `/acoustic/events` (log),
visible live in the Firebase console. The dashboard's **CLOUD SYNCED** chip
turns green when writes succeed.

## 4. Hardware calibration (the accuracy fix)

The old firmware sent a single railed ADC sample, so every node read ~4095 for
any loud sound and the source always collapsed to the grid centre. The new
firmware (`a.ino`, `d.ino`) measures **RMS energy**, which falls off with
distance, so nodes genuinely differ.

To get clean localization:

1. Lower each **MAX4466 gain trim-pot** so a loud clap next to the mic reads
   ~3000–3800 in the serial monitor — **not a flat 4095**. Headroom is what
   creates differentiation.
2. Match `ENERGY_GAIN` in `a.ino` and `d.ino` so a near, loud event reads
   ~3500 and ambient sits in the low hundreds.
3. Threat thresholds are tunable via `ATLN_THREAT_HIGH` / `ATLN_THREAT_MEDIUM`.

The localizer (inverse-distance attenuation + grid search) lives in
`engine.py` and is unit-testable off-hardware:

```bash
python -c "import engine; s=engine.Simulator(7); e=s.event(kind='gunshot',sx=0.5,sy=0.5)['energies']; print(e, engine.localize(e))"
```
