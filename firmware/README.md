# firmware — ESP32-S3 USB ⇔ Wi-Fi L2 ブリッジ

ESP-IDF 5.5 + esp_tinyusb 1.x。`WIFI_MODE_APSTA`で、STAはIPを持たないUSB L2ブリッジ、SoftAPだけがESP-NETIF/DHCP/HTTPを持つ。管理画面は `http://192.168.4.1/`。

## FM送信機管理

- `board_pins.h` rev 0.3のSPI2配線を使用する。FPGAはMode 0/16 bit/1 MHz、ADF4002は24 bitでSCK/MOSIを共用する。
- FPGA通信中はADF_LEをLowに保持する。
- 周波数は76.0～108.0 MHz、100 kHz刻みだけ受理する。R=100、PFD=100 kHz、N=760～1080。
- 周波数変更はRF OFF・FPGA mute → ADF設定 → Lock確認 → N/Kv/KIDX/COMP設定の順。
- PLL Lock喪失は10 ms周期の独立安全タスクで検出し、FM_TX_ENをLowにする。
- 設定は明示的な保存操作でNVSへ保存する。RF ON状態は保存・復元しない。
- SoftAPはWPA2必須、Web APIはHTTP Basic認証必須。`menuconfig`でSSID/AP passwordとWeb user/passwordを必ず変更する。
- `CONFIG_FM_ALLOW_RF_OUTPUT`は既定OFF。実機でSPIとPLL Lockを確認するまでWebからRF ONは拒否される。

## ビルドの組合せ

| USB 側 | ビルド | ホスト側 |
|---|---|---|
| CDC-NCM (既定) | `idf.py build` | OS 標準ドライバ (Linux / macOS / Windows 11) |
| 独自ベンダークラス | `idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.vendor" build` | `host/linux/usblan_tap` |

切り替えるときは `sdkconfig` を消してからビルドする。

## 書き込み

- **初回**: BOOT を押しながら RESET → ROM の USB-Serial/JTAG が出るので `idf.py -p /dev/ttyACM0 flash`。
- **2 回目以降 (USB-LAN の線のまま、ボタン不要)**: `host/linux/usblan_ota build/usb_lan.bin`
  - ベンダー制御要求でイメージを送り、空いている OTA 面 (ota_0 / ota_1) に書いて再起動する。
  - NCM ビルドでもベンダークラスビルドでも使える (動作中の通信を止める必要もない)。
  - 新しいファームが起動処理を最後まで終えられなかった場合は、次の再起動で前のファームに戻る (ロールバック)。
  - パーティションは `partitions.csv` (3MB × 2 面)。

## Wi-Fi モード

| モード | 用途 | ホストに見せる MAC |
|---|---|---|
| STA | 既存のルーターに参加。ホストはルーターの DHCP で IP を取る | Wi-Fi STA の MAC |
| AP  | ESP32 がアクセスポイント。スマホが直接つながり、ホストが DHCP サーバを動かす | SoftAP の MAC |

既定は menuconfig の `USB-LAN adapter → Default Wi-Fi mode`。ホスト側の設定例は `host/linux/README.md`。
国コードは既定で `JP` (ch1-13)。

## UART コンソール (GPIO43/44 テストパッド, 115200bps)

| コマンド | 説明 |
|---|---|
| `status` | モード・MAC・接続状態 (AP モードでは接続中の端末一覧) |
| `mode sta\|ap` | モードを NVS に保存して再起動 |
| `wifi <ssid> [<password>]` | STA: 参加先を設定して再接続 (NVS 保存) |
| `scan` | STA: 周囲の AP 一覧 |
| `ap <ssid> <password> [channel]` | AP: SoftAP の設定 (パスワード 8〜63 文字、NVS 保存、即反映) |

STA で切断された場合は 1 秒から最大 30 秒まで間隔を倍にしながら再接続する。
SoftAP はパスワード未設定だと起動しない (オープン AP は作らない)。

## 注意

- 技適: 外部アンテナは WROOM-1U の認証に記載された型式のみ。
- 未検証: この環境では `idf.py build` と実機確認をしていない。
