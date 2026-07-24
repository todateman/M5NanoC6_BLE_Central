// 参考
// https://github.com/espressif/arduino-esp32/blob/master/libraries/BLE/examples/BLE_client/BLE_client.ino
// https://qiita.com/Teach/items/629c338da05a3134a1eb#%E3%82%B9%E3%83%86%E3%83%83%E3%83%976-notify
// https://github.com/teach310/AtomLiteSample/blob/d275df573c59a7d9ced38330498d182313b69bd8/src/main.cpp

#include <Arduino.h>
#include <BLEDevice.h>
#include <Wire.h>

// Grove Port -> I2C (M5Stack Basicをマスターとするスレーブとして動作)
#define I2C_SCL_PIN 1
#define I2C_SDA_PIN 2
#define I2C_SLAVE_ADDR 0x08
#define I2C_FRAME_SIZE 32  // 1byte長さ + 最大31byteデータ

// マスターが要求するデータの種類を表すコマンド
#define CMD_ENGINE_TEMP 0x01
#define CMD_PRI_PRE 0x02
#define CMD_SEC_PRE 0x03
#define CMD_FUEL_PRE 0x04

// M5Core2 (Chibi-T_Furoshiki_AutoAirAdjust) は1本のNotify Characteristicで
// 3種類のセンサーデータを送るため、先頭のタグ文字列で種別を判別する
#define TAG_PRI_PRE "PRI:"
#define TAG_SEC_PRE "SEC:"
#define TAG_FUEL_PRE "FUEL:"

#define BLUE_LED_PIN 7  // 青色LED端子番号

// ServerのBLE サービスとキャラクタリスティックのUUIDを定義 https://www.uuidgenerator.net/version4
#define SERVICE_UUID "7c44181A-c1a4-4635-a119-b490ed272552"
#define CHARACTERISTIC_UUID "7c442A00-c1a4-4635-a119-b490ed272552"
#define NOTIFY_CHARACTERISTIC_UUID "7c442A6E-c1a4-4635-a119-b490ed272552"

static BLEUUID serviceUUID(SERVICE_UUID);
static BLEUUID charUUID(CHARACTERISTIC_UUID);
static BLEUUID notifyCharUUID(NOTIFY_CHARACTERISTIC_UUID);
static BLEAdvertisedDevice *pPeripheral;
static BLERemoteCharacteristic *pRemoteCharacteristic;
static BLERemoteCharacteristic *pNotifyCharacteristic;

static int8_t state = 0;
static unsigned long ledOnTime = 0;  // LED点灯開始時刻
static const unsigned long LED_ON_DURATION = 500;  // LED点灯時間（ミリ秒）

// BLE受信データバッファ（改行までのデータを保持）
static String rxBuffer = "";
static const size_t RX_BUFFER_MAX = 256;  // 最大バッファサイズ

// 各データのI2C送信用フレーム（先頭1byteが長さ、以降が実データ）。M5Stack Basicからの要求時に最新値を返す
static uint8_t engineTempFrame[I2C_FRAME_SIZE] = {0};
static uint8_t priPreFrame[I2C_FRAME_SIZE] = {0};
static uint8_t secPreFrame[I2C_FRAME_SIZE] = {0};
static uint8_t fuelPreFrame[I2C_FRAME_SIZE] = {0};

// マスターが直前に書き込んできたコマンド（onReceiveで更新し、onRequestで参照する）
static uint8_t lastCommand = 0x00;

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

// 文字列データを長さプレフィックス付きのI2C送信用フレームに変換して格納する
static void updateFrame(uint8_t *frame, const String &value)
{
  size_t dataLen = min(value.length(), (size_t)(I2C_FRAME_SIZE - 1));
  frame[0] = (uint8_t)dataLen;
  memcpy(&frame[1], value.c_str(), dataLen);
  if (dataLen < I2C_FRAME_SIZE - 1)
  {
    memset(&frame[1 + dataLen], 0, I2C_FRAME_SIZE - 1 - dataLen);
  }
}

