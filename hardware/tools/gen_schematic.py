#!/usr/bin/env python3
"""USB-LAN 基板の初版回路図 (KiCad 7/8/9 で開ける .kicad_sch) と BOM を生成する。

接続はすべてピン端点に置いたネットラベルで表現しているため、配線 (wire) は無い。
KiCad 上で手修正を始めたら .kicad_sch 側を正とし、このスクリプトは再実行しないこと
(再実行すると手修正が上書きされる)。

    python3 hardware/tools/gen_schematic.py
"""
import csv
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
KICAD_DIR = ROOT / "kicad"
PROJECT = "usb_lan"
LIB = "usb_lan"  # 回路図に埋め込むローカルシンボルのライブラリ名

_ns = uuid.UUID("6f1c0b5e-6d2a-4c61-9d7e-8b1f1f3c2a10")


def uid(*key):
    """再生成しても差分が出ないよう、キーから決定的に UUID を作る。"""
    return str(uuid.uuid5(_ns, "/".join(map(str, key))))


ROOT_UUID = uid("root")

# ---------------------------------------------------------------------------
# シンボル定義 (lib 座標系: Y 上向き, 単位 mm)
# pins: (number, name, type, x, y, angle)  angle=0 は左側ピン (本体へ右向き)
# ---------------------------------------------------------------------------
PIN_LEN = 2.54


def ic_symbol(name, left, right, top=(), bottom=(), width=15.24, ref="U"):
    """left/right/top/bottom: [(number, name, type), ...] (None は空き行)"""
    rows = max(len(left), len(right))
    h = (rows + 1) * 2.54
    h = max(h, 7.62)
    half_w = width / 2
    top_y = round(h / 2 / 2.54) * 2.54
    bot_y = top_y - h
    pins = []
    for i, p in enumerate(left):
        if p:
            pins.append((*p, -half_w - PIN_LEN, top_y - 2.54 * (i + 1), 0))
    for i, p in enumerate(right):
        if p:
            pins.append((*p, half_w + PIN_LEN, top_y - 2.54 * (i + 1), 180))
    n = len(top)
    for i, p in enumerate(top):
        x = (i - (n - 1) / 2) * 2.54
        pins.append((*p, x, top_y + PIN_LEN, 270))
    n = len(bottom)
    for i, p in enumerate(bottom):
        x = (i - (n - 1) / 2) * 2.54
        pins.append((*p, x, bot_y - PIN_LEN, 90))
    return dict(name=name, ref=ref, body=("rect", -half_w, top_y, half_w, bot_y),
                pins=pins, show_names=True, show_numbers=True)


def two_pin(name, ref, kind):
    return dict(name=name, ref=ref, body=(kind,),
                pins=[("1", "~", "passive", 0, 3.81, 270),
                      ("2", "~", "passive", 0, -3.81, 90)],
                show_names=False, show_numbers=False)


def one_pin(name, ref):
    return dict(name=name, ref=ref, body=("tp",),
                pins=[("1", "1", "passive", 0, -2.54, 90)],
                show_names=False, show_numbers=False)


