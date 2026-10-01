# USB-LAN rev0.3 電源保護案

## 目的
- USB 5 V 給電
- ESP32-S3 Wi-Fi 送信時のピーク電流に余裕を持たせる
- 誤って 12 V を入力した場合に下流へ流さず遮断する
- 過電流/短絡は PTC + eFuse の二段で保護する

## 電源ツリー
```text
USB-C VBUS
  |
  +-- U4 TPS259573DSGR (eFuse / 内蔵MOSFET, 2.7-18 V)
  |      OVLO 約6.0 V
  |      ILIM 約1.0 A
  |
  +-- F1 MF-NSMF075/16X (PTC, Ihold 0.75 A, Itrip 1.5 A, Vmax 16 V)
  |
  +-- +5V_PROTECTED
         |
         +-- U3 TPS62162DSGR (Buck, fixed 3.3 V, 1 A)
         |      L1 3.3 uH
         |      CIN 10 uF X7R
         |      COUT 22 uF X7R
         |
         +-- J2 pin 2 (+5 V)
         +-- USBLC6-2SC6 VBUS reference pin
```

## U4 TPS259573
- IN: USB-C VBUS
- OUT: F1 へ
- EN/OVLO: 抵抗分圧で約6.0 V遮断
  - Rtop 402 kOhm / Rbottom 100 kOhm を初期値
  - 実機で5.25 V正常動作と12 V遮断を確認する
- ILM: 約1.0 Aに設定（データシート式からRILMを選定）
- dVdt: 10 nFを初期値としてソフトスタート
- FLT: テストパッドまたはDNPでよい
- GND/EPAD: GNDへ

## U3 TPS62162
- VIN: +5V_PROTECTED
- EN: +5V_PROTECTED
- SW -> L1 3.3 uH -> +3V3
- VOS: +3V3
- FB: 固定3.3 V版なのでAGNDへ
- PG: 未使用ならNC
- PGND / AGND / EPAD: GND
- CIN: 10 uF X7R をVIN-PGND直近
- COUT: 22 uF X7R を+3V3-PGND直近
- ESP32直近の0.1 uFは維持

## 12 V誤入力時の注意
USBLC6-2SC6 の VBUS ピンを生VBUSへ直接つながない。
必ず +5V_PROTECTED 側へ接続し、12 V誤入力時にUSB保護ICやESP32側へ過電圧が回らないようにする。

## 確認項目
- 正常5 V入力で起動
- 5.25 V入力で誤遮断しない
- 6 V付近でOVLO動作
- 12 V誤入力で下流+5V_PROTECTED / +3V3が上昇しない
- Wi-Fi連続送信 + USB-LAN通信時の5 V入力電流
- BuckのSWノード配置をESP32アンテナ / USB D+/D-から離す
- ERC / 実機確認は未実施
