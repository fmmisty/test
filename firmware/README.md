# firmware — ESP32-S3 USB ⇔ Wi-Fi L2 ブリッジ

ESP-IDF 5.5 + esp_tinyusb 1.x。`WIFI_MODE_APSTA`で、STAはIPを持たないUSB L2ブリッジ、SoftAPだけがESP-NETIF/DHCP/HTTPを持つ。管理画面は `http://192.168.4.1/`。

## FM送信機管理

- `board_pins.h` rev 0.4の配線を使用する。J2 (SPI2・ADF4002・TX_EN) に加え、J3で切替式ループフィルタ (FMTX Rev.B5以降のU306/U307 TMUX1136)・MPXミュート・ADF4002 CEを制御する。
- FPGAはMode 0/16 bit/1 MHz、ADF4002は24 bitでSCK/MOSIを共用する。FPGA通信中はADF_LEをLowに保持する。
- FPGA_RSTはHighでリセット。通常はLowに保つ（rev 0.3のファームはHighのままにしていた）。
- 周波数は76.0～108.0 MHz、100 kHz刻みだけ受理する。R=100、PFD=100 kHz、N=760～1080。
- ADF4002: 電流設定1 = 625 uA（TRANSMIT、G1=0）、電流設定2 = 2.5 mA（ACQUIRE、G1=1）、RSET 5.1 kΩ、MUXOUT = デジタルロック検出。
- 位相検出極性は**負**（`ADF_PD_POSITIVE false`）。CP → 反転型アクティブPI → 反転2段 → VTUNEで正味1回反転するため。**実機で要確認**：逆だとVTUNEが電源側に張り付き、ロックしない。
- 周波数変更の手順（`fm_control_set`、約0.9秒）:
  1. RF OFF、MPXミュート（FPGAの`MPX_MUTE`とアナログの`MPX_RUN`）。FPGAのMUTEDを待つ。
  2. FPGAへN/Kv/KIDX/COMP_ENを書き、ステータス語で取り込みを確認する（MPXコアはミュート中しか取り込まない）。
  3. TRANSMIT→ACQUIRE: CPハイインピーダンス、`LF_FB_SEL`=1、10 us、`LF_INPUT_SEL`=1、`LF_PRECHARGE`=1。
  4. R、N（2.5 mA）、CP ON。ロック検出が50 ms続くまで待つ（最大0.5秒）。
  5. C302のプリチャージを250 ms待つ（R306 1 kΩ × C302 22 uFの11倍）。
  6. ACQUIRE→TRANSMIT: CPハイインピーダンス、`LF_INPUT_SEL`=0、2 us、`LF_FB_SEL`=0、20 us、`LF_PRECHARGE`=0、N（625 uA）、CP ON。
  7. 0.5秒待ち、最後の100 msロックしていることを確認する。
- 同調が成功してもRFとMPXは止めたまま。出すのはRF ON操作（`fm_control_set_rf(true)`）だけで、PLLがTRANSMITでロック中かつFPGAが設定を取り込み済みのときに限る。
- 起動時は保存済みチャンネルへ自動で同調する（RFは出さない）。
- PLL Lock喪失は10 ms周期の独立安全タスクで検出し、FM_TX_ENをLow、MPXをミュートにしてFAULTにする。再同調するまでRF ONは拒否する。
- FPGAの有無と状態は、ステータス語（[15] MUTED、[14] COMP有効、[13:7] 有効KIDX、[6] MMCMロック、[4:0]=00010）で判定する。
- 設定は明示的な保存操作でNVSへ保存する。RF ON状態は保存・復元しない。
- SoftAPはWPA2必須、Web APIはHTTP Basic認証必須。`menuconfig`でSSID/AP passwordとWeb user/passwordを必ず変更する。
- `CONFIG_FM_ALLOW_RF_OUTPUT`は既定OFF。実機でSPIとPLL Lockを確認するまでWebからRF ONは拒否される。

### PC上のテスト（実機・ESP-IDF不要、gccのみ）

```
sh firmware/tests/host/run_host_tests.sh
```

- `test_adf4002`: ADF4002のラッチ語とKIDXを、データシートRev. Cから手計算した値と照合（21項目）。
- `test_fm_sequence`: `fm_control.c`そのものを、ADF4002（SCK/MOSI共用、LEラッチ、ロック検出）とFPGAレジスタのモデルに対して実行（28項目、RF許可ビルドは32項目）。書込み語の順序、スイッチの順序と待ち時間、FPGAの取り込み、RFロックアウト、ロックしない場合、FPGAなし、送信中のロック外れ、NVS保存・復元を確認する。

2026-10-02に全項目合格（WSL Ubuntu 24.04、gcc 13）。これはロジックの確認であり、実機の確認ではない。

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
| `mode sta` | STA L2ブリッジを維持（旧APブリッジモードは廃止） |
| `wifi <ssid> [<password>]` | STA: 参加先を設定して再接続 (NVS 保存) |
| `scan` | STA: 周囲の AP 一覧 |
| `ap <ssid> <password> [channel]` | AP: SoftAP の設定 (パスワード 8〜63 文字、NVS 保存、即反映) |

STA で切断された場合は 1 秒から最大 30 秒まで間隔を倍にしながら再接続する。
SoftAP はパスワード未設定だと起動しない (オープン AP は作らない)。

## 注意

- 技適: 外部アンテナは WROOM-1U の認証に記載された型式のみ。
- ESP-IDF v5.5 / ESP32-S3で`idf.py build`合格（2026-10-02、rev 0.4）。`usb_lan.bin`は981,808 bytes、3 MBアプリ領域の69%が空き。
- 実機でのSPI波形、PLL Lock、位相検出極性、ACQUIRE→TRANSMIT切替時のVTUNE段差、RF出力は未確認。確認までは`CONFIG_FM_ALLOW_RF_OUTPUT=n`を維持する。
- 起動時に期待されるADF4002の書込み語（92.0 MHz）: `0x0C0113`、`0x0C0112`、`0x100190`、`0x239801`、続いて同調手順（`0x0C0112`、`0x100190`、`0x239801`、`0x0C0012`、…、`0x0C0112`、`0x039801`、`0x0C0012`）。
- rev 0.4のJ3と抵抗R10〜R15は回路図（`hardware/kicad`）に追加済み。基板は未作成。