SYMBOLS = {
    "ESP32-S3-WROOM-1U": ic_symbol(
        "ESP32-S3-WROOM-1U", width=20.32,
        left=[("2", "3V3", "power_in"), ("3", "EN", "input"), None,
              ("27", "IO0", "bidirectional"), ("15", "IO3", "bidirectional"),
              ("26", "IO45", "bidirectional"), ("16", "IO46", "bidirectional"), None,
              ("13", "IO19/USB_D-", "bidirectional"), ("14", "IO20/USB_D+", "bidirectional"), None,
              ("37", "TXD0/IO43", "bidirectional"), ("36", "RXD0/IO44", "bidirectional"), None,
              ("39", "IO1", "bidirectional"), ("38", "IO2", "bidirectional"),
              ("4", "IO4", "bidirectional"), ("5", "IO5", "bidirectional"),
              ("6", "IO6", "bidirectional"), ("7", "IO7", "bidirectional"),
              ("12", "IO8", "bidirectional")],
        right=[("17", "IO9", "bidirectional"), ("18", "IO10", "bidirectional"),
               ("19", "IO11", "bidirectional"), ("20", "IO12", "bidirectional"),
               ("21", "IO13", "bidirectional"), ("22", "IO14", "bidirectional"),
               ("8", "IO15", "bidirectional"), ("9", "IO16", "bidirectional"),
               ("10", "IO17", "bidirectional"), ("11", "IO18", "bidirectional"),
               ("23", "IO21", "bidirectional"), ("28", "IO35", "bidirectional"),
               ("29", "IO36", "bidirectional"), ("30", "IO37", "bidirectional"),
               ("31", "IO38", "bidirectional"), ("32", "IO39", "bidirectional"),
               ("33", "IO40", "bidirectional"), ("34", "IO41", "bidirectional"),
               ("35", "IO42", "bidirectional"), ("24", "IO47", "bidirectional"),
               ("25", "IO48", "bidirectional")],
        bottom=[("1", "GND", "passive"), ("40", "GND", "passive"), ("41", "GND(EPAD)", "passive")]),
    "USB_C_USB2.0_16P": ic_symbol(
        "USB_C_USB2.0_16P", ref="J", width=12.7,
        left=[],
        right=[("A4", "VBUS", "passive"), ("A9", "VBUS", "passive"),
               ("B4", "VBUS", "passive"), ("B9", "VBUS", "passive"), None,
               ("A5", "CC1", "bidirectional"), ("B5", "CC2", "bidirectional"), None,
               ("A6", "D+", "bidirectional"), ("B6", "D+", "bidirectional"),
               ("A7", "D-", "bidirectional"), ("B7", "D-", "bidirectional"), None,
               ("A8", "SBU1", "bidirectional"), ("B8", "SBU2", "bidirectional")],
        bottom=[("A1", "GND", "passive"), ("A12", "GND", "passive"),
                ("B1", "GND", "passive"), ("B12", "GND", "passive"),
                ("S1", "SHIELD", "passive")]),
    "USBLC6-2SC6": ic_symbol(
        "USBLC6-2SC6", width=10.16,
        left=[("1", "I/O1", "passive"), ("3", "I/O2", "passive")],
        right=[("6", "I/O1", "passive"), ("4", "I/O2", "passive")],
        top=[("5", "VBUS", "passive")], bottom=[("2", "GND", "passive")]),
    "LTC4365CTS8": ic_symbol(
        "LTC4365CTS8", width=12.7,
        left=[("1", "VIN", "power_in"), ("2", "UV", "input"), ("3", "OV", "input"), ("4", "GND", "power_in")],
        right=[("8", "GATE", "output"), ("7", "VOUT", "input"), ("6", "FAULT", "open_collector"), ("5", "SHDN", "input")]),
    "TPS62162DSG": ic_symbol(
        "TPS62162DSG", width=12.7,
        left=[("2", "VIN", "power_in"), ("3", "EN", "input"), ("5", "FB", "input"), ("4", "AGND", "power_in")],
        right=[("7", "SW", "power_out"), ("6", "VOS", "input"), ("8", "PG", "open_collector"), ("1", "PGND", "power_in")],
        bottom=[("9", "EP", "power_in")]),
    "NMOS": ic_symbol(
        "NMOS", ref="Q", width=10.16,
        left=[("1", "G", "input"), ("2", "S", "passive")],
        right=[("3", "D", "passive")]),
    "BarrelJack_2Pin": ic_symbol(
        "BarrelJack_2Pin", ref="J", width=10.16,
        left=[("1", "+", "passive"), ("2", "-", "passive")]),
    "R": two_pin("R", "R", "res"),
    "C": two_pin("C", "C", "cap"),
    "L": two_pin("L", "L", "fuse"),
    "Polyfuse": two_pin("Polyfuse", "F", "fuse"),
    "SW_Push": two_pin("SW_Push", "SW", "sw"),
    "TestPoint": one_pin("TestPoint", "TP"),
    "LED": two_pin("LED", "D", "led"),
    # 送信機 (FM) 接続用の拡張ヘッダ。ピン番号は 2x8 ヘッダの実ピン番号 (奇数=左列, 偶数=右列)
    "Conn_02x08": ic_symbol(
        "Conn_02x08", ref="J", width=12.7,
        left=[("1", "+3V3", "passive"), ("3", "GND", "passive"), ("5", "SDA", "passive"),
              ("7", "MISO", "passive"), ("9", "RST", "passive"), ("11", "MUX", "passive"),
              ("13", "SCK", "passive"), ("15", "ADF_LE", "passive")],
        right=[("2", "+5V", "passive"), ("4", "GND", "passive"), ("6", "SCL", "passive"),
               ("8", "CS", "passive"), ("10", "EN", "passive"), ("12", "ADC", "passive"),
               ("14", "MOSI", "passive"), ("16", "GND", "passive")]),
    # 切替式ループフィルタ / MPX 制御用のヘッダ (rev 0.4)。奇数=左列, 偶数=右列
    "Conn_02x04": ic_symbol(
        "Conn_02x04", ref="J", width=12.7,
        left=[("1", "LF_IN", "passive"), ("3", "LF_PRE", "passive"), ("5", "MPX_MUTE", "passive"),
              ("7", "+3V3", "passive")],
        right=[("2", "LF_FB", "passive"), ("4", "MPX_RUN", "passive"), ("6", "ADF_CE", "passive"),
               ("8", "GND", "passive")]),
    "PWR_FLAG": dict(name="PWR_FLAG", ref="#FLG", body=("flag",),
                     pins=[("1", "pwr", "power_out", 0, 0, 90)],
                     show_names=False, show_numbers=False, power=True),
}

# ---------------------------------------------------------------------------
# 部品 (ref, symbol, value, footprint, 配置 x, y, BOM 情報)
# ---------------------------------------------------------------------------
FP = {
    "0603R": "Resistor_SMD:R_0603_1608Metric",
    "0603C": "Capacitor_SMD:C_0603_1608Metric",
    "0805C": "Capacitor_SMD:C_0805_2012Metric",
}