static void notifyCallback(
    BLERemoteCharacteristic *pBLERemoteCharacteristic,
    uint8_t *pData,
    size_t length,
    bool isNotify)
{
  digitalWrite(BLUE_LED_PIN, HIGH); // 本体LED点灯
  ledOnTime = millis();  // LED点灯開始時刻を記録
  Serial.print("Notify callback for characteristic ");
  Serial.print(pBLERemoteCharacteristic->getUUID().toString().c_str());
  Serial.print(" of data length ");
  Serial.println(length);
  
  // 受信データをバッファに追加
  for (size_t i = 0; i < length; i++)
  {
    char c = (char)pData[i];
    
    if (c == '\n')
    {
      // 改行を受信 → メッセージ完成
      Serial.print("Complete message: ");
      Serial.println(rxBuffer.c_str());

      // 先頭のタグでデータ種別を判別し、対応するフレームを更新する
      // （タグが無い場合は従来どおりCMD_ENGINE_TEMP用として扱う）
      if (rxBuffer.startsWith(TAG_PRI_PRE))
      {
        updateFrame(priPreFrame, rxBuffer.substring(strlen(TAG_PRI_PRE)));
        Serial.println("I2C frame updated: PRI_PRE");
      }
      else if (rxBuffer.startsWith(TAG_SEC_PRE))
      {
        updateFrame(secPreFrame, rxBuffer.substring(strlen(TAG_SEC_PRE)));
        Serial.println("I2C frame updated: SEC_PRE");
      }
      else if (rxBuffer.startsWith(TAG_FUEL_PRE))
      {
        updateFrame(fuelPreFrame, rxBuffer.substring(strlen(TAG_FUEL_PRE)));
        Serial.println("I2C frame updated: FUEL_PRE");
      }
      else
      {
        updateFrame(engineTempFrame, rxBuffer);
        Serial.println("I2C frame updated: ENGINE_TEMP");
      }

      // バッファクリア
      rxBuffer = "";
    }
    else if (c != '\r')
    {
      // キャリッジリターンは無視、その他の文字をバッファに追加
      if (rxBuffer.length() < RX_BUFFER_MAX)
      {
        rxBuffer += c;
      }
      else
      {
        // バッファオーバーフロー対策
        Serial.println("ERROR: RX buffer overflow!");
        rxBuffer = "";
      }
    }
  }
  
  if (rxBuffer.length() > 0)
  {
    Serial.print("Buffered data: ");
    Serial.println(rxBuffer.c_str());
  }
}

// M5Stack Basic（I2Cマスター）からのコマンド書き込み受信時に呼ばれる
void onI2CReceive(int numBytes)
{
  if (numBytes > 0)
  {
    lastCommand = Wire.read();
  }
  // 想定外の追加バイトは読み捨てる
  while (Wire.available())
  {
    Wire.read();
  }
}

// M5Stack Basic（I2Cマスター）からの読み出し要求時に呼ばれる
void onI2CRequest()
{
  switch (lastCommand)
  {
  case CMD_ENGINE_TEMP:
    Wire.write(engineTempFrame, I2C_FRAME_SIZE);
    break;
  case CMD_PRI_PRE:
    Wire.write(priPreFrame, I2C_FRAME_SIZE);
    break;
  case CMD_SEC_PRE:
    Wire.write(secPreFrame, I2C_FRAME_SIZE);
    break;
  case CMD_FUEL_PRE:
    Wire.write(fuelPreFrame, I2C_FRAME_SIZE);
    break;
  default:
  {
    // 未知のコマンド: 長さ0の空フレームを返す
    uint8_t emptyFrame[I2C_FRAME_SIZE] = {0};
    Wire.write(emptyFrame, I2C_FRAME_SIZE);
    break;
  }
  }
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

  pinMode(BLUE_LED_PIN, OUTPUT); // 本体LED青

  digitalWrite(BLUE_LED_PIN, LOW);  // 本体LED消灯

  // I2Cスレーブとして初期化し、M5Stack Basicからの要求に応答する
  Wire.begin(I2C_SLAVE_ADDR, I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.onReceive(onI2CReceive);
  Wire.onRequest(onI2CRequest);

  BLEDevice::init("M5NanoC6 BLE Client");

  // setupで単発実行。繰り返し実行するならloopに配置する必要がある
  scan();
}

void loop()
{
  // LED点灯時間後に自動消灯
  if (ledOnTime > 0 && (millis() - ledOnTime) >= LED_ON_DURATION)
  {
    digitalWrite(BLUE_LED_PIN, LOW);  // 本体LED消灯
    ledOnTime = 0;
  }

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