// 参考
// https://github.com/espressif/arduino-esp32/blob/master/libraries/BLE/examples/BLE_client/BLE_client.ino
// https://qiita.com/Teach/items/629c338da05a3134a1eb#%E3%82%B9%E3%83%86%E3%83%83%E3%83%976-notify
// https://github.com/teach310/AtomLiteSample/blob/d275df573c59a7d9ced38330498d182313b69bd8/src/main.cpp

#include <Arduino.h>
#include <BLEDevice.h>
#include "driver/i2c_slave.h"

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

// マスターが直前に書き込んできたコマンド（受信コールバックで更新し、送信処理で参照する）
static volatile uint8_t lastCommand = 0x00;

// ESP-IDFネイティブI2Cスレーブドライバ関連
// (ESP32-C6はarduino-esp32のWireライブラリのスレーブ読み取り要求(onRequest)が
//  構造的に発火しない既知の制限があるため、Wireを使わずdriver/i2c_slave.hを直接使う)
//
// 重要: ESP-IDF(v5.5)のi2c_slave.c実装では、on_stretch_occurコールバックが
// 返った直後にドライバ側が無条件でクロックストレッチを解除する(i2c_ll_slave_clear_stretch)。
// そのためストレッチ発生(＝読み取り開始)を検知してからタスク経由で応答データを
// 用意していては確実に間に合わない。応答フレームは、その直前に完了している
// 「コマンドバイトの書き込み(onI2CReceiveDone)」の時点で用意し、i2c_slave_transmit()で
// TXリングバッファへ積んでおく必要がある(読み取り開始時にはFIFOに既にデータがある状態にする)。
static i2c_slave_dev_handle_t i2cSlaveHandle = nullptr;
static TaskHandle_t i2cSlaveTaskHandle = nullptr;
static uint8_t i2cCmdBuffer[1];  // 受信コマンドバイト用バッファ
static volatile uint32_t i2cStretchEventCount = 0;  // 診断用(ISRからのインクリメントのみ、ログはタスク側で出す)
// setup()と再初期化(resetI2CSlaveDevice)の両方から使うためファイルスコープに置く
static i2c_slave_config_t i2cSlaveConfig = {};

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
    // 周囲のBLEデバイスすべてに対して呼ばれるため、無条件のSerial.printf(CORE_DEBUG_LEVELの
    // 制御対象外で常に出力される)は周辺機器の多い環境ではCPU/シリアル送信時間を大きく消費し、
    // I2C応答タスクのスケジューリング遅延（TXリングバッファのオーバーフロー）の一因になっていた。
    // 対象のService UUIDを持つデバイスのみログを出す
    if (!advertisedDevice.haveServiceUUID() || !advertisedDevice.isAdvertisingService(serviceUUID))
    {
      return;
    }
    Serial.printf("Advertised Device: %s \n", advertisedDevice.toString().c_str());

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