PARTS = [
    # ref, sym, value, footprint, x, y, mfr, mpn, note
    ("J1", "USB_C_USB2.0_16P", "USB-C (USB2.0)", "Connector_USB:USB_C_Receptacle_GCT_USB4105-xx-A_16P_TopMnt_Horizontal",
     40.64, 71.12, "GCT", "USB4105-GF-A", "USB2.0 専用 16P レセプタクル"),
    ("R1", "R", "5.1k", FP["0603R"], 78.74, 43.18, "Yageo", "RC0603FR-075K1L", "CC1 プルダウン (UFP/Sink)"),
    ("R2", "R", "5.1k", FP["0603R"], 88.9, 43.18, "Yageo", "RC0603FR-075K1L", "CC2 プルダウン (UFP/Sink)"),
    ("R6", "R", "1M", FP["0603R"], 30.48, 111.76, "Yageo", "RC0603FR-071ML", "シールド-GND ブリーダ (ケース方針次第で DNP/0Ω)"),
    ("C5", "C", "4.7nF 100V", FP["0603C"], 43.18, 111.76, "Murata", "GRM188R72A472KA01D", "シールド-GND 高周波バイパス"),
    ("U2", "USBLC6-2SC6", "USBLC6-2SC6", "Package_TO_SOT_SMD:SOT-23-6",
     114.3, 71.12, "STMicroelectronics", "USBLC6-2SC6", "USB D+/D- ESD 保護 (コネクタ直近)"),
    ("R4", "R", "0", FP["0603R"], 142.24, 63.5, "Yageo", "RC0603JR-070RL", "D+ 直列 (予約。波形/EMI を見て 0〜22Ω)"),
    ("R5", "R", "0", FP["0603R"], 152.4, 63.5, "Yageo", "RC0603JR-070RL", "D- 直列 (予約。波形/EMI を見て 0〜22Ω)"),
    ("C1", "C", "4.7uF 25V", FP["0805C"], 81.28, 152.4, "Murata", "GRM21BR61E475KA12L", "USB VBUS local bulk (USB-C is data/service only; not tied to main 5V)"),
    ("J4", "BarrelJack_2Pin", "5V DC IN center-positive", "", 45.72, 152.4, "-", "-", "Main supply input, 5V/1A adapter"),
    ("U3", "LTC4365CTS8", "LTC4365CTS8", "Package_TO_SOT_SMD:TSOT-23-8",
     104.14, 152.4, "Analog Devices", "LTC4365CTS8#TRMPBF", "UV/OV/reverse supply protection controller"),
    ("Q1", "NMOS", "AO3400A", "Package_TO_SOT_SMD:SOT-23", 137.16, 144.78, "Alpha & Omega Semiconductor", "AO3400A", "Back-to-back protection MOSFET, input side"),
    ("Q2", "NMOS", "AO3400A", "Package_TO_SOT_SMD:SOT-23", 157.48, 144.78, "Alpha & Omega Semiconductor", "AO3400A", "Back-to-back protection MOSFET, output side"),
    ("F1", "Polyfuse", "0.75A hold", "Fuse:Fuse_1206_3216Metric",
     182.88, 152.4, "Littelfuse", "1206L075SLYR", "PTC after MOSFET protection; 0.75A hold class"),
    ("U4", "TPS62162DSG", "TPS62162 3.3V", "Package_DFN_QFN:WSON-8-1EP_2x2mm_P0.5mm",
     223.52, 152.4, "Texas Instruments", "TPS62162DSGR", "3.3V fixed, 1A synchronous buck"),
    ("L1", "L", "2.2uH Isat>=1.5A DCR<=0.15R", "Inductor_SMD:L_1210_3225Metric",
     251.46, 152.4, "-", "-", "TPS62162 output inductor"),
    ("R16", "R", "1.65M 1%", FP["0603R"], 71.12, 175.26, "Yageo", "-", "LTC4365 UV/OV ladder: DC_IN to UV"),
    ("R17", "R", "54.9k 1%", FP["0603R"], 81.28, 175.26, "Yageo", "-", "LTC4365 UV/OV ladder: UV to OV"),
    ("R18", "R", "162k 1%", FP["0603R"], 91.44, 175.26, "Yageo", "-", "LTC4365 UV/OV ladder: OV to GND"),
    ("R19", "R", "5.1k", FP["0603R"], 119.38, 175.26, "Yageo", "-", "Rev.E gate slew network series resistor"),
    ("C7", "C", "4.7nF C0G 50V", FP["0603C"], 129.54, 175.26, "Murata", "-", "Rev.E gate slew network capacitor"),
    ("R20", "R", "10R", FP["0603R"], 137.16, 165.1, "Yageo", "-", "GATE_BUS to Q1 gate"),
    ("R21", "R", "10R", FP["0603R"], 157.48, 165.1, "Yageo", "-", "GATE_BUS to Q2 gate"),
    ("R22", "R", "100k", FP["0603R"], 104.14, 175.26, "Yageo", "-", "LTC4365 SHDN pull-up to DC_IN"),
    ("R23", "R", "100k", FP["0603R"], 203.2, 175.26, "Yageo", "-", "LTC4365 FAULT pull-up to +3V3"),
    ("R24", "R", "100k", FP["0603R"], 233.68, 175.26, "Yageo", "-", "TPS62162 PG pull-up to +3V3"),
    ("C8", "C", "10uF 10V", FP["0805C"], 213.36, 175.26, "Murata", "-", "TPS62162 input bulk"),
    ("C9", "C", "0.1uF 25V", FP["0603C"], 223.52, 175.26, "Murata", "-", "TPS62162 input HF bypass"),
    ("C10", "C", "22uF 10V", FP["0805C"], 251.46, 175.26, "Murata", "-", "TPS62162 output bulk"),
    ("C11", "C", "0.1uF 10V", FP["0603C"], 261.62, 175.26, "Murata", "-", "TPS62162 output HF bypass"),
    ("C12", "C", "100uF low-ESR", "Capacitor_SMD:CP_Elec_6.3x5.8", 271.78, 175.26, "-", "-", "ESP32 Wi-Fi transient bulk"),
    ("C13", "C", "10uF X7R", FP["0805C"], 281.94, 175.26, "Murata", "-", "ESP32 local bulk"),
    ("C14", "C", "0.1uF X7R", FP["0603C"], 292.1, 175.26, "Murata", "-", "ESP32 local HF bypass"),
    ("TP7", "TestPoint", "LTC_FAULT", "TestPoint:TestPoint_Pad_D1.5mm", 203.2, 185.42, "-", "-", "LTC4365 fault monitor"),
    ("U1", "ESP32-S3-WROOM-1U", "ESP32-S3-WROOM-1U-N8R2", "RF_Module:ESP32-S3-WROOM-1U",
     254.0, 106.68, "Espressif", "ESP32-S3-WROOM-1U-N8R2", "外部アンテナ版 (モジュール上に U.FL 実装済み)"),
    ("R3", "R", "10k", FP["0603R"], 190.5, 170.18, "Yageo", "RC0603FR-0710KL", "EN プルアップ (RC 遅延)"),
    ("C6", "C", "1uF", FP["0603C"], 203.2, 170.18, "Murata", "GRM188R61E105KA12D", "EN RC 遅延 (電源立上り対策)"),
    ("SW1", "SW_Push", "RESET", "Button_Switch_SMD:SW_SPST_PTS810",
     215.9, 170.18, "C&K", "PTS810 SJM 250 SMTR LFS", "EN (リセット)"),
    ("SW2", "SW_Push", "BOOT", "Button_Switch_SMD:SW_SPST_PTS810",
     228.6, 170.18, "C&K", "PTS810 SJM 250 SMTR LFS", "GPIO0 (ダウンロードモード)"),
    ("TP1", "TestPoint", "TXD0", "TestPoint:TestPoint_Pad_D1.5mm", 322.58, 50.8, "-", "-", "UART0 TX (GPIO43)"),
    ("TP2", "TestPoint", "RXD0", "TestPoint:TestPoint_Pad_D1.5mm", 332.74, 50.8, "-", "-", "UART0 RX (GPIO44)"),
    ("TP3", "TestPoint", "GND", "TestPoint:TestPoint_Pad_D1.5mm", 342.9, 50.8, "-", "-", "UART 用 GND"),
    ("TP4", "TestPoint", "3V3", "TestPoint:TestPoint_Pad_D1.5mm", 353.06, 50.8, "-", "-", "3V3 モニタ"),
    ("TP5", "TestPoint", "EN", "TestPoint:TestPoint_Pad_D1.5mm", 363.22, 50.8, "-", "-", "自動書込み用 EN"),
    ("TP6", "TestPoint", "IO0", "TestPoint:TestPoint_Pad_D1.5mm", 373.38, 50.8, "-", "-", "自動書込み用 GPIO0"),
    ("J2", "Conn_02x08", "FM I/F", "Connector_PinHeader_2.54mm:PinHeader_2x08_P2.54mm_Vertical",
     345.44, 116.84, "-", "2x8 2.54mm ピンヘッダ", "送信機接続用 拡張ヘッダ (FPGA: 4 線 SPI・RST / ADF4002: SPI 共用 + LE・MUXOUT / I2C / ADC)"),
    ("R7", "R", "4.7k", FP["0603R"], 312.42, 96.52, "Yageo", "RC0603FR-074K7L", "I2C SDA プルアップ (送信機側にあれば DNP)"),
    ("R8", "R", "4.7k", FP["0603R"], 322.58, 96.52, "Yageo", "RC0603FR-074K7L", "I2C SCL プルアップ (送信機側にあれば DNP)"),
    ("R9", "R", "1k", FP["0603R"], 312.42, 165.1, "Yageo", "RC0603FR-071KL", "状態 LED 電流制限 (~1.2mA)"),
    ("J3", "Conn_02x04", "FM LOOP", "Connector_PinHeader_2.54mm:PinHeader_2x04_P2.54mm_Vertical",
     345.44, 220.98, "-", "2x4 2.54mm ピンヘッダ", "送信機のループフィルタ切替 (TMUX1136 SEL) / MPX ミュート / ADF4002 CE"),
    ("R10", "R", "10k", FP["0603R"], 302.26, 248.92, "Yageo", "RC0603FR-0710KL", "起動中の既定値 (LF_INPUT_SEL / LF_FB_SEL / MPX_MUTE は High、MPX_RUN / ADF_CE / FM_TX_EN は Low)"),
    ("R11", "R", "10k", FP["0603R"], 312.42, 248.92, "Yageo", "RC0603FR-0710KL", "起動中の既定値 (LF_INPUT_SEL / LF_FB_SEL / MPX_MUTE は High、MPX_RUN / ADF_CE / FM_TX_EN は Low)"),
    ("R12", "R", "10k", FP["0603R"], 322.58, 248.92, "Yageo", "RC0603FR-0710KL", "起動中の既定値 (LF_INPUT_SEL / LF_FB_SEL / MPX_MUTE は High、MPX_RUN / ADF_CE / FM_TX_EN は Low)"),
    ("R13", "R", "10k", FP["0603R"], 332.74, 248.92, "Yageo", "RC0603FR-0710KL", "起動中の既定値 (LF_INPUT_SEL / LF_FB_SEL / MPX_MUTE は High、MPX_RUN / ADF_CE / FM_TX_EN は Low)"),
    ("R14", "R", "10k", FP["0603R"], 342.9, 248.92, "Yageo", "RC0603FR-0710KL", "起動中の既定値 (LF_INPUT_SEL / LF_FB_SEL / MPX_MUTE は High、MPX_RUN / ADF_CE / FM_TX_EN は Low)"),
    ("R15", "R", "10k", FP["0603R"], 353.06, 248.92, "Yageo", "RC0603FR-0710KL", "起動中の既定値 (LF_INPUT_SEL / LF_FB_SEL / MPX_MUTE は High、MPX_RUN / ADF_CE / FM_TX_EN は Low)"),
    ("D1", "LED", "LED 緑", "LED_SMD:LED_0603_1608Metric", 312.42, 180.34, "Würth Elektronik", "150060GS75000",
     "状態表示 (GPIO21, High で点灯)"),
    ("#FLG01", "PWR_FLAG", "PWR_FLAG", "", 30.48, 152.4, None, None, None),
    ("#FLG02", "PWR_FLAG", "PWR_FLAG", "", 30.48, 172.72, None, None, None),
    ("#FLG03", "PWR_FLAG", "PWR_FLAG", "", 30.48, 132.08, None, None, None),
]

