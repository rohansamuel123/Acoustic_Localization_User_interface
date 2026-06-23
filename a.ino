#include <WiFi.h>
#include <esp_now.h>

#define MIC_PIN 34

char NODE_ID = 'A';

// Master MAC Address
uint8_t masterAddress[] = {0xB4,0xBF,0xE9,0x0E,0x8B,0x98};

typedef struct {
  char node;
  int peak;
  unsigned long timestamp;
} SensorData;

SensorData packet;

float background = 1800;

void OnDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status)
{
  Serial.print("Send Status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Success" : "Fail");
}

void setup()
{
  Serial.begin(115200);

  WiFi.mode(WIFI_STA);

  if (esp_now_init() != ESP_OK)
  {
    Serial.println("ESP-NOW Init Failed");
    return;
  }

  esp_now_register_send_cb(OnDataSent);

  esp_now_peer_info_t peerInfo = {};

  memcpy(peerInfo.peer_addr, masterAddress, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK)
  {
    Serial.println("Failed to add peer");
    return;
  }

  Serial.println("Node Ready");
}

void loop()
{
  int sample = analogRead(MIC_PIN);

  background = background * 0.99 + sample * 0.01;

  int threshold = background + 800;

  if(sample > threshold)
  {
    packet.node = NODE_ID;
    packet.peak = sample;
    packet.timestamp = millis();

    esp_now_send(
      masterAddress,
      (uint8_t *)&packet,
      sizeof(packet)
    );

    Serial.print("EVENT: ");
    Serial.println(sample);

    delay(500);
  }

  delay(5);
}