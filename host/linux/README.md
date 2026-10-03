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
make          # usblan_tap, usblan_ota
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

## ファーム更新 (usblan_ota)

```sh
sudo ./usblan_ota ../../firmware/build/usb_lan.bin
```

USB-LAN の線のまま (ボタン操作なしで) ファームを書き換える。ベンダー制御要求で送るので、
NCM ビルド (カーネルの cdc_ncm が使用中) でも、ベンダークラスビルドで `usblan_tap` が動いていても使える。
既定で 1209:0001 (ベンダークラス) → 303a:4000 (NCM) の順に探す。別の VID/PID は `-v`/`-p` で指定。

## Wi-Fi のモードごとのホスト設定

ファームの Wi-Fi は 2 モード (UART コンソールの `mode sta|ap` で切替、再起動で反映)。
どちらでも ESP32 自体は IP を持たず、フレームをそのまま中継するだけ。

### STA モード (既存の Wi-Fi ルーターに参加)

```sh
sudo dhclient usblan0          # ルーターの DHCP からアドレスをもらう
```
スマホは同じルーターにつなぎ、ホストの IP にアクセスする。

### AP モード (ESP32 がアクセスポイント、スマホが直接つながる)

ホストが AP の「中の人」になるので、ホストに固定 IP を振って DHCP サーバを動かす。

```sh
sudo ip addr add 192.168.4.1/24 dev usblan0
sudo dnsmasq --no-daemon --interface=usblan0 --bind-interfaces \
     --dhcp-range=192.168.4.10,192.168.4.100,12h
```
スマホで SSID (既定 `usb-lan`) に接続すると 192.168.4.x が割り当てられ、
`http://192.168.4.1/` でホスト上の設定画面 (送信機の周波数設定など) に届く。
インターネットには出られないので、Android は「インターネット未接続」と出るが接続は維持される。

NCM モードのファームでも同じ (インタフェース名が `usb0` や `enx...` になるだけ)。

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