# 基板外 (ケーブル・アンテナ・ケース) の BOM 行
OFFBOARD = [
    ("CBL1", "IPEX MHF I (U.FL) - SMA(R-SMA) バルクヘッド ピグテール 1.13mm 〜100mm", "-", "-",
     "モジュール上の U.FL → ケースパネル。損失 ~0.5dB/10cm 程度。コネクタ種別 (SMA/R-SMA) はアンテナに合わせる"),
    ("ANT1", "2.4GHz 外部アンテナ (技適の工事設計認証に記載された型式のみ)", "-", "要確認",
     "指定外アンテナは技適違反。WROOM-1U の認証情報でアンテナ型式・利得を確認して同一品を購入"),
    ("ENC1", "金属ケース", "-", "-", "USB-C / SMA 用の穴加工。シールドとケースの接続方針を決める"),
]

# ネット: net -> [(ref, pin), ...]
NETS = {
    "USB_VBUS": [("J1", "A4"), ("J1", "A9"), ("J1", "B4"), ("J1", "B9"), ("U2", "5"), ("C1", "1"), ("#FLG01", "1")],
    "DC_IN_5V": [("J4", "1"), ("U3", "1"), ("R16", "1"), ("R22", "1"), ("Q1", "3")],
    "UV_SET": [("U3", "2"), ("R16", "2"), ("R17", "1")],
    "OV_SET": [("U3", "3"), ("R17", "2"), ("R18", "1")],
    "FET_SOURCE_COMMON": [("Q1", "2"), ("Q2", "2")],
    "PROTECTED_5V": [("Q2", "3"), ("U3", "7"), ("F1", "1")],
    "+5V": [("F1", "2"), ("U4", "2"), ("U4", "3"), ("C8", "1"), ("C9", "1"), ("J2", "2")],
    "GATE_BUS": [("U3", "8"), ("R19", "1"), ("R20", "1"), ("R21", "1")],
    "GATE_RC": [("R19", "2"), ("C7", "1")],
    "Q1_GATE": [("R20", "2"), ("Q1", "1")],
    "Q2_GATE": [("R21", "2"), ("Q2", "1")],
    "LTC_SHDN": [("U3", "5"), ("R22", "2")],
    "LTC_FAULT": [("U3", "6"), ("R23", "1"), ("TP7", "1")],
    "SW_3V3": [("U4", "7"), ("L1", "1")],
    "+3V3": [("L1", "2"), ("U4", "6"), ("U4", "8"), ("R23", "2"), ("R24", "1"),
             ("C10", "1"), ("C11", "1"), ("C12", "1"), ("C13", "1"), ("C14", "1"),
             ("U1", "2"), ("R3", "1"), ("TP4", "1"),
             ("J2", "1"), ("R7", "1"), ("R8", "1"), ("J3", "7"), ("R10", "2"), ("R11", "2"), ("R13", "2")],
    "TPS_PG": [("U4", "8"), ("R24", "2")],
    "GND": [("J1", "A1"), ("J1", "A12"), ("J1", "B1"), ("J1", "B12"), ("J4", "2"),
            ("R1", "2"), ("R2", "2"), ("R6", "2"), ("C5", "2"), ("U2", "2"), ("C1", "2"),
            ("U3", "4"), ("R18", "2"), ("C7", "2"),
            ("U4", "1"), ("U4", "4"), ("U4", "5"), ("U4", "9"),
            ("C8", "2"), ("C9", "2"), ("C10", "2"), ("C11", "2"), ("C12", "2"), ("C13", "2"), ("C14", "2"),
            ("U1", "1"), ("U1", "40"), ("U1", "41"), ("C6", "2"), ("SW1", "2"), ("SW2", "2"),
            ("TP3", "1"), ("#FLG02", "1"), ("J2", "3"), ("J2", "4"), ("J2", "16"), ("D1", "1"),
            ("J3", "8"), ("R12", "2"), ("R14", "2"), ("R15", "2")],
    "SHIELD": [("J1", "S1"), ("R6", "1"), ("C5", "1"), ("#FLG03", "1")],
    "CC1": [("J1", "A5"), ("R1", "1")],
    "CC2": [("J1", "B5"), ("R2", "1")],
    "USB_C_DP": [("J1", "A6"), ("J1", "B6"), ("U2", "1"), ("U2", "6"), ("R4", "1")],
    "USB_C_DN": [("J1", "A7"), ("J1", "B7"), ("U2", "3"), ("U2", "4"), ("R5", "1")],
    "USB_DP": [("R4", "2"), ("U1", "14")],
    "USB_DN": [("R5", "2"), ("U1", "13")],
    "EN": [("U1", "3"), ("R3", "2"), ("C6", "1"), ("SW1", "1"), ("TP5", "1")],
    "BOOT_IO0": [("U1", "27"), ("SW2", "1"), ("TP6", "1")],
    "UART_TX": [("U1", "37"), ("TP1", "1")],
    "UART_RX": [("U1", "36"), ("TP2", "1")],
    # ---- 送信機 (FM) 接続用 拡張ヘッダ J2 ----
    "I2C_SDA": [("U1", "12"), ("R7", "2"), ("J2", "5")],      # GPIO8
    "I2C_SCL": [("U1", "17"), ("R8", "2"), ("J2", "6")],      # GPIO9
    "SPI_MISO": [("U1", "21"), ("J2", "7")],                  # GPIO13 (SPI2 MISO, IO_MUX)
    "FPGA_CS": [("U1", "18"), ("J2", "8")],                   # GPIO10 (SPI2 CS0, IO_MUX)
    "FPGA_RST": [("U1", "4"), ("J2", "9")],                   # GPIO4
    "FM_TX_EN": [("U1", "5"), ("J2", "10"), ("R15", "1")],    # GPIO5 (RF 出力 ON/OFF)。起動中は R15 で Low
    "ADF_MUXOUT": [("U1", "6"), ("J2", "11")],                # GPIO6 (ADF4002 ロック検出, 入力)
    "FM_ADC": [("U1", "39"), ("J2", "12")],                   # GPIO1 (ADC1_CH0)
    "SPI_SCK": [("U1", "20"), ("J2", "13")],                  # GPIO12 (SPI2 SCLK, IO_MUX) FPGA と ADF4002 CLK で共用
    "SPI_MOSI": [("U1", "19"), ("J2", "14")],                 # GPIO11 (SPI2 MOSI, IO_MUX) FPGA と ADF4002 DATA で共用
    "ADF_LE": [("U1", "22"), ("J2", "15")],                   # GPIO14 (ADF4002 LE, 立上りでラッチ)
    # ---- 切替式ループフィルタ / MPX 制御 J3 (rev 0.4) ----
    "LF_INPUT_SEL": [("U1", "8"), ("J3", "1"), ("R10", "1")],  # GPIO15: 1 = ACQUIRE
    "LF_FB_SEL": [("U1", "9"), ("J3", "2"), ("R11", "1")],     # GPIO16: 1 = ACQUIRE
    "LF_PRECHARGE": [("U1", "10"), ("J3", "3")],               # GPIO17: 1 = プリチャージ
    "MPX_RUN": [("U1", "11"), ("J3", "4"), ("R12", "1")],      # GPIO18: 1 = MPX を VT 加算器へ
    "MPX_MUTE": [("U1", "7"), ("J3", "5"), ("R13", "1")],      # GPIO7: 1 = FPGA ミュート
    "ADF_CE": [("U1", "38"), ("J3", "6"), ("R14", "1")],       # GPIO2: 0 = ADF4002 パワーダウン
    # ---- 状態 LED ----
    "LED_DRV": [("U1", "23"), ("R9", "1")],                   # GPIO21
    "LED_A": [("R9", "2"), ("D1", "2")],
}
# 意図的に未接続とするピン (no_connect マーカーを置く)
NC_EXPLICIT = [("J1", "A8"), ("J1", "B8")]

