================================================================
 RS-FEC 送受信(TX/RX)一式  — Cora Z7 2台版
 まとめ日: 2026-10-15
================================================================

■ これは何か
 RS(255,239)+インターリーブD=32 誤り訂正コアを載せた Cora Z7 を
 2台使い、LAN(独立HUB)で UDP 送受信する一式。
 - TX機: UDPクライアント(送信)  IP 192.168.1.20
 - RX機: UDPサーバ(受信)        IP 192.168.1.10 : port 5001
 両機とも wait_for_lan 付き(LAN未接続なら待機、挿すと開始)。

■ 完成・実機実証ずみ（2026-09-11）
 - 2台を独立HUB(社内LANと物理分離)で接続→電源ON
 - TX(192.168.1.20) → RX(192.168.1.10:5001) で UDP 送信
 - 実測 約956 Mbps(ほぼ1Gbps線速)、UART(115200)で確認

■ 同梱ファイル
 BOOT_tx.bin … TX機のSDへ入れる起動イメージ(UDP client + wait_for_lan)
 BOOT_rx.bin … RX機のSDへ入れる起動イメージ(UDP server + wait_for_lan)
 rtl/        … RS-FEC の Verilog コア一式
   rs_fec_tx_core.v / rs_fec_rx_core.v … 符号化/復号コア(最適化版)
   rs_decoder_simple.v / rs_encoder.v / bm_module.v / gf_mul.v / rs_sdp_ram.v … 下位
   fec_loopback_axis.v … PS↔PL AXIブリッジ(Stage3:FEC統合用, 下記※)
   rs_fec_selftest_top.v / cora_selftest_top.v … 1台自己テスト版
 tx_app_src/ … TX機アプリのソース(main.c に IP/MAC/wait_for_lan)
 rx_app_src/ … RX機アプリのソース(同上)

■ 使い方(2台 UDP 送受信)
 1) BOOT_tx.bin を microSD にコピー(ファイル名は BOOT.bin にリネーム)→ TX機へ
 2) BOOT_rx.bin を別microSDにコピー(BOOT.bin にリネーム)→ RX機へ
 3) 両機の MODE ジャンパを SD 起動に設定、独立HUBに2台を接続
 4) 電源ON(5V/3Aを黒バレルジャック、JP3を給電元に合わせる)
 5) LANを挿すと wait_for_lan 解除→通信開始
 6) 確認は UART(microUSB経由、115200 8N1)。TX機/RX機でCOMが別
    ※本PC環境では COM3/COM5 等。複数JTAGが見えるので要注意。

■ ネットワーク設定(main.c)
 TX: IP 192.168.1.20 / 宛先 192.168.1.10:5001 / MAC 末尾 …01:03
 RX: IP 192.168.1.10 / 待受 port 5001         / MAC 末尾 …01:02
 ※2台のMACは必ず別にすること(重複するとスイッチが誤学習し不達)
 ※PCは独立HUB網に非接続(別subnet)→PCからのpingは不可、確認はUART

■ ※Stage3(FECをUDPに統合)について — 未完
 「データを符号化してUDP送信 / 受信して復号・訂正」の統合(PS↔PL AXI)は
 作業中。1台AXIループバック(fec_loopback_axis.v + fec_app)で
 「長さは往復OK(7648B)だが中身が化ける」バグを切り分け中。
 FECコア自体は sim/自己テストで誤り訂正を実証ずみ。バグはAXI周りの
 データ取り回し(dwidth変換のバイト順/sof生成/tlast整列)が容疑。

■ 達成済みのマイルストーン
 ① 1台自己テスト(実機で200byteバースト誤り訂正, 緑LED)
 ② SD常設化(SD挿入+電源ONで自走)
 ③ Ethernet疎通(自作lwIP, ping応答)
 ④ wait_for_lan(LAN未接続→待機→挿すと開始)
 ⑤ 2台 UDP 送受信 約956Mbps  ← 本一式
 ⑥ FEC×UDP 統合(Stage3) … 作業中

(RS-FECコア本体の最適化版・取説は別途: Desktop\rs_fec_optimized_20260902.zip)
================================================================
