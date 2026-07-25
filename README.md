# M5NanoC6 BLE Central

[M5NanoC6](https://docs.m5stack.com/ja/core/M5NanoC6)（ESP32-C6） を使用した BLE Central (クライアント) 実装サンプルです。  
M5Stack シリーズ（ESP32）では Wi-Fi と BLE を同時利用できないという制約があるため、これを回避するために M5NanoC6 を外付け BLE 受信機 (ブリッジ) として用い、取得した BLE Notify データを Grove ポート経由の I2C スレーブとして保持し、M5Stack Basic（I2C マスター）からの読み出し要求に応じて返します。  
（I2Cスレーブアドレス: 0x08, SDA=GPIO2, SCL=GPIO1）

## 特徴

- Arduino フレームワーク (esp32-arduino)
- BLE Central: Heater / AutoAirAdjust 2台の BLE ペリフェラルに**同時接続**（Service UUID は共通のため、アドバタイズ名で判別）→ Notify 購読
- Notify 受信で LED(青) 点灯し、受信データを32バイト固定フレームに格納して保持
- Grove ポートを I2C スレーブ化 (アドレス 0x08、コマンドで要求データの種類を指定して応答)
- PlatformIO プロジェクト構成 (複数 env 拡張可能)
- Arduino フレームワーク (esp32-arduino)
- USB CDC 有効化設定済み (高速アップロード 1.5Mbps 設定)

## ハードウェア要件

- M5Stack NanoC6 (ESP32-C6) ボード
- Grove ポート接続先: M5Stack Basic（I2C マスター、Port A: G21=SDA, G22=SCL 想定）
- 接続先 BLE ペリフェラル (後述 UUID 実装、以下の2台)
  - [Chibi-T_Furoshiki_Heater](https://github.com/todateman/Chibi-T_Furoshiki_Heater)（M5DinMeter、エンジン温度）
  - [Chibi-T_Furoshiki_AutoAirAdjust](https://github.com/todateman/Chibi-T_Furoshiki_AutoAirAdjust)（M5Core2、1次側/2次側空気圧・燃圧）
- 電源: USB 5V または Grove 5V 給電
- 動作環境: 屋内 / 屋根のある屋外

## BLE UUID 一覧

Service UUID は両ペリフェラルで共通のため、接続先の判別は BLE アドバタイズ名（`getName()`）で行います。

| 用途 | UUID |
| ---- | ---- |
| Service（共通） | `7c44181A-c1a4-4635-a119-b490ed272552` |

### Chibi-T_Furoshiki_Heater (M5DinMeter)

- アドバタイズ名: `M5Din Furoshiki Heater`

| 用途 | UUID |
| ---- | ---- |
| Write Characteristic（存在確認用、実データなし） | `7c442A00-c1a4-4635-a119-b490ed272552` |
| Notify Characteristic | `7c442A6E-c1a4-4635-a119-b490ed272552` |

### Chibi-T_Furoshiki_AutoAirAdjust (M5Core2)

- アドバタイズ名: `ChibiT-AutoAirAdjust`
- 実装元: [feature/BLE ブランチ, commit 5b8b8aa](https://github.com/todateman/Chibi-T_Furoshiki_AutoAirAdjust)

| 用途 | UUID |
| ---- | ---- |
| Write Characteristic（存在確認用、実データなし） | `c9f878f1-c311-4452-ae5e-e813b4fe057d` |
| Notify Characteristic | `1d25ec49-e19c-4bb6-8c36-5dc8d8aaaebe` |

※ UUID は Version4 で生成。ペリフェラル側が同一 UUID を持つ必要があります（Service UUID のみ共通、Characteristic UUID はペリフェラルごとに異なります）。

## 動作概要

1. 起動時に BLE 初期化し、アクティブスキャンを開始  
   （Service UUID は共通のため、アドバタイズ名で Heater / AutoAirAdjust を判別する。両者ともデバイス名が Scan Response 側に含まれるため、名前判別にはアクティブスキャンが必須）
2. 対象デバイスを検出すると接続要求（見つかった方から順に、部分的な接続を許容。片方だけでも正常に動作する）
3. 接続後:
   - Write キャラクタリスティック存在確認 (現状未使用／将来拡張枠)
   - Notify キャラクタリスティック購読登録
4. Notify 受信:
   - 青色 LED 点灯 (次ループで消灯)
   - 改行までのデータを先頭タグ（`PRI:` / `SEC:` / `FUEL:` / タグなし）で判別し、対応する I2C 送信用フレーム（`priPreFrame` / `secPreFrame` / `fuelPreFrame` / `engineTempFrame`）に格納・保持し、M5Stack Basic からの要求を待機
   - Heater / AutoAirAdjust それぞれ専用の受信バッファを保持するため、2台からの Notify が混ざることはない
5. 未発見のペリフェラルが残っている限り、スキャンを継続する
6. 切断イベント発生時:
   - 切断されたペリフェラルのみ状態をリセットし、次のスキャンで自動的に再接続を試みる（もう一方の接続は維持される）

### シンプルなデータフロー（論理）

```text
Heater / AutoAirAdjust --(Notify: 文字列データ)--> M5NanoC6 (I2Cスレーブ, addr=0x08) <--(I2C要求/応答)-- M5Stack Basic (I2Cマスター)
```

## ソース構成

```text
src/main.cpp        BLEスキャン/接続/Notifyハンドラ + I2Cスレーブ応答
platformio.ini      PlatformIO 設定
custon_hwids.py     事前スクリプト (HWID カスタマイズ?) 要確認
sdkconfig.*         ESP-IDF ベースの設定 (一部反映) 要確認
include/, lib/, test/ README のみ (拡張用)
```

## I2C通信仕様

AutoAirAdjust 側の BLE 送信機能は [feature/BLE ブランチ, commit 5b8b8aa](https://github.com/todateman/Chibi-T_Furoshiki_AutoAirAdjust) で実装済みで、本 Central 側も対応済みです。

- スレーブアドレス: `0x08`
- ピン: SDA=GPIO2, SCL=GPIO1（Grove ポート）
- コマンド方式: マスターは読み出し前に1バイトのコマンドコードを書き込み、どのデータを要求するかを明示する
  - `CMD_ENGINE_TEMP` (`0x01`): <https://github.com/todateman/Chibi-T_Furoshiki_Heater> からエンジン温度データ (*.**°Ｃ) を要求
  - `CMD_PRI_PRE` (`0x02`): <https://github.com/todateman/Chibi-T_Furoshiki_AutoAirAdjust> から1次側空気圧センサデータ (*.**MPa) を要求
  - `CMD_SEC_PRE` (`0x03`): <https://github.com/todateman/Chibi-T_Furoshiki_AutoAirAdjust> から2次側空気圧センサデータ (*.**MPa) を要求
  - `CMD_FUEL_PRE` (`0x04`): <https://github.com/todateman/Chibi-T_Furoshiki_AutoAirAdjust> から燃圧センサデータ (*.**MPa) を要求
  - 未定義のコマンドを書き込んだ場合は長さ0（先頭バイトが `0x00`）の空フレームを返す
  - `CMD_PRI_PRE` / `CMD_SEC_PRE` / `CMD_FUEL_PRE` の3つは同一の BLE ペリフェラル（M5Core2、Chibi-T_Furoshiki_AutoAirAdjust）から届く。  
  ペリフェラル側は1本の Notify Characteristic で3種類のデータを送るため、M5NanoC6 は改行区切りメッセージの先頭タグでデータ種別を判別し、それぞれ専用フレームに格納する
    - `PRI:` → `CMD_PRI_PRE` 用フレーム（例: `PRI:0.85\n`）
    - `SEC:` → `CMD_SEC_PRE` 用フレーム（例: `SEC:0.72\n`）
    - `FUEL:` → `CMD_FUEL_PRE` 用フレーム（例: `FUEL:2.10\n`）
    - いずれのタグにも一致しないメッセージは従来どおり `CMD_ENGINE_TEMP` 用フレームに格納する（後方互換）
- フレーム形式: 32バイト固定
  - `[0]`: データ長 (0〜31)
  - `[1..31]`: データ本体（余りはゼロ埋め）
  - BLE Notify 未受信時（起動直後など）は全ゼロを返す
- 動作: マスターがコマンドを書き込んだ後 `Wire.requestFrom()` で読み出すと、直近に BLE で受信した最新データを返す（新しい Notify を受信するまで同じ値を返し続ける）

### M5Stack Basic（マスター）側のサンプルコード

```cpp
#include <M5Stack.h>
#include <Wire.h>

#define I2C_SLAVE_ADDR 0x08
#define I2C_FRAME_SIZE 32
#define CMD_ENGINE_TEMP 0x01
#define CMD_PRI_PRE 0x02
#define CMD_SEC_PRE 0x03
#define CMD_FUEL_PRE 0x04

void setup() {
  M5.begin();
  Wire.begin(21, 22); // Grove Port A (SDA, SCL)
}

// コマンドを指定してフレームを読み出し、シリアルに表示する
void requestAndPrint(uint8_t command, const char *label) {
  // 1. コマンドを書き込み、要求するデータの種類を伝える
  Wire.beginTransmission(I2C_SLAVE_ADDR);
  Wire.write(command);
  Wire.endTransmission();

  // 2. フレームを読み出す
  uint8_t frame[I2C_FRAME_SIZE];
  Wire.requestFrom(I2C_SLAVE_ADDR, I2C_FRAME_SIZE);
  for (int i = 0; i < I2C_FRAME_SIZE && Wire.available(); i++) {
    frame[i] = Wire.read();
  }

  uint8_t len = frame[0];
  if (len > 0) {
    String data;
    for (int i = 0; i < len; i++) {
      data += (char)frame[1 + i];
    }
    Serial.printf("%s: %s\n", label, data.c_str());
  }
}

void loop() {
  // エンジン温度、1次側/2次側空気圧、燃圧をそれぞれ要求して表示
  requestAndPrint(CMD_ENGINE_TEMP, "ENGINE_TEMP");
  requestAndPrint(CMD_PRI_PRE, "PRI_PRE");
  requestAndPrint(CMD_SEC_PRE, "SEC_PRE");
  requestAndPrint(CMD_FUEL_PRE, "FUEL_PRE");

  delay(500); // ポーリング間隔
}
```

※ Port A のピン番号は使用する M5Stack シリーズ機種によって異なる場合があるため、実機に応じて `Wire.begin(sda, scl)` の引数を調整してください。

## ビルド & アップロード手順 (PlatformIO)

VS Code + PlatformIO 拡張機能を使用します。

1. このリポジトリを VS Code で開く
2. 左側 PlatformIO パネルから環境 `M5NanoC6` を選択
3. "Build" をクリック (もしくは コマンドパレットから `PlatformIO: Build`)
4. USB 経由でボードを接続し "Upload" を実行
5. シリアルモニタ (115200bps) を開きログを確認

### CLI (任意)

```bash
pio run -e M5NanoC6
pio run -e M5NanoC6 -t upload
pio device monitor -b 115200
```

## 実行時ログ例 (概略)

```text
Advertised Device: <device info>
Device found! (M5Din Furoshiki Heater)
Advertised Device: <device info>
Device found! (ChibiT-AutoAirAdjust)
Connected to server (M5Din Furoshiki Heater)
 - Found our service
 - Found our characteristic
 - Registered for notify
Connected to server (ChibiT-AutoAirAdjust)
 - Found our service
 - Found our characteristic
 - Registered for notify
Notify callback for characteristic ... of data length N
```

## カスタマイズポイント

- スキャン時間: `#define SCAN_DURATION_SEC 5`
- I2Cスレーブアドレス: `#define I2C_SLAVE_ADDR 0x08` で変更可
- I2Cピン: `#define I2C_SDA_PIN 2`, `#define I2C_SCL_PIN 1` (Grove ポート)
- フレームサイズ: `#define I2C_FRAME_SIZE 32` で変更可（データ本体は `I2C_FRAME_SIZE - 1` バイトまで）
- コマンド: `CMD_ENGINE_TEMP` / `CMD_PRI_PRE` / `CMD_SEC_PRE` / `CMD_FUEL_PRE` を定義済み。さらに他のデータを中継する場合は新しいコマンド定数とフレーム/ハンドリング（および必要ならタグ文字列）を追加
- LED ピン: `#define BLUE_LED_PIN 7`
- UUID: `SERVICE_UUID`（共通）/ `HEATER_CHARACTERISTIC_UUID` / `HEATER_NOTIFY_CHARACTERISTIC_UUID` / `AUTOAIR_CHARACTERISTIC_UUID` / `AUTOAIR_NOTIFY_CHARACTERISTIC_UUID` で差し替え可能
- デバイス名フィルタ: `HEATER_DEVICE_NAME` / `AUTOAIR_DEVICE_NAME`（接続先の判別に使用、Service UUID が共通のため必須）
- 再接続ポリシー: 切断されたペリフェラルのみ `resetPeripheral()` で状態をリセットし、次の `scan()` で自動的に再接続を試みる（もう一方の接続には影響しない）

## 今後の改善案

- float16 → float32 変換/スケーリングユーティリティ
- アクティブスキャンによる消費電力増への対応（必要ならスキャン間隔/ウィンドウの調整）
- 3台目以降のペリフェラル追加が必要になった場合の汎用化（現状は Heater/AutoAirAdjust の2台固定の意図的な設計）
- 再接続の待機/バックオフ（現状は見つかり次第即座に再接続を試みるのみ）
- Notify データのタイムアウト検知（一定時間 Notify が来ない場合に「値が古い」ことを判別する仕組み）
- Write キャラクタリスティック活用 (将来の制御コマンド)
- データ検証 (CRC / バージョン / シーケンス番号)
- LED 点灯時間制御 (非同期タイマ) および点滅パターンで状態表示
- 状態遷移図とエラーハンドリング整備

## ライセンス

本ソフトウェアは MIT License です。`LICENSE` を参照してください。

---
ドキュメント最終更新: 2026-07-25 (Heater/AutoAirAdjust 同時接続対応)