# ---------------------------------------------------------------------------
# テキスト注記
# ---------------------------------------------------------------------------
NOTES = [
    (25.4, 22.86, "USB-C 入力 / ESD 保護\n"
                  "・CC1/CC2 は各 5.1k で GND (UFP/Sink)。USB-Cは通信/保守専用で本体電源には使わない\n"
                  "・USBLC6-2SC6 はコネクタ直近に配置。D+/D- は 90Ω 差動、スタブ無しで A/B 面を合流\n"
                  "・R4/R5 は直列抵抗の予約パッド (初期 0Ω)"),
    (25.4, 124.46, "電源 Rev.E: 5V/1A DC IN → LTC4365 → AO3400A×2 back-to-back → PTC → TPS62162 3.3V\n"
                   "・UV/OV: 1.65M / 54.9k / 162k の3抵抗ラダーで約4.30V / 5.76V\n"
                   "・GATE_BUS: 5.1k + 4.7nF(C0G)直列 to GND、各MOSFET gateへ10Ω\n"
                   "・USB-C VBUSは通信/ESD参照のみ。本体5V電源とは接続しない"),
    (25.4, 106.68, "シールド: 金属ケースへの接続方針で R6/C5 を調整\n(ケース=FG なら 1M//4.7nF、直結なら R6=0Ω)"),
    (180.34, 22.86, "ESP32-S3-WROOM-1U\n"
                    "・GPIO19/20 は内蔵 USB-OTG PHY に直結 (TinyUSB CDC-NCM)\n"
                    "・ストラップ: IO0 (BOOT SW), IO3/IO45/IO46 は未接続 (内部プル任せ)\n"
                    "・N8R2 は Quad PSRAM のため IO35-37 も使用可 (Octal 版 R8 では使用不可)\n"
                    "・アンテナはモジュール上の U.FL → ピグテールでケース外 SMA へ"),
    (180.34, 190.5, "EN: 10k/1uF の RC 遅延 + RESET スイッチ\nIO0: BOOT スイッチ (内部プルアップ)\n"
                    "書込み: BOOT 押しながら RESET → ROM の USB-Serial/JTAG で idf.py flash"),
    (317.5, 38.1, "UART0 テストパッド (ログ/予備書込み)"),
    (297.18, 66.04, "J2: 送信機 (FM) 接続用 拡張ヘッダ (3.3V ロジック)\n"
                    "・I2C: SDA=GPIO8 / SCL=GPIO9 (4.7k プルアップ)\n"
                    "・SPI2 (IO_MUX): SCK=GPIO12 / MOSI=GPIO11 / MISO=GPIO13\n"
                    "・FPGA: CS=GPIO10, RST=GPIO4 (High でリセット) / ADF4002: SCK・MOSI 共用, LE=GPIO14, MUXOUT=GPIO6\n"
                    "・TX_EN=GPIO5 (RF 出力 ON/OFF), ADC=GPIO1 (RF レベル)\n"
                    "・+5V はLTC4365保護 + PTC後の5V DCアダプタ系\n"
                    "・USB-C VBUSとは分離。5Vロジックの送信機はレベル変換が必要"),
    (297.18, 152.4, "状態 LED: GPIO21 (High で点灯)"),
    (297.18, 195.58, "J3 (rev 0.4): 切替式ループフィルタ / MPX 制御 (3.3V ロジック, TMUX1136 の SEL へ)\n"
                     "・LF_INPUT_SEL=GPIO15 / LF_FB_SEL=GPIO16 (1 = ACQUIRE), LF_PRECHARGE=GPIO17\n"
                     "・MPX_RUN=GPIO18, MPX_MUTE=GPIO7 (FPGA へ), ADF_CE=GPIO2\n"
                     "・R10-R15: 起動中 (ピンがハイインピーダンスの間) は ミュート・RF OFF・ACQUIRE・PLL 停止"),
]

