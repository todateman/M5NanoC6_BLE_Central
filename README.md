# M5NanoC6 BLE Central

[M5NanoC6](https://docs.m5stack.com/ja/core/M5NanoC6)（ESP32-C6) を使用した BLE Central (クライアント) 実装サンプルです。  
M5Stack シリーズ（ESP32）では Wi-Fi と BLE を同時利用できないという制約があるため、これを回避するために M5NanoC6 を外付け BLE 受信機 (ブリッジ) として用い、取得した BLE Notify データを I2C 経由で ESP32 側マイコンへ転送します (I2C アドレス 0x67)。

## 特徴

- Arduino フレームワーク (esp32-arduino) を使用
- BLE Central 動作: 指定サービス UUID を持つアドバタイズをスキャン → 接続 → Notify購読
- Notify受信時に LED (青) を点灯し受信データをそのまま I2C 送信
- I2C アドレス `0x67` を固定利用
- PlatformIO プロジェクト構成 (複数 env 対応可能)
- USB CDC 有効化設定済み (高速アップロード 1.5Mbps 設定)

## ハードウェア要件

- M5Stack NanoC6 (ESP32-C6) ボード
- I2C アドレス: `0x67`  
  （接続先デバイスは M5Stack シリーズ（ESP32 搭載マイコン）を想定
- 接続先 BLE ペリフェラル (後述 UUID を持つサービス/キャラクタリスティック実装)
- 電源: USB 5V または Grove コネクタ 5V 給電
- 動作環境: 屋内 / 屋根のある屋外

## BLE UUID 一覧
| 用途 | UUID |
|------|------|
| Service | `7c44181A-c1a4-4635-a119-b490ed272552` |
| Write Characteristic | `7c442A00-c1a4-4635-a119-b490ed272552` |
| Notify Characteristic | `7c442A6E-c1a4-4635-a119-b490ed272552` |

※ UUID は Version4 で生成。ペリフェラル側が同一 UUID を持つ必要があります。

## 動作概要

1. 起動時に BLE 初期化し 5 秒間パッシブスキャン
2. 指定サービス UUID を検出すると接続要求
3. 接続後:
   - Write キャラクタリスティック存在確認 (現状未使用／将来拡張枠)
   - Notify キャラクタリスティック購読登録
4. Notify 受信:
   - 青色 LED 点灯 (次ループで消灯)
   - データ (最大 float16 長: 2 バイト / もしくは int 値) を I2C 経由で下流 ESP32 へ送信
5. 切断イベント発生時:
   - `onDisconnect` で state を IDLE に戻すのみ  
     (現状: 自動再スキャン未実装 / 改善予定)

### シンプルなデータフロー（論理）

```text
BLE Peripheral  --(Notify: 数値 float16/int)-->  M5NanoC6 (Central)  --(I2C 0x67)-->  ESP32 (既存M5Stack)
```

## ソース構成

```text
src/main.cpp        メインロジック (BLEスキャン, 接続, Notifyハンドラ, I2C送信)
platformio.ini      PlatformIO 設定
custon_hwids.py     事前スクリプト (HWID カスタマイズ?) 要確認
sdkconfig.*         ESP-IDF ベースの設定 (一部反映) 要確認
include/, lib/, test/ README のみ (拡張用)
```

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
Device found!
Connected to server
 - Found our service
 - Found our characteristic
 - Registered for notify
Notify callback for characteristic ... of data length N
```

## カスタマイズポイント

- スキャン時間: `scan()` 内 `pBLEScan->start(5, false);`
- I2C ピン: `Wire.begin(2, 1);` (SDA=2, SCL=1) M5NanoC6 の Grove コネクタ仕様
- LED ピン: `#define BLUE_LED_PIN 7`
- サービス / キャラクタリスティック UUID: `#define SERVICE_UUID ...` 等を書き換え可能
- 再接続ポリシー: 現在は切断後に自動で再スキャンせず IDLE に戻るのみ。`onDisconnect` で `scan()` を呼ぶ、もしくは状態遷移追加予定。

## 今後の改善案

- 接続失敗/切断後の自動再スキャンループ（指数バックオフ or 一定間隔）
- I2C 送信結果 (ACK) の検査とリトライ
- 受信数値フォーマット: float16 → float32 変換/スケーリングユーティリティ
- 省電力 (スキャン間隔調整 / Wi-Fi 無効化)
- Write キャラクタリスティック活用 (将来の制御コマンド)
- データ検証 (CRC / 符号化バージョン / シーケンス番号)
- LED 点灯時間をタイマで制御しチラツキ最適化
- 状態遷移図とエラーハンドリング整備

## ライセンス
本ソフトウェアは MIT License です。`LICENSE` を参照してください。

---
ドキュメント最終更新: 2025-11-08
