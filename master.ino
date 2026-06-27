/*
 * ATLN — MASTER NODE firmware (Node D: sensor + gateway)
 * ---------------------------------------------------------------------------
 * WHY THIS WAS REWRITTEN
 *   Old bugs that wrecked accuracy:
 *     1. It stored a single raw ADC sample per node (railed to ~4095 for any
 *        loud sound) -> no differentiation.
 *     2. A, B, C, D were NEVER reset or decayed, so once a node latched a high
 *        value it stayed high forever and the centroid was stuck at (2, 2).
 *     3. Localization ran on-chip with a weak linear centroid.
 *
 * WHAT THIS DOES NOW
 *   - Measures its own RMS energy (same method as the sensor nodes).
 *   - Collects RMS energies from A, B, C over ESP-NOW, each time-stamped.
 *   - DECAYS any node that hasn't sent a fresh packet recently, so stale
 *     events fade instead of sticking.
 *   - Streams the four raw energies as JSON over USB serial.  The Python
 *     backend now owns localization (inverse-distance attenuation + grid
 *     search) and threat classification, which is far more accurate and is
 *     unit-tested off-hardware.
 *
 *   Output line (every ~120 ms):
 *     {"A":312,"B":1840,"C":640,"D":3550}
 *
 *   Calibration: match ENERGY_GAIN to the sensor nodes and set the MAX4466
 *   gain so a loud near event reads ~3000-3800 (never a flat 4095).
 */

#include <WiFi.h>
#include <esp_now.h>

#define MIC_PIN 34

typedef struct {
  char node;
  int  energy;
  unsigned long timestamp;
} SensorData;

SensorData incoming;

// Energies + last-update time for A(0) B(1) C(2) D(3).
float         E[4]       = {0, 0, 0, 0};
unsigned long lastUpd[4] = {0, 0, 0, 0};

// ── Calibration / behaviour ────────────────────────────────────────────────
const int           N_SAMPLES = 220;     // RMS window (match sensor nodes)
float               ENERGY_GAIN = 6.0;   // RMS -> 0..4095 (match sensor nodes)
const unsigned long STALE_MS  = 280;     // node considered stale after this
const float         DECAY     = 0.78;    // multiply stale nodes each cycle
const int           STREAM_MS = 120;     // serial output cadence

int sampleBuf[N_SAMPLES];

int nodeIndex(char n) {
  if (n == 'A') return 0;
  if (n == 'B') return 1;
  if (n == 'C') return 2;
  return 3;
}

float measureEnergy() {
  long acc = 0;
  for (int i = 0; i < N_SAMPLES; i++) {
    sampleBuf[i] = analogRead(MIC_PIN);
    acc += sampleBuf[i];
  }
  float mean = acc / (float)N_SAMPLES;

  float sumSq = 0;
  for (int i = 0; i < N_SAMPLES; i++) {
    float dev = sampleBuf[i] - mean;
    sumSq += dev * dev;
  }
  float rms = sqrt(sumSq / (float)N_SAMPLES);

  float energy = rms * ENERGY_GAIN;
  if (energy > 4095) energy = 4095;
  return energy;
}

void OnDataRecv(const esp_now_recv_info_t *recv_info,
                const uint8_t *incomingData, int len) {
  memcpy(&incoming, incomingData, sizeof(incoming));
  int i = nodeIndex(incoming.node);
  if (i >= 0 && i < 3) {                 // A, B, C come from peers
    E[i] = incoming.energy;
    lastUpd[i] = millis();
  }
}

void setup() {
  Serial.begin(115200);
  analogReadResolution(12);
  WiFi.mode(WIFI_STA);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW FAILED");
    return;
  }
  esp_now_register_recv_cb(OnDataRecv);
  Serial.println("MASTER READY (RMS energy mode)");
}

void loop() {
  // Node D measures its own energy locally every cycle.
  E[3] = measureEnergy();
  lastUpd[3] = millis();

  // Decay any peer that has gone quiet so old events don't linger.
  unsigned long now = millis();
  for (int i = 0; i < 3; i++) {
    if (now - lastUpd[i] > STALE_MS) {
      E[i] *= DECAY;
      if (E[i] < 1) E[i] = 0;
    }
  }

  // Stream raw energies; Python computes (x, y), threat and confidence.
  Serial.print("{");
  Serial.print("\"A\":"); Serial.print((int)E[0]); Serial.print(",");
  Serial.print("\"B\":"); Serial.print((int)E[1]); Serial.print(",");
  Serial.print("\"C\":"); Serial.print((int)E[2]); Serial.print(",");
  Serial.print("\"D\":"); Serial.print((int)E[3]);
  Serial.println("}");

  delay(STREAM_MS);
}