# ---------------------------------------------------------------------------
# 出力
# ---------------------------------------------------------------------------
F = "(effects (font (size 1.27 1.27)))"
FH = "(effects (font (size 1.27 1.27)) hide)"


def fmt(v):
    s = f"{v:.4f}".rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


def lib_symbol_sexpr(key, s):
    q = f"{LIB}:{key}"
    out = [f'    (symbol "{q}"']
    if s.get("power"):
        out.append("      (power)")
    if not s["show_numbers"]:
        out.append("      (pin_numbers hide)")
    out.append("      (pin_names (offset 0.254)" + ("" if s["show_names"] else " hide") + ")")
    out.append("      (in_bom " + ("no" if s.get("power") else "yes") + ") (on_board " + ("no" if s.get("power") else "yes") + ")")
    out.append(f'      (property "Reference" "{s["ref"]}" (at 0 0 0) {FH if s.get("power") else F})')
    out.append(f'      (property "Value" "{key}" (at 0 0 0) {F})')
    out.append(f'      (property "Footprint" "" (at 0 0 0) {FH})')
    out.append(f'      (property "Datasheet" "~" (at 0 0 0) {FH})')
    body = s["body"]
    g = []
    stroke = "(stroke (width 0.254) (type default))"
    if body[0] == "rect":
        _, x1, y1, x2, y2 = body
        g.append(f"(rectangle (start {fmt(x1)} {fmt(y1)}) (end {fmt(x2)} {fmt(y2)}) {stroke} (fill (type background)))")
    elif body[0] in ("res", "fuse"):
        g.append(f"(rectangle (start -1.016 2.54) (end 1.016 -2.54) {stroke} (fill (type none)))")
        if body[0] == "fuse":
            g.append(f"(polyline (pts (xy -1.524 -1.778) (xy 1.524 1.778)) {stroke} (fill (type none)))")
    elif body[0] == "cap":
        g.append(f"(polyline (pts (xy -2.032 0.762) (xy 2.032 0.762)) (stroke (width 0.508) (type default)) (fill (type none)))")
        g.append(f"(polyline (pts (xy -2.032 -0.762) (xy 2.032 -0.762)) (stroke (width 0.508) (type default)) (fill (type none)))")
        g.append(f"(polyline (pts (xy 0 2.54) (xy 0 0.762)) {stroke} (fill (type none)))")
        g.append(f"(polyline (pts (xy 0 -0.762) (xy 0 -2.54)) {stroke} (fill (type none)))")
    elif body[0] == "sw":
        g.append(f"(circle (center 0 2.032) (radius 0.508) {stroke} (fill (type none)))")
        g.append(f"(circle (center 0 -2.032) (radius 0.508) {stroke} (fill (type none)))")
        g.append(f"(polyline (pts (xy 1.016 2.54) (xy 1.016 -2.54)) {stroke} (fill (type none)))")
        g.append(f"(polyline (pts (xy 1.016 0) (xy 2.54 0)) {stroke} (fill (type none)))")
    elif body[0] == "led":
        g.append(f"(polyline (pts (xy -1.27 -1.016) (xy 1.27 -1.016) (xy 0 1.016) (xy -1.27 -1.016)) {stroke} (fill (type none)))")
        g.append(f"(polyline (pts (xy -1.27 1.016) (xy 1.27 1.016)) {stroke} (fill (type none)))")
        g.append(f"(polyline (pts (xy 0 2.54) (xy 0 1.016)) {stroke} (fill (type none)))")
        g.append(f"(polyline (pts (xy 0 -1.016) (xy 0 -2.54)) {stroke} (fill (type none)))")
    elif body[0] == "tp":
        g.append(f"(circle (center 0 0.762) (radius 0.762) {stroke} (fill (type none)))")
        g.append(f"(polyline (pts (xy 0 0) (xy 0 -0.254)) {stroke} (fill (type none)))")
    elif body[0] == "flag":
        g.append(f"(polyline (pts (xy 0 0) (xy 0 1.27) (xy -1.016 1.905) (xy 0 2.54) (xy 1.016 1.905) (xy 0 1.27)) "
                 f"(stroke (width 0) (type default)) (fill (type none)))")
    out.append(f'      (symbol "{key}_0_1"')
    out += ["        " + x for x in g]
    out.append("      )")
    out.append(f'      (symbol "{key}_1_1"')
    for num, name, ptype, x, y, ang in s["pins"]:
        length = 0 if s.get("power") else (PIN_LEN if s["body"][0] in ("rect",) else (1.27 if s["body"][0] != "tp" else 1.778))
        out.append(f'        (pin {ptype} line (at {fmt(x)} {fmt(y)} {ang}) (length {fmt(length)})'
                   f' (name "{name}" {F}) (number "{num}" {F}))')
    out.append("      )")
    out.append("    )")
    return "\n".join(out)


