// 参考
// https://github.com/espressif/arduino-esp32/blob/master/libraries/BLE/examples/BLE_client/BLE_client.ino
// https://qiita.com/Teach/items/629c338da05a3134a1eb#%E3%82%B9%E3%83%86%E3%83%83%E3%83%976-notify
// https://github.com/teach310/AtomLiteSample/blob/d275df573c59a7d9ced38330498d182313b69bd8/src/main.cpp

#include <Arduino.h>
#include <BLEDevice.h>

// I2C -> SoftwareSerial
#include "SoftwareSerial.h"
#define rxPin 1   // SCL
#define txPin 2   // SCA
// Set up a new SoftwareSerial object
SoftwareSerial SoftSerial;

// ServerのBLE サービスとキャラクタリスティックのUUIDを定義 https://www.uuidgenerator.net/version4
#define SERVICE_UUID "7c445963-c1a4-4635-a119-b490ed272552"
#define CHARACTERISTIC_UUID "ced47adc-db99-46a2-9248-cb70b7bd836f"
#define NOTIFY_CHARACTERISTIC_UUID "dca30b2b-658e-484a-a7fc-974c08800429"

static BLEUUID serviceUUID(SERVICE_UUID);
static BLEUUID charUUID(CHARACTERISTIC_UUID);
static BLEUUID notifyCharUUID(NOTIFY_CHARACTERISTIC_UUID);
static BLEAdvertisedDevice *pPeripheral;
static BLERemoteCharacteristic *pRemoteCharacteristic;
static BLERemoteCharacteristic *pNotifyCharacteristic;

static int8_t state = 0;

#define STATE_IDLE 0
#define STATE_DO_CONNECT 1
#define STATE_CONNECTED 3

class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks
{
  void onResult(BLEAdvertisedDevice advertisedDevice)
  {
    Serial.printf("Advertised Device: %s \n", advertisedDevice.toString().c_str());
    if (advertisedDevice.haveServiceUUID() && advertisedDevice.isAdvertisingService(serviceUUID))
    {
      Serial.println("Device found!");
      pPeripheral = new BLEAdvertisedDevice(advertisedDevice);
      advertisedDevice.getScan()->stop();
      state = STATE_DO_CONNECT;
    }
  }
};

class MyClientCallbacks : public BLEClientCallbacks
{
  void onConnect(BLEClient *pclient)
  {
    Serial.println("onConnect");
    state = STATE_CONNECTED;
  }

  void onDisconnect(BLEClient *pclient)
  {
    Serial.println("onDisconnect");
    state = STATE_IDLE;
  }
};

static void notifyCallback(
    BLERemoteCharacteristic *pBLERemoteCharacteristic,
    uint8_t *pData,
    size_t length,
    bool isNotify)
{
  Serial.print("Notify callback for characteristic ");
  Serial.print(pBLERemoteCharacteristic->getUUID().toString().c_str());
  Serial.print(" of data length ");
  Serial.println(length);
  Serial.print("data: ");
  Serial.write((char *)pData, length);
  Serial.println();
  // Output GrovePort
  SoftSerial.write((char *)pData, length);
  SoftSerial.println();
}

void scan()
{
  BLEScan *pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  // Interval, Windowはdefaultの値で動作して問題なさそうなため設定しない。
  // アドバタイズを受信するだけのためパッシブスキャン
  // trueにすると高速にペリフェラルを検出できるかもしれないが、パッシブでもすぐ検出できるため必要性は感じていない
  // https://github.com/espressif/arduino-esp32/blob/master/libraries/BLE/examples/BLE_scan/BLE_scan.ino#L27
  pBLEScan->setActiveScan(false);

  // スキャン5秒には特に意味はない。
  // スキャン結果を残しておく必要がないため、終わったクリアする。そのためにis_continueはfalseにする
  pBLEScan->start(5, false);
}

bool connect()
{
  BLEClient *pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallbacks());
  if (!pClient->connect(pPeripheral))
  {
    return false;
  }

  BLERemoteService *pRemoteService = pClient->getService(serviceUUID);
  if (pRemoteService == nullptr)
  {
    Serial.print("Failed to find our service UUID: ");
    Serial.println(serviceUUID.toString().c_str());
    pClient->disconnect();
    return false;
  }
  Serial.println(" - Found our service");

  // Writeで使うためにCharacteristicを保持しておく
  pRemoteCharacteristic = pRemoteService->getCharacteristic(charUUID);
  if (pRemoteCharacteristic == nullptr)
  {
    Serial.print("Failed to find our characteristic UUID: ");
    Serial.println(charUUID.toString().c_str());
    pClient->disconnect();
    return false;
  }
  Serial.println(" - Found our characteristic");

  if (pRemoteCharacteristic->canRead())
  {
    String value = pRemoteCharacteristic->readValue();
    Serial.print("The characteristic value was: ");
    Serial.println(value.c_str());
  }

  if (!pRemoteCharacteristic->canWrite())
  {
    Serial.println("Characteristic is not writable");
    pClient->disconnect();
    return false;
  }

  pNotifyCharacteristic = pRemoteService->getCharacteristic(notifyCharUUID);
  if (pNotifyCharacteristic == nullptr)
  {
    Serial.print("Failed to find our notify characteristic UUID: ");
    Serial.println(notifyCharUUID.toString().c_str());
    pClient->disconnect();
    return false;
  }

  if (pNotifyCharacteristic->canNotify())
  {
    pNotifyCharacteristic->registerForNotify(notifyCallback);
    Serial.println(" - Registered for notify");
  }

  return true;
}

void setup()
{
  Serial.begin(115200);

    // Define pin modes for TX and RX
    pinMode(rxPin, INPUT);
    pinMode(txPin, OUTPUT);
    
    // Set the baud rate for the SoftwareSerial object
    SoftSerial.begin(115200, SWSERIAL_8N1, rxPin, txPin , false, 256);

  BLEDevice::init("M5NanoC6 BLE Client");

  // setupで単発実行。繰り返し実行するならloopに配置する必要がある
  scan();
}

void loop()
{
  switch (state)
  {
  case STATE_DO_CONNECT:
    if (connect())
    {
      Serial.println("Connected to server");
    }
    else
    {
      Serial.println("Failed to connect");
      state = STATE_IDLE;
    }
    break;
  case STATE_CONNECTED:
    break;
  default:
    break;
  }
}