// 文字列データを長さプレフィックス付きのI2C送信用フレームに変換して格納する。
// 末尾1byte(frame[I2C_FRAME_SIZE-1])はコマンドエコー用に予約し、ここでは触れない
// (実際の値はi2cSlaveTaskが送信直前に設定する。マスター側でコマンドと応答のズレを
//  検知できるようにするための仕組み。詳細はi2cSlaveTaskのコメント参照)
static void updateFrame(uint8_t *frame, const String &value)
{
  size_t dataLen = min(value.length(), (size_t)(I2C_FRAME_SIZE - 2));
  frame[0] = (uint8_t)dataLen;
  memcpy(&frame[1], value.c_str(), dataLen);
  if (dataLen < I2C_FRAME_SIZE - 2)
  {
    memset(&frame[1 + dataLen], 0, I2C_FRAME_SIZE - 2 - dataLen);
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

// M5Stack Basic（I2Cマスター）からのコマンド書き込み受信完了時にISRコンテキストで呼ばれる。
// この時点でコマンドに対応する応答フレームをタスクに用意させる（読み取り開始を待ってからでは遅い）
static bool onI2CReceiveDone(i2c_slave_dev_handle_t handle,
                              const i2c_slave_rx_done_event_data_t *evt, void *arg)
{
  lastCommand = evt->buffer[0];
  BaseType_t hpw = pdFALSE;
  vTaskNotifyGiveFromISR(i2cSlaveTaskHandle, &hpw);
  return hpw == pdTRUE;
}

// クロックストレッチ発生時にISRコンテキストで呼ばれる（現在はflags.stretch_en=0のため通常は
// 発火しない。診断用に残してあるだけで、カウントが増えていれば何らかの理由でストレッチが
// 発生していることが分かる。ドライバはこのコールバックが返った直後に無条件でストレッチを
// 解除するため、ここでは応答データを用意できない（Serial出力もISRからは非安全なため行わない）
static bool onI2CStretch(i2c_slave_dev_handle_t handle,
                          const i2c_slave_stretch_event_data_t *evt, void *arg)
{
  i2cStretchEventCount = i2cStretchEventCount + 1;  // volatileへの++はC++20で非推奨のため加算代入で書く
  return false;
}

// I2Cスレーブデバイスを(再)初期化する。i2cSlaveConfigは事前に設定済みであること
static bool initI2CSlaveDevice()
{
  esp_err_t err = i2c_new_slave_device(&i2cSlaveConfig, &i2cSlaveHandle);
  if (err != ESP_OK)
  {
    Serial.printf("[I2C] i2c_new_slave_device failed: %s\n", esp_err_to_name(err));
    return false;
  }

  i2c_slave_event_callbacks_t i2cCallbacks = {};
  i2cCallbacks.on_recv_done = onI2CReceiveDone;
  i2cCallbacks.on_stretch_occur = onI2CStretch;
  err = i2c_slave_register_event_callbacks(i2cSlaveHandle, &i2cCallbacks, nullptr);
  if (err != ESP_OK)
  {
    Serial.printf("[I2C] i2c_slave_register_event_callbacks failed: %s\n", esp_err_to_name(err));
    return false;
  }

  i2c_slave_receive(i2cSlaveHandle, i2cCmdBuffer, sizeof(i2cCmdBuffer));  // 受信をアーム
  return true;
}

// 直近のデバイス再作成から最低これだけ間隔を空ける(ミリ秒)
static const unsigned long I2C_RESET_COOLDOWN_MS = 50;
static unsigned long lastI2CResetMillis = 0;

// TXリングバッファが詰まる等でi2c_slave_transmit()が失敗し続ける場合の自己修復。
// デバイスを作り直すことで未消費のリングバッファ内容を強制的に破棄する
// (タスクコンテキストから呼ぶこと。ISRからは呼べない)
static void resetI2CSlaveDevice()
{
  unsigned long now = millis();
  if (i2cSlaveHandle && (now - lastI2CResetMillis) < I2C_RESET_COOLDOWN_MS)
  {
    // i2c_del_slave_device+i2c_new_slave_deviceはGPIOのI2Cペリフェラルへの
    // 再アタッチを伴い、それ自体がSDA/SCL上に瞬間的なグリッチを生んで
    // さらなる誤検出を誘発している可能性がある。短時間に連続で発生した場合は
    // デバイスの作り直しまでは行わず、受信の再アームのみで様子を見る
    Serial.println("[I2C] transmit stuck, but skipping full reset (cooldown)");
    i2c_slave_receive(i2cSlaveHandle, i2cCmdBuffer, sizeof(i2cCmdBuffer));
    return;
  }

  Serial.println("[I2C] resetting slave device (transmit stuck)");
  lastI2CResetMillis = now;
  if (i2cSlaveHandle)
  {
    i2c_del_slave_device(i2cSlaveHandle);
    i2cSlaveHandle = nullptr;
  }
  if (!initI2CSlaveDevice())
  {
    // 再初期化に失敗してもクラッシュさせない(BLE接続は維持する)。
    // 次のコマンド受信で再度失敗が続く場合はログで分かる
    Serial.println("[I2C] slave device re-init failed");
  }
}

// コマンド受信のたびにタスクコンテキストで呼ばれ、応答フレームをTXリングバッファに積んでおく
// （ISR内でi2c_slave_transmit/receiveを呼ばないための橋渡し）
static void i2cSlaveTask(void *arg)
{
  static uint8_t emptyFrame[I2C_FRAME_SIZE] = {0};
  // 送信用の一時バッファ。共有のフレーム配列(engineTempFrame等)を直接書き換えず、
  // ここにコピーしてからコマンドエコーを埋め込む(BLE Notifyコールバック側からの
  // 書き込みと競合させないため)
  static uint8_t txBuffer[I2C_FRAME_SIZE];
  for (;;)
  {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    uint8_t *frame = emptyFrame;  // 未知のコマンドは長さ0の空フレームを返す
    uint8_t cmd = lastCommand;
    switch (cmd)
    {
    case CMD_ENGINE_TEMP:
      frame = engineTempFrame;
      break;
    case CMD_PRI_PRE:
      frame = priPreFrame;
      break;
    case CMD_SEC_PRE:
      frame = secPreFrame;
      break;
    case CMD_FUEL_PRE:
      frame = fuelPreFrame;
      break;
    }

    // 何らかの理由（バスノイズ等）でマスター側が受け取った応答が要求と異なる
    // コマンドの分だった場合にマスター側で検知・リトライできるよう、フレーム末尾
    // 1byteに要求されたコマンドをそのままエコーバックする
    memcpy(txBuffer, frame, I2C_FRAME_SIZE);
    txBuffer[I2C_FRAME_SIZE - 1] = cmd;

    // タイムアウトは短めにする。詰まっている場合はここで長時間ブロックせず
    // 素早く検知してresetI2CSlaveDevice()に切り替えるため
    esp_err_t err = i2c_slave_transmit(i2cSlaveHandle, txBuffer, I2C_FRAME_SIZE, 5);
    // 診断用ログ（動作確認できたら削除/コメントアウトして問題ない）
    Serial.printf("[I2C] cmd=0x%02X transmit=%s stretch_total=%lu\n",
                  cmd, (err == ESP_OK) ? "OK" : esp_err_to_name(err),
                  (unsigned long)i2cStretchEventCount);

    if (err == ESP_OK)
    {
      // 次のコマンドバイト受信に備えて再アーム
      i2c_slave_receive(i2cSlaveHandle, i2cCmdBuffer, sizeof(i2cCmdBuffer));
    }
    else
    {
      // リングバッファ詰まり等、通常の再アームでは復旧しないため
      // デバイスごと作り直して未消費データを破棄する(受信の再アームも兼ねる)
      resetI2CSlaveDevice();
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
  // (ESP32-C6ではArduino Wireのスレーブ読み取り要求(onRequest)が発火しないため、
  //  driver/i2c_slave.hを直接使用する)
  // タスクはコールバック登録より前に生成する(ISRからvTaskNotifyGiveFromISRで直接参照するため)。
  // 優先度は高め(20)にして、コマンド受信からTX準備完了までをできるだけ短くする
  // (マスター側は書き込み直後にすぐ読み取りを行うため、応答準備の遅れがそのまま失敗に直結する)
  xTaskCreate(i2cSlaveTask, "i2c_slave_task", 4096, nullptr, 20, &i2cSlaveTaskHandle);

  // i2cSlaveConfigはファイルスコープの変数(74行目付近)。resetI2CSlaveDevice()での
  // 再初期化時にも同じ設定を使い回すため、ここではローカル変数として再宣言しない
  i2cSlaveConfig.i2c_port = -1;  // 自動選択
  i2cSlaveConfig.sda_io_num = (gpio_num_t)I2C_SDA_PIN;
  i2cSlaveConfig.scl_io_num = (gpio_num_t)I2C_SCL_PIN;
  i2cSlaveConfig.clk_source = I2C_CLK_SRC_DEFAULT;
  // 未読了のフレームが溜まった場合の保険として、I2C_FRAME_SIZE(32)の8フレーム分を確保。
  // 突発的な多重コマンド受信があってもリセット(自体がバス上のグリッチを誘発しうる)に
  // 頼らず吸収できる余地を増やす (2→4→8フレーム分と段階的に拡大)
  i2cSlaveConfig.send_buf_depth = 256;
  i2cSlaveConfig.slave_addr = I2C_SLAVE_ADDR;
  i2cSlaveConfig.addr_bit_len = I2C_ADDR_BIT_LEN_7;
  i2cSlaveConfig.intr_priority = 3;  // BLEスキャン処理との競合による割り込み遅延を減らすため明示的に高めに設定
  // クロックストレッチは無効化する。BLEスキャン等でCPUが混雑している状況では
  // ストレッチの解除(ISR処理)自体が遅延し、マスター側からはSCLが長時間(観測上約1秒)
  // Low に張り付いたように見え、Wire.setTimeOut()では救えないバス全体のスタックを
  // 引き起こしていた。応答フレームはコマンド受信完了(onI2CReceiveDone)の時点で
  // 事前に用意しているため、ストレッチが無くても通常は間に合う。万一間に合わなくても
  // 「ストレッチ無し」なら単に空/古いフレームが返るだけで、バスは長時間ブロックしない
  i2cSlaveConfig.flags.stretch_en = 0;

  // 初期化本体はinitI2CSlaveDevice()に切り出してあり、実行時の自己修復
  // (resetI2CSlaveDevice())からも同じ関数を呼び出す。起動時に失敗する場合は
  // 配線・アドレス設定等の致命的な問題である可能性が高いため、従来通り停止する
  if (!initI2CSlaveDevice())
  {
    Serial.println("[I2C] initial slave device init failed, halting");
    abort();
  }

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