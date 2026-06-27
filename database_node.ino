#include <WiFi.h>
#include <esp_now.h>
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

// WiFi Credentials
#define WIFI_SSID "Rohan"
#define WIFI_PASSWORD "idontknow" // <-- MUST CHANGE TO YOUR ACTUAL PASSWORD

// Firebase config
#define API_KEY "AIzaSyAXsDFhSni5UMUEOxGYEP49U0bCyidy8_E"
#define DATABASE_URL "https://acousticthreatnetwork-default-rtdb.firebaseio.com/"
#define USER_EMAIL "rohansamueld.cs25@rvce.edu.in"
#define USER_PASSWORD "rohan200608"

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// ── Master Node State ──────────────────────────────────────────────────────
typedef struct {
  char node;
  int  energy;
  unsigned long timestamp;
} SensorData;

SensorData incoming;

// Energies + last-update time for A(0) B(1) C(2) D(3).
float         E[4]       = {0, 0, 0, 0};
unsigned long lastUpd[4] = {0, 0, 0, 0};

// Decay / streaming constants
const unsigned long STALE_MS  = 280;     // node considered stale after this
const float         DECAY     = 0.78;    // multiply stale nodes each cycle
const int           STREAM_MS = 120;     // serial output cadence

unsigned long lastStreamTime = 0;
bool firebaseInitStarted = false;

void OnDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *incomingData, int len) {
  if (len == sizeof(SensorData)) {
    memcpy(&incoming, incomingData, sizeof(incoming));
    int i = -1;
    if (incoming.node == 'A') i = 0;
    else if (incoming.node == 'B') i = 1;
    else if (incoming.node == 'C') i = 2;
    else if (incoming.node == 'D') i = 3;

    if (i >= 0) {
      E[i] = incoming.energy;
      lastUpd[i] = millis();
    }
  }
}

// ── Localization constants & functions ────────────────────────────────────
const float GRID_SIZE = 4.0;
const float D0 = 0.55;
const float THREAT_HIGH = 2600.0;
const float THREAT_MEDIUM = 1500.0;

const float nodeX[4] = {0.0, 4.0, 0.0, 4.0};
const float nodeY[4] = {0.0, 0.0, 4.0, 4.0};

void localize(int energies[4], float &outX, float &outY, float &confidence) {
  float vals[4];
  float total = 0;
  for(int i = 0; i < 4; i++) {
    vals[i] = max(0.0f, (float)energies[i]);
    total += vals[i];
  }
  
  if (total <= 1e-6) {
    outX = GRID_SIZE / 2.0;
    outY = GRID_SIZE / 2.0;
    confidence = 0.0;
    return;
  }
  
  float meas[4];
  for(int i = 0; i < 4; i++) meas[i] = vals[i] / total;
  
  float bestX = GRID_SIZE / 2.0;
  float bestY = GRID_SIZE / 2.0;
  float best_obj = 1e9;
  
  int steps = 41;
  for (int i = 0; i < steps; i++) {
    float x = GRID_SIZE * i / (steps - 1);
    for (int j = 0; j < steps; j++) {
      float y = GRID_SIZE * j / (steps - 1);
      
      float inv[4];
      float s = 0;
      for(int k = 0; k < 4; k++) {
        float dx = x - nodeX[k];
        float dy = y - nodeY[k];
        float d = sqrt(dx*dx + dy*dy);
        inv[k] = 1.0 / (d + D0);
        s += inv[k];
      }
      
      float obj = 0;
      for(int k = 0; k < 4; k++) {
        float p = inv[k] / s;
        float diff = meas[k] - p;
        obj += diff * diff;
      }
      
      if (obj < best_obj) {
        best_obj = obj;
        bestX = x;
        bestY = y;
      }
    }
  }
  
  float win = 0.1;
  for (int iter = 0; iter < 4; iter++) {
    bool improved = false;
    float d_vals[] = {-win, -win/2, 0, win/2, win};
    for(int i = 0; i < 5; i++) {
      for(int j = 0; j < 5; j++) {
        float x = bestX + d_vals[i];
        float y = bestY + d_vals[j];
        if (x < 0) x = 0; if (x > GRID_SIZE) x = GRID_SIZE;
        if (y < 0) y = 0; if (y > GRID_SIZE) y = GRID_SIZE;
        
        float inv[4];
        float s = 0;
        for(int k = 0; k < 4; k++) {
          float dx = x - nodeX[k];
          float dy = y - nodeY[k];
          float d = sqrt(dx*dx + dy*dy);
          inv[k] = 1.0 / (d + D0);
          s += inv[k];
        }
        
        float obj = 0;
        for(int k = 0; k < 4; k++) {
          float p = inv[k] / s;
          float diff = meas[k] - p;
          obj += diff * diff;
        }
        
        if (obj < best_obj) {
          best_obj = obj;
          bestX = x;
          bestY = y;
          improved = true;
        }
      }
    }
    win *= 0.5;
  }
  
  float fit = exp(-best_obj * 9.0);
  float max_meas = meas[0];
  for(int i = 1; i < 4; i++) if (meas[i] > max_meas) max_meas = meas[i];
  float contrast = (max_meas - 0.25) / 0.75;
  if (contrast < 0) contrast = 0;
  if (contrast > 1) contrast = 1;
  
  confidence = 0.35 * fit + 0.65 * contrast;
  if (confidence < 0) confidence = 0;
  if (confidence > 1) confidence = 1;
  
  outX = bestX;
  outY = bestY;
}

