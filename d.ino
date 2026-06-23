#include <WiFi.h>
#include <esp_now.h>

#define MIC_PIN 34

typedef struct {
  char node;
  int peak;
  unsigned long timestamp;
} SensorData;

SensorData incoming;

int A=0;
int B=0;
int C=0;
int D=0;

float background = 1800;

float posX = 0;
float posY = 0;

String threat = "LOW";

void OnDataRecv(
  const esp_now_recv_info_t *recv_info,
  const uint8_t *incomingData,
  int len)
{
  memcpy(&incoming,incomingData,sizeof(incoming));

  switch(incoming.node)
  {
    case 'A':
      A=incoming.peak;
      break;

    case 'B':
      B=incoming.peak;
      break;

    case 'C':
      C=incoming.peak;
      break;
  }
}

void setup()
{
  Serial.begin(115200);

  WiFi.mode(WIFI_STA);

  if(esp_now_init()!=ESP_OK)
  {
    Serial.println("ESP NOW FAILED");
    return;
  }

  esp_now_register_recv_cb(OnDataRecv);

  Serial.println("MASTER READY");
}

void loop()
{
  int sample = analogRead(MIC_PIN);

  background =
      background*0.99 +
      sample*0.01;

  if(sample > background+800)
  {
      D = sample;
  }

  int total = A+B+C+D;

  if(total>0)
  {
      posX =
      (
       A*0 +
       B*4 +
       C*0 +
       D*4
      )/(float)total;

      posY =
      (
       A*0 +
       B*0 +
       C*4 +
       D*4
      )/(float)total;
  }

  int maxPeak=max(max(A,B),max(C,D));

  if(maxPeak>3800)
      threat="HIGH";
  else if(maxPeak>3000)
      threat="MEDIUM";
  else
      threat="LOW";

  Serial.print("{");
  Serial.print("\"A\":"); Serial.print(A); Serial.print(",");
  Serial.print("\"B\":"); Serial.print(B); Serial.print(",");
  Serial.print("\"C\":"); Serial.print(C); Serial.print(",");
  Serial.print("\"D\":"); Serial.print(D); Serial.print(",");
  Serial.print("\"x\":"); Serial.print(posX,2); Serial.print(",");
  Serial.print("\"y\":"); Serial.print(posY,2); Serial.print(",");
  Serial.print("\"threat\":\""); Serial.print(threat); Serial.print("\"");
  Serial.println("}");

  delay(200);
}