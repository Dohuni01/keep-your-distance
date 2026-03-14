#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

#define LED_PIN 2

BLEScan* pBLEScan;
int rssiThreshold = -72;

// 안드로이드에서 service data에 사용한 UUID
BLEUUID targetServiceDataUUID("0000180F-0000-1000-8000-00805F9B34FB");

// 안드로이드에서 보내는 공통 service data
String TARGET_DATA = "SAFE";

class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    int rssi = advertisedDevice.getRSSI();

    // 1. Service Data 없으면 무시
    if (!advertisedDevice.haveServiceData()) {
      return;
    }

    // 2. Service Data UUID 읽기
    BLEUUID dataUuid = advertisedDevice.getServiceDataUUID();

    // 3. UUID가 다르면 무시
    if (!dataUuid.equals(targetServiceDataUUID)) {
      return;
    }

    // 4. Service Data 읽기
    String receivedData = advertisedDevice.getServiceData();

    Serial.println("====================");
    Serial.print("Service Data UUID: ");
    Serial.println(dataUuid.toString().c_str());
    Serial.print("Service Data: ");
    Serial.print(receivedData);
    Serial.print(" / RSSI: ");
    Serial.println(rssi);

    // 5. SAFE일 때만 거리 판단
    if (receivedData == TARGET_DATA) {
      if (rssi >= rssiThreshold) {
        Serial.println(">>> 3m 이내");
        digitalWrite(LED_PIN, HIGH);
      } else {
        Serial.println(">>> 3m 밖");
        digitalWrite(LED_PIN, LOW);
      }
    } else {
      Serial.println(">>> SAFE 아님");
      digitalWrite(LED_PIN, LOW);
    }

    Serial.println("====================");
  }
};

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  BLEDevice::init("");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);

  Serial.println("BLE 스캔 시작");
}

void loop() {
  digitalWrite(LED_PIN, LOW);
  pBLEScan->start(3, false);
  pBLEScan->clearResults();
  delay(500);
}