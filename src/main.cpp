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
// SERVICE_UUIDはHeater/AutoAirAdjustで共通のため、接続先の判別はアドバタイズ名で行う
#define SERVICE_UUID "7c44181A-c1a4-4635-a119-b490ed272552"

// Chibi-T_Furoshiki_Heater (M5DinMeter) 用キャラクタリスティック
#define HEATER_DEVICE_NAME "M5Din Furoshiki Heater"
#define HEATER_CHARACTERISTIC_UUID "7c442A00-c1a4-4635-a119-b490ed272552"
#define HEATER_NOTIFY_CHARACTERISTIC_UUID "7c442A6E-c1a4-4635-a119-b490ed272552"

// Chibi-T_Furoshiki_AutoAirAdjust (M5Core2) 用キャラクタリスティック
#define AUTOAIR_DEVICE_NAME "ChibiT-AutoAirAdjust"
#define AUTOAIR_CHARACTERISTIC_UUID "c9f878f1-c311-4452-ae5e-e813b4fe057d"
#define AUTOAIR_NOTIFY_CHARACTERISTIC_UUID "1d25ec49-e19c-4bb6-8c36-5dc8d8aaaebe"

#define SCAN_DURATION_SEC 5  // 1回のスキャン時間（秒）。特に意味のある値ではない

static unsigned long ledOnTime = 0;  // LED点灯開始時刻
static const unsigned long LED_ON_DURATION = 500;  // LED点灯時間（ミリ秒）
static const size_t RX_BUFFER_MAX = 256;  // Notify受信バッファの最大サイズ

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

#define PERIPH_HEATER 0
#define PERIPH_AUTOAIR 1
#define PERIPH_COUNT 2

// ペリフェラル1台分の接続状態・UUID・BLEオブジェクトをまとめて保持する
// (Heater/AutoAirAdjustの2台構成に特化しており、3台目以降への汎用対応は意図しない)
struct PeripheralContext
{
  const char *name;                    // アドバタイズ名（判別用）
  BLEUUID charUUID;                    // 存在確認用キャラクタリスティック
  BLEUUID notifyCharUUID;              // Notifyキャラクタリスティック
  BLEAdvertisedDevice *pPeripheral;    // スキャンで発見したデバイス
  BLERemoteCharacteristic *pRemoteCharacteristic;
  BLERemoteCharacteristic *pNotifyCharacteristic;
  int8_t state;                        // STATE_IDLE / STATE_DO_CONNECT / STATE_CONNECTED
  String rxBuffer;                     // このペリフェラル専用のNotify受信バッファ
};

static BLEUUID serviceUUID(SERVICE_UUID);

static PeripheralContext peripherals[PERIPH_COUNT] = {
    // Heater: エンジン温度(タグなし文字列)を送信
    {HEATER_DEVICE_NAME, BLEUUID(HEATER_CHARACTERISTIC_UUID), BLEUUID(HEATER_NOTIFY_CHARACTERISTIC_UUID),
     nullptr, nullptr, nullptr, STATE_IDLE, ""},
    // AutoAirAdjust: 1次/2次側空気圧・燃圧(PRI:/SEC:/FUEL:タグ付き)を送信
    {AUTOAIR_DEVICE_NAME, BLEUUID(AUTOAIR_CHARACTERISTIC_UUID), BLEUUID(AUTOAIR_NOTIFY_CHARACTERISTIC_UUID),
     nullptr, nullptr, nullptr, STATE_IDLE, ""},
};

static BLEScan *pBLEScan;  // setup()で取得し使い回す

// 指定ペリフェラルの状態を初期化し、次のscan()で再発見できるようにする
static void resetPeripheral(int idx)
{
  PeripheralContext &p = peripherals[idx];
  if (p.pPeripheral != nullptr)
  {
    delete p.pPeripheral;
    p.pPeripheral = nullptr;
  }
  p.pRemoteCharacteristic = nullptr;
  p.pNotifyCharacteristic = nullptr;
  p.rxBuffer = "";
  p.state = STATE_IDLE;
}

// 未発見(STATE_IDLE)のペリフェラルが1台でも残っていればスキャンが必要
static bool needsScan()
{
  for (int i = 0; i < PERIPH_COUNT; i++)
  {
    if (peripherals[i].state == STATE_IDLE)
    {
      return true;
    }
  }
  return false;
}

class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks
{
  void onResult(BLEAdvertisedDevice advertisedDevice)
  {
    Serial.printf("Advertised Device: %s \n", advertisedDevice.toString().c_str());
    if (!advertisedDevice.haveServiceUUID() || !advertisedDevice.isAdvertisingService(serviceUUID))
    {
      return;
    }

    // Service UUIDはHeater/AutoAirAdjustで共通のため、アドバタイズ名でどちらかを判別する
    String name = advertisedDevice.getName().c_str();
    for (int i = 0; i < PERIPH_COUNT; i++)
    {
      if (peripherals[i].pPeripheral != nullptr)
      {
        continue;  // 既に発見済み
      }
      if (name != peripherals[i].name)
      {
        continue;
      }
      Serial.printf("Device found! (%s)\n", peripherals[i].name);
      peripherals[i].pPeripheral = new BLEAdvertisedDevice(advertisedDevice);
      peripherals[i].state = STATE_DO_CONNECT;
      break;
    }

    // 両方とも発見済みならこれ以上スキャンを続ける必要はない
    if (peripherals[PERIPH_HEATER].pPeripheral != nullptr &&
        peripherals[PERIPH_AUTOAIR].pPeripheral != nullptr)
    {
      advertisedDevice.getScan()->stop();
    }
  }
};

