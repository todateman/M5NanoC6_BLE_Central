# M5NanoC6 BLE Central

[M5NanoC6](https://docs.m5stack.com/ja/core/M5NanoC6)（ESP32-C6) を使用した BLE Central (クライアント) 実装サンプルです。  
M5Stack シリーズ（ESP32）では Wi-Fi と BLE を同時利用できないという制約があるため、これを回避するために M5NanoC6 を外付け BLE 受信機 (ブリッジ) として用い、取得した BLE Notify データを Grove ポートの SoftwareSerial(UART) 経由で https://github.com/todateman/Chibi-T_Furoshiki_Logger へ転送します  
（115200bps, 8N1, rxPin=1, txPin=2）。

## 特徴

- Arduino フレームワーク (esp32-arduino)
- BLE Central: 指定サービス UUID をスキャン → 接続 → Notify 購読
- Notify 受信で LED(青) 点灯し、受信バイト列をそのまま SoftwareSerial(UART) 出力
- Grove ポートを UART ブリッジ化 (SoftwareSerial 115200bps / 8N1 / バッファ 256)
- PlatformIO プロジェクト構成 (複数 env 拡張可能)
- Arduino フレームワーク (esp32-arduino)
- USB CDC 有効化設定済み (高速アップロード 1.5Mbps 設定)

## ハードウェア要件

- M5Stack NanoC6 (ESP32-C6) ボード
- Grove ポート接続先: ESP32 搭載 M5Stack 系マイコン (UART 受信側。115200bps 想定)
- 接続先 BLE ペリフェラル (後述 UUID 実装)
- 電源: USB 5V または Grove 5V 給電
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
   - データ (最大 float16 長: 2 バイト / もしくは int 値) を SoftwareSerial (UART) で下流 ESP32 へ送信（改行付加）
5. 切断イベント発生時:
   - `onDisconnect` で state を IDLE に戻すのみ  
     (現状: 自動再スキャン未実装 / 改善予定)

### シンプルなデータフロー（論理）

```text
BLE Peripheral  --(Notify: 数値 float16/int)-->  M5NanoC6 (Central)  --(UART 115200 8N1)-->  ESP32 (既存M5Stack)
```

## ソース構成

```text
src/main.cpp        BLEスキャン/接続/Notifyハンドラ + SoftwareSerial ブリッジ
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
- UART/SoftwareSerial: `SoftSerial.begin(115200, SWSERIAL_8N1, rxPin, txPin, false, 256)` で速度/フォーマット/バッファ変更可
- ピン: `rxPin=1`, `txPin=2` (Grove ポート)
- LED ピン: `#define BLUE_LED_PIN 7`
- UUID: `#define SERVICE_UUID ...` 等で差し替え可能
- 再接続ポリシー: 現状は切断後 IDLE のみ。`onDisconnect` 内で `scan()` を再呼出すか状態追加予定
- 改行付加: コードは `SoftSerial.write(data); SoftSerial.println();` で後続パースを容易化

## 今後の改善案

- 接続失敗/切断後の自動再スキャンループ（指数バックオフ or 一定間隔）
- 再接続時の SoftwareSerial バッファクリア/同期
- float16 → float32 変換/スケーリングユーティリティ
- 省電力 (スキャン間隔調整 / Wi-Fi 無効化)
- Write キャラクタリスティック活用 (将来の制御コマンド)
- データ検証 (CRC / バージョン / シーケンス番号)
- LED 点灯時間制御 (非同期タイマ) および点滅パターンで状態表示
- 状態遷移図とエラーハンドリング整備

## ライセンス

本ソフトウェアは MIT License です。`LICENSE` を参照してください。

---
ドキュメント最終更新: 2025-11-08