def pin_pos(ref):
    """ref の各ピンの回路図座標 (Y 下向き) と向きを返す。"""
    p = PART_BY_REF[ref]
    s = SYMBOLS[p[1]]
    x0, y0 = p[4], p[5]
    return {num: (round(x0 + x, 4), round(y0 - y, 4), ang) for num, _, _, x, y, ang in s["pins"]}


PART_BY_REF = {p[0]: p for p in PARTS}


def symbol_instance(p):
    ref, key, value, fp, x, y, mfr, mpn, note = p
    s = SYMBOLS[key]
    power = s.get("power")
    xs = [px for *_, px, _, _ in s["pins"]]
    right = max(xs) if s["body"][0] == "rect" else 2.54
    top = max(py for *_, py, _ in s["pins"])
    bot = min(py for *_, py, _ in s["pins"])
    out = [f'  (symbol (lib_id "{LIB}:{key}") (at {fmt(x)} {fmt(y)} 0) (unit 1)',
           f'    (in_bom {"no" if power else "yes"}) (on_board {"no" if power else "yes"}) (dnp no)',
           f'    (uuid "{uid("sym", ref)}")']
    if s["body"][0] == "rect":
        rpos = (x, y - top - 1.27)
        vpos = (x, y - bot + 1.27)
        rj = vj = ""
    else:
        rpos = (x + 2.54, y - 1.27)
        vpos = (x + 2.54, y + 1.27)
        rj = vj = " (justify left)"
    out.append(f'    (property "Reference" "{ref}" (at {fmt(rpos[0])} {fmt(rpos[1])} 0)'
               + (f" {FH}" if power else f" (effects (font (size 1.27 1.27)){rj})") + ")")
    out.append(f'    (property "Value" "{value}" (at {fmt(vpos[0])} {fmt(vpos[1])} 0)'
               + f" (effects (font (size 1.27 1.27)){vj}))")
    out.append(f'    (property "Footprint" "{fp}" (at {fmt(x)} {fmt(y)} 0) {FH})')
    out.append(f'    (property "Datasheet" "~" (at {fmt(x)} {fmt(y)} 0) {FH})')
    if mpn:
        out.append(f'    (property "Manufacturer" "{mfr}" (at {fmt(x)} {fmt(y)} 0) {FH})')
        out.append(f'    (property "MPN" "{mpn}" (at {fmt(x)} {fmt(y)} 0) {FH})')
    for num, *_ in s["pins"]:
        out.append(f'    (pin "{num}" (uuid "{uid("pin", ref, num)}"))')
    out.append(f'    (instances (project "{PROJECT}" (path "/{ROOT_UUID}" (reference "{ref}") (unit 1))))')
    out.append("  )")
    return "\n".join(out)