String classify(int energies[4]) {
  int peak = energies[0];
  for(int i = 1; i < 4; i++) if(energies[i] > peak) peak = energies[i];
  
  if (peak > THREAT_HIGH) return "HIGH";
  if (peak > THREAT_MEDIUM) return "MEDIUM";
  return "LOW";
}

unsigned long eventId = 0;
bool armed = true;
unsigned long lastEventTime = 0;
const float TRIGGER_ENERGY = THREAT_MEDIUM;
const float REARM_ENERGY = THREAT_MEDIUM * 0.55;
const unsigned long REFRACTORY_MS = 1200;
unsigned long lastFirebaseUpdate = 0;

void setup() {
  Serial.begin(115200);

  WiFi.mode(WIFI_STA);
  // Start WiFi connection (Non-blocking)
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  // Initialize ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW Init Failed");
    return;
  }
  esp_now_register_recv_cb(OnDataRecv);
  
  // Notice: We removed the blocking while(WiFi.status() != WL_CONNECTED) loop!
  Serial.println("MASTER / DATABASE NODE READY (Listening for A, B, C, D)");
}

void loop() {
  unsigned long now = millis();

  // Initialize Firebase ONLY once WiFi connects, without blocking
  if (WiFi.status() == WL_CONNECTED && !firebaseInitStarted) {
    firebaseInitStarted = true;
    config.api_key = API_KEY;
    config.database_url = DATABASE_URL;
    auth.user.email = USER_EMAIL;
    auth.user.password = USER_PASSWORD;

    Firebase.begin(&config, &auth);
    Firebase.reconnectWiFi(true);
    Serial.println("[WIFI] Connected! Firebase initializing...");
  }

  // 1. Decay any peer that has gone quiet so old events don't linger.
  if (now - lastStreamTime >= STREAM_MS) {
    lastStreamTime = now;
    
    for (int i = 0; i < 4; i++) {
      if (now - lastUpd[i] > STALE_MS) {
        E[i] *= DECAY;
        if (E[i] < 1) E[i] = 0;
      }
    }

    // 2. Stream raw energies to Serial for the Python backend
    Serial.print("{");
    Serial.print("\"A\":"); Serial.print((int)E[0]); Serial.print(",");
    Serial.print("\"B\":"); Serial.print((int)E[1]); Serial.print(",");
    Serial.print("\"C\":"); Serial.print((int)E[2]); Serial.print(",");
    Serial.print("\"D\":"); Serial.print((int)E[3]);
    Serial.println("}");
  }

  // 3. Perform Localization and Firebase Upload
  int energies[4] = {(int)E[0], (int)E[1], (int)E[2], (int)E[3]};
  float x, y, conf;
  localize(energies, x, y, conf);
  String threat = classify(energies);
  
  int peak = energies[0];
  for(int i = 1; i < 4; i++) if (energies[i] > peak) peak = energies[i];
  
  bool isEvent = false;
  if (peak < REARM_ENERGY) {
    armed = true;
  }
  
  if (armed && peak > TRIGGER_ENERGY && (now - lastEventTime > REFRACTORY_MS)) {
    armed = false;
    lastEventTime = now;
    eventId++;
    isEvent = true;
  }
  
  // Throttle regular updates to 500ms, or push immediately on a new event
  if (firebaseInitStarted && Firebase.ready()) {
    if (isEvent || (now - lastFirebaseUpdate > 500)) {
      if (!isEvent) {
        lastFirebaseUpdate = now;
      }
      
      FirebaseJson json;
      json.set("A", energies[0]);
      json.set("B", energies[1]);
      json.set("C", energies[2]);
      json.set("D", energies[3]);
      
      json.set("x", (double)((int)(x * 1000 + 0.5)) / 1000.0);
      json.set("y", (double)((int)(y * 1000 + 0.5)) / 1000.0);
      
      json.set("threat", threat);
      json.set("confidence", (double)((int)(conf * 1000 + 0.5)) / 1000.0);
      json.set("event_id", eventId);
      json.set("source", "live");
      json.set("ts", (double)now);
      
      FirebaseJson energiesJson;
      energiesJson.set("A", energies[0]);
      energiesJson.set("B", energies[1]);
      energiesJson.set("C", energies[2]);
      energiesJson.set("D", energies[3]);
      json.set("energies", energiesJson);
      
      Firebase.RTDB.setJSON(&fbdo, "/acoustic/latest", &json);
      if (isEvent && threat != "LOW") {
         Firebase.RTDB.pushJSON(&fbdo, "/acoustic/events", &json);
         Serial.println("[FIREBASE] New acoustic event logged!");
      }
    }
  }

  // Small delay to prevent tight loop from starving WiFi task
  delay(10);
}