class MyClientCallbacks : public BLEClientCallbacks
{
public:
  explicit MyClientCallbacks(int idx) : peripheralIndex(idx) {}

  void onConnect(BLEClient *pclient)
  {
    Serial.printf("onConnect (%s)\n", peripherals[peripheralIndex].name);
    peripherals[peripheralIndex].state = STATE_CONNECTED;
  }

  void onDisconnect(BLEClient *pclient)
  {
    Serial.printf("onDisconnect (%s)\n", peripherals[peripheralIndex].name);
    resetPeripheral(peripheralIndex);  // 再スキャンで再発見できるようにする
  }

private:
  int peripheralIndex;
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
  // どちらのペリフェラルからのNotifyかをCharacteristic UUIDで判別する
  int idx = -1;
  for (int i = 0; i < PERIPH_COUNT; i++)
  {
    if (pBLERemoteCharacteristic->getUUID().equals(peripherals[i].notifyCharUUID))
    {
      idx = i;
      break;
    }
  }
  if (idx < 0)
  {
    return;  // 想定外のCharacteristic（基本的に発生しない）
  }
  String &rxBuffer = peripherals[idx].rxBuffer;

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
  // 5秒間のブロッキングスキャン。結果は使い切ったら不要なのでクリアする(is_continue=false)。
  // 未発見のペリフェラルが残っている限りloop()から繰り返し呼ばれる
  pBLEScan->start(SCAN_DURATION_SEC, false);
}

bool connect(int idx)
{
  PeripheralContext &p = peripherals[idx];

  BLEClient *pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallbacks(idx));
  if (!pClient->connect(p.pPeripheral))
  {
    return false;
  }

  BLERemoteService *pRemoteService = pClient->getService(serviceUUID);
  if (pRemoteService == nullptr)
  {
    Serial.printf("[%s] Failed to find our service UUID: %s\n", p.name, serviceUUID.toString().c_str());
    pClient->disconnect();
    return false;
  }
  Serial.printf("[%s] - Found our service\n", p.name);

  // Writeで使うためにCharacteristicを保持しておく
  p.pRemoteCharacteristic = pRemoteService->getCharacteristic(p.charUUID);
  if (p.pRemoteCharacteristic == nullptr)
  {
    Serial.printf("[%s] Failed to find our characteristic UUID: %s\n", p.name, p.charUUID.toString().c_str());
    pClient->disconnect();
    return false;
  }
  Serial.printf("[%s] - Found our characteristic\n", p.name);

  if (p.pRemoteCharacteristic->canRead())
  {
    String value = p.pRemoteCharacteristic->readValue();
    Serial.printf("[%s] The characteristic value was: %s\n", p.name, value.c_str());
  }

  if (!p.pRemoteCharacteristic->canWrite())
  {
    Serial.printf("[%s] Characteristic is not writable\n", p.name);
    pClient->disconnect();
    return false;
  }

  p.pNotifyCharacteristic = pRemoteService->getCharacteristic(p.notifyCharUUID);
  if (p.pNotifyCharacteristic == nullptr)
  {
    Serial.printf("[%s] Failed to find our notify characteristic UUID: %s\n", p.name, p.notifyCharUUID.toString().c_str());
    pClient->disconnect();
    return false;
  }

  if (p.pNotifyCharacteristic->canNotify())
  {
    p.pNotifyCharacteristic->registerForNotify(notifyCallback);
    Serial.printf("[%s] - Registered for notify\n", p.name);
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

  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  // Interval, Windowはdefaultの値で動作して問題なさそうなため設定しない。
  // Heater/AutoAirAdjustいずれもデバイス名が128bit Service UUIDと合わせるとレガシー広告パケットに
  // 収まらないため、名前はScan Responseに回される。名前でのペリフェラル判別にはScan Responseの
  // 取得が必須のため、アクティブスキャンにする(パッシブでは名前が空になり判別できない)
  // https://github.com/espressif/arduino-esp32/blob/master/libraries/BLE/examples/BLE_scan/BLE_scan.ino#L27
  pBLEScan->setActiveScan(true);

  // setup時点で1回スキャンを実行。以降は未発見のペリフェラルが残っていればloop()で継続する
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

  // Heater/AutoAirAdjust それぞれ独立に接続処理を進める（片方が失敗してももう片方は継続動作する）
  for (int i = 0; i < PERIPH_COUNT; i++)
  {
    if (peripherals[i].state != STATE_DO_CONNECT)
    {
      continue;
    }
    if (connect(i))
    {
      Serial.printf("Connected to server (%s)\n", peripherals[i].name);
    }
    else
    {
      Serial.printf("Failed to connect (%s)\n", peripherals[i].name);
      resetPeripheral(i);  // 次のscan()で再発見・再接続を試みられるようにする
    }
  }

  // 未発見のペリフェラルが残っていればスキャンを継続する
  // (両方接続済みならscan()を呼ばずCPUを消費しない。切断されると該当エントリがIDLEに戻り自動的に再開する)
  if (needsScan())
  {
    scan();
  }
}