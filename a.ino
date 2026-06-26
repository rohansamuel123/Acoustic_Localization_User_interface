/*
 * ATLN — SENSOR NODE firmware (Nodes A, B, C)
 * ---------------------------------------------------------------------------
 * WHY THIS WAS REWRITTEN
 *   The old firmware sent a single raw ADC sample (`packet.peak = sample`).
 *   A loud sound rails the ADC near 4095 at EVERY microphone, so all nodes
 *   reported the same value -> no spatial differentiation -> the localizer
 *   always collapsed to the centre of the grid.
 *
 * WHAT THIS DOES INSTEAD
 *   It measures the *RMS energy* of the signal over a short window (the AC
 *   component, with the DC bias removed).  RMS amplitude is proportional to
 *   the sound pressure at the mic, which genuinely falls off with distance,
 *   so a node close to the source reports a much larger value than a far one.
 *   The master + Python backend then triangulate from those differences.
 *
 *   >>> CALIBRATION (important) <<<
 *   1. Turn the MAX4466 gain trim-pot DOWN until a loud clap right next to the
 *      mic reads ~3000-3800 here (NOT a flat 4095).  Headroom = differentiation.
 *   2. Adjust ENERGY_GAIN so a near, loud event reads ~3500 in the serial log
 *      and ambient sits near a few hundred.
 *   Each board flashes this same file — just change NODE_ID to 'A', 'B' or 'C'.
 */

#include <WiFi.h>
#include <esp_now.h>

#define MIC_PIN 34

char NODE_ID = 'A';          // <-- set to 'A', 'B' or 'C' per board

// Master MAC address (Node D).  Replace with your master's MAC.
uint8_t masterAddress[] = {0xB4, 0xBF, 0xE9, 0x0E, 0x8B, 0x98};

typedef struct {
  char node;
  int  energy;               // RMS energy, scaled to 0..4095
  unsigned long timestamp;
} SensorData;

SensorData packet;

// ── Calibration constants ──────────────────────────────────────────────────
const int   N_SAMPLES   = 220;   // samples per RMS window (~a few ms)
float       ENERGY_GAIN = 6.0;   // RMS -> 0..4095 scale  (tune per hardware)
const float SEND_FLOOR  = 55.0;  // skip transmitting near-silence
const int   LOOP_DELAY  = 60;    // ms between measurements (~16 Hz stream)

int sampleBuf[N_SAMPLES];

// Measure the RMS energy of the AC component (DC bias removed).
float measureEnergy() {
  long acc = 0;
  for (int i = 0; i < N_SAMPLES; i++) {
    sampleBuf[i] = analogRead(MIC_PIN);
    acc += sampleBuf[i];
  }
  float mean = acc / (float)N_SAMPLES;          // DC bias

  float sumSq = 0;
  for (int i = 0; i < N_SAMPLES; i++) {
    float dev = sampleBuf[i] - mean;
    sumSq += dev * dev;
  }
  float rms = sqrt(sumSq / (float)N_SAMPLES);   // AC amplitude

  float energy = rms * ENERGY_GAIN;
  if (energy > 4095) energy = 4095;
  return energy;
}

void OnDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  // (optional) Serial.println(status == ESP_NOW_SEND_SUCCESS ? "ok" : "fail");
}

void setup() {
  Serial.begin(115200);
  analogReadResolution(12);                     // 0..4095
  WiFi.mode(WIFI_STA);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW Init Failed");
    return;
  }
  esp_now_register_send_cb(OnDataSent);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, masterAddress, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;
  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to add peer");
    return;
  }
  Serial.print("Node ");
  Serial.print(NODE_ID);
  Serial.println(" ready (RMS energy mode)");
}

void loop() {
  float energy = measureEnergy();

  // Continuously stream energy to the master so it always has a fresh reading;
  // skip dead air to keep the ESP-NOW channel quiet.
  if (energy > SEND_FLOOR) {
    packet.node      = NODE_ID;
    packet.energy    = (int)energy;
    packet.timestamp = millis();
    esp_now_send(masterAddress, (uint8_t *)&packet, sizeof(packet));
  }

  delay(LOOP_DELAY);
}
