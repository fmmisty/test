# usblan_tap — ESP32-S3 USB-LAN 用 Linux ドライバ (ユーザー空間)

ファームを **ベンダークラス** でビルドしたとき (`firmware/sdkconfig.vendor`) に使うホスト側ドライバ。
libusb でデバイスを開き、TAP インタフェース `usblanN` として Linux のネットワークスタックに見せる。
カーネルモジュールは不要。

```
ESP32-S3 ── bulk IN ──> usblan_tap ──> TAP usblan0 ──> Linux (DHCP / ping ...)
         <─ bulk OUT ──            <──
```

プロトコル (フレームの区切り方・制御要求) は [`protocol/usblan_proto.h`](../../protocol/usblan_proto.h)。
ファームとこのドライバは同じヘッダ (同じパーサ) を使う。

## ビルド

```sh
sudo apt install libusb-1.0-0-dev build-essential pkg-config
make          # usblan_tap
make test     # フレーム分解のユニットテスト
```

## 使い方

```sh
sudo ./usblan_tap              # 既定 VID:PID = 1209:0001
sudo dhclient usblan0          # Wi-Fi 側ネットワークの DHCP からアドレス取得
ping -I usblan0 <ゲートウェイ>
```

- TAP の MAC はデバイスの Wi-Fi STA MAC に自動で合わせる (L2 ブリッジの前提)。
- Wi-Fi のリンク状態を 1 秒ごとに問い合わせ、TAP のキャリア (up/down) に反映する。
- root 以外で USB を開くには `sudo make install-udev` (plugdev グループ)。TAP 作成には CAP_NET_ADMIN が要る。

## ファーム側のビルド

```sh
cd firmware
rm -f sdkconfig
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.vendor" build flash
```

## 既知の制約

- esp_tinyusb 1.x のベンダー FIFO は 64 byte 固定なので、スループットは NCM より低い見込み (未計測)。
- 1209:0001 は pid.codes の **テスト用** PID。人に配るなら正式な VID/PID を取ること。
- 実機では未検証。