LABEL_ANGLE = {0: (180, "right"), 180: (0, "left"), 270: (90, "left"), 90: (270, "left")}


def label(net, x, y, pin_angle):
    ang, just = LABEL_ANGLE[pin_angle]
    return (f'  (label "{net}" (at {fmt(x)} {fmt(y)} {ang}) (fields_autoplaced)'
            f' (effects (font (size 1.27 1.27)) (justify {just} bottom)) (uuid "{uid("lbl", net, x, y)}"))')


def check():
    used = set()
    for net, nodes in NETS.items():
        assert len(nodes) >= 2, f"net {net} has a single node"
        for ref, pin in nodes:
            assert ref in PART_BY_REF, ref
            assert pin in pin_pos(ref), (ref, pin)
            assert (ref, pin) not in used, f"{ref}.{pin} in two nets"
            used.add((ref, pin))
    for n in NC_EXPLICIT:
        assert n not in used
    return used


def build_schematic():
    used = check()
    parts = [
        "(kicad_sch (version 20230121) (generator eeschema)",
        f'  (uuid "{ROOT_UUID}")',
        '  (paper "A3")',
        '  (title_block (title "USB-LAN adapter (ESP32-S3-WROOM-1U / USB CDC-NCM)")'
        ' (date "2026-10-01") (rev "0.3")'
        ' (comment 1 "Initial draft - generated by hardware/tools/gen_schematic.py"))',
        "  (lib_symbols",
    ]
    parts += [lib_symbol_sexpr(k, s) for k, s in SYMBOLS.items()]
    parts.append("  )")
    for p in PARTS:
        parts.append(symbol_instance(p))
    for net, nodes in NETS.items():
        for ref, pin in nodes:
            x, y, ang = pin_pos(ref)[pin]
            parts.append(label(net, x, y, ang))
    # 未使用ピンに no_connect
    for p in PARTS:
        for num, (x, y, _) in pin_pos(p[0]).items():
            if (p[0], num) not in used:
                parts.append(f'  (no_connect (at {fmt(x)} {fmt(y)}) (uuid "{uid("nc", p[0], num)}"))')
    for i, (x, y, text) in enumerate(NOTES):
        t = text.replace('"', "'").replace("\n", "\\n")
        parts.append(f'  (text "{t}" (at {fmt(x)} {fmt(y)} 0)'
                     f' (effects (font (size 1.524 1.524)) (justify left top)) (uuid "{uid("note", i)}"))')
    parts.append('  (sheet_instances (path "/" (page "1")))')
    parts.append(")")
    return "\n".join(parts) + "\n"


def build_project():
    return """{
  "meta": { "filename": "usb_lan.kicad_pro", "version": 1 },
  "sheets": [ [ "%s", "" ] ]
}
""" % ROOT_UUID


def build_bom():
    groups = {}
    for ref, key, value, fp, *_rest in PARTS:
        if ref.startswith("#"):
            continue
        mfr, mpn, note = _rest[2], _rest[3], _rest[4]
        k = (value, fp, mfr, mpn)
        groups.setdefault(k, {"refs": [], "note": note})["refs"].append(ref)
    rows = []
    for (value, fp, mfr, mpn), g in groups.items():
        refs = sorted(g["refs"], key=lambda r: (r.rstrip("0123456789"), int(r[len(r.rstrip("0123456789")):] or 0)))
        rows.append([", ".join(refs), len(refs), value, fp.split(":")[-1], mfr, mpn, g["note"]])
    rows.sort(key=lambda r: r[0])
    for ref, desc, mfr, mpn, note in OFFBOARD:
        rows.append([ref, 1, desc, "(基板外)", mfr, mpn, note])
    return rows


def main():
    KICAD_DIR.mkdir(parents=True, exist_ok=True)
    (KICAD_DIR / f"{PROJECT}.kicad_sch").write_text(build_schematic(), encoding="utf-8")
    pro = KICAD_DIR / f"{PROJECT}.kicad_pro"
    if not pro.exists():
        pro.write_text(build_project(), encoding="utf-8")
    with open(ROOT / "bom.csv", "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["Reference", "Qty", "Value", "Footprint", "Manufacturer", "MPN", "Note"])
        w.writerows(build_bom())
    print("wrote", KICAD_DIR / f"{PROJECT}.kicad_sch", "and", ROOT / "bom.csv")


if __name__ == "__main__":
    main()
