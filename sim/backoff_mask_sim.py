#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
バックオフ vs スペクトルマスク 予測（実条件・I/Q 不要の机上検討）
=====================================================================
目的: MN-band D-STL 実条件で、GaN PA を何 dB 出力バックオフ(OBO)すれば
      実マスク（−37 dBc@±250kHz / −48 dBc@±750kHz）に入るかを予測。

実条件:
  - 変調: 単一キャリア 64QAM, RRC ロールオフ α=0.2
  - シンボルレート: 374.4 ksps（占有 ≒405 kHz）
  - マスク: fo±250 kHz で −37 dBc 以下 / fo±750 kHz で −48 dBc 以下
  - 出力目標: 平均 2 W (+33 dBm)。デバイス Psat 例: 25W(+44dBm)→OBO 11dB /
             10W(+40dBm)→OBO 7dB

PA モデル: 代表的な Saleh（AM/AM 圧縮＋AM/PM）。※実機 AM/AM が入れば差し替え。
  → 絶対値は目安。傾向・必要 OBO のオーダーを示す。実機はスペアナで要確認。

依存: numpy（図は matplotlib 任意）
実行: python3 sim/backoff_mask_sim.py
"""
import numpy as np

rng = np.random.default_rng(2020)

RS = 374.4e3          # シンボルレート [sps]
ALPHA = 0.2           # ロールオフ
OSR = 16              # サンプル/シンボル
FS = RS * OSR         # サンプリング周波数
MASK = [(250e3, -37.0), (750e3, -48.0)]   # (オフセット[Hz], 上限[dBc])


def rrc(beta, sps, span=16):
    N = span * sps
    t = np.arange(-N/2, N/2+1)/sps
    h = np.zeros_like(t)
    for i, ti in enumerate(t):
        if abs(ti) < 1e-8:
            h[i] = 1-beta+4*beta/np.pi
        elif beta > 0 and abs(abs(ti)-1/(4*beta)) < 1e-6:
            h[i] = beta/np.sqrt(2)*((1+2/np.pi)*np.sin(np.pi/(4*beta)) +
                                    (1-2/np.pi)*np.cos(np.pi/(4*beta)))
        else:
            h[i] = (np.sin(np.pi*ti*(1-beta)) +
                    4*beta*ti*np.cos(np.pi*ti*(1+beta))) / \
                   (np.pi*ti*(1-(4*beta*ti)**2))
    return h/np.sqrt(np.sum(h**2))


def gen_64qam_rrc(n_sym=12000):
    m = 8
    lv = np.arange(-(m-1), m, 2)
    pts = (lv[None, :]+1j*lv[:, None]).ravel()
    pts /= np.sqrt((np.abs(pts)**2).mean())
    syms = pts[rng.integers(0, pts.size, n_sym)]
    up = np.zeros(n_sym*OSR, dtype=complex); up[::OSR] = syms
    x = np.convolve(up, rrc(ALPHA, OSR), mode='same')
    x /= np.sqrt((np.abs(x)**2).mean())    # rms=1
    return x


def saleh(v):
    aa, ba, ap, bp = 2.0, 1.0, 1.4, 1.0    # Psat(正規化)=1 @ r=1
    r = np.abs(v)
    A = aa*r/(1+ba*r**2)
    P = ap*r**2/(1+bp*r**2)
    return A*np.exp(1j*(np.angle(v)+P))


PSAT = 2.0*1.0/(1+1.0)   # Saleh A_max at r=1 = 1.0 → Psat=1.0（電力）
# （正規化 Psat=1。OBO = -10log10(平均出力電力)）


def psd_dbc(y, nfft=1<<15):
    """出力 PSD を計算し、ピークを 0 dBc 基準に正規化して返す。"""
    N = (len(y)//nfft)*nfft
    y = y[:N].reshape(-1, nfft)
    w = np.hanning(nfft)
    S = np.mean(np.abs(np.fft.fftshift(np.fft.fft(y*w, axis=1), axes=1))**2, axis=0)
    f = np.fft.fftshift(np.fft.fftfreq(nfft, 1/FS))
    p = 10*np.log10(S/ S.max() + 1e-20)     # ピーク=0 dBc
    return f, p


def shoulder(f, p, off, half=20e3):
    """±off [Hz] 付近(±half)の PSD 平均[dBc]（上下の悪い方）。"""
    def band(c):
        m = (f > c-half) & (f < c+half)
        return 10*np.log10(np.mean(10**(p[m]/10)))
    return max(band(+off), band(-off))


def run_pa(x, drive):
    """drive=入力rmsスケール。出力 y、平均出力電力、OBO[dB] を返す。"""
    y = saleh(drive*x)
    pout = (np.abs(y)**2).mean()
    obo = -10*np.log10(pout/PSAT)
    return y, obo


# ---- 簡易 MP-DPD（ILA 1 パス, order7/mem3, タップ間隔1）----
def mp_basis(x, order=7, mem=3):
    ax = np.abs(x); cols = []
    for mm in range(mem+1):
        xm = np.roll(x, mm); xm[:mm] = 0
        axm = np.roll(ax, mm); axm[:mm] = 0
        for k in range(1, order+1, 2):
            cols.append(xm*axm**(k-1))
    return np.column_stack(cols)


def dpd_once(drive, x):
    """間接学習(ILA) 1 パス。小信号利得 g0=aa=2.0 を基準に後段逆モデルを同定し
    前段(予歪器)へコピー。u=Φ(v)w を PA へ入力。v=drive*x（PA 入力レベル）。"""
    g0 = 2.0                       # Saleh 小信号利得 (=aa)
    v = drive*x                    # PA 入力
    y = saleh(v)                   # PA 出力
    z = y/g0                       # 利得正規化した観測（v と同レベル）
    Phi = mp_basis(z)              # 後段逆モデルの基底 Φ(z)
    A = Phi.conj().T@Phi; A += 1e-3*np.trace(A)/A.shape[0]*np.eye(A.shape[0])
    w = np.linalg.solve(A, Phi.conj().T@v)   # v ≈ Φ(z) w を最小二乗
    u = mp_basis(v)@w              # 予歪器出力 u=Φ(v)w（同じ w を前段へ）
    return saleh(u)                # 予歪後に PA 通過


def main():
    x = gen_64qam_rrc()
    print("\n===== 実条件 バックオフ vs マスク 予測（Saleh 代表モデル）=====")
    print(f"64QAM SC, α={ALPHA}, Rs={RS/1e3:.1f}ksps, 占有≒405kHz")
    print(f"マスク: ±250kHz ≤ −37 dBc / ±750kHz ≤ −48 dBc")
    print("-"*64)
    print(f"{'OBO[dB]':>8}{'@±250k[dBc]':>14}{'@±750k[dBc]':>14}{'判定':>10}")
    print("-"*64)
    results = []
    for drive in [0.10, 0.14, 0.18, 0.22, 0.28, 0.35, 0.45]:
        y, obo = run_pa(x, drive)
        f, p = psd_dbc(y)
        s250 = shoulder(f, p, 250e3)
        s750 = shoulder(f, p, 750e3)
        ok = (s250 <= -37) and (s750 <= -48)
        print(f"{obo:>8.1f}{s250:>14.1f}{s750:>14.1f}{'  ○ 合格' if ok else '  × 不足':>10}")
        results.append((obo, s250, s750, ok, y, f, p))
    print("-"*64)
    # デバイス対応
    print("デバイス対応（平均2W=+33dBm 出力時）:")
    print("  ・25W(+44dBm)品 → OBO 11 dB    ・10W(+40dBm)品 → OBO 7 dB")

    # 代表点(10W品=OBO~7-8dB)で DPD 効果
    dr = 0.22
    y0, obo0 = run_pa(x, dr)
    f0, p0 = psd_dbc(y0)
    yd = dpd_once(dr, x); fd, pd = psd_dbc(yd)
    print(f"\n>>> DPD 効果（OBO≈{obo0:.1f}dB ＝10W品を2Wで運用する点）:")
    print(f"    無DPD : ±250k {shoulder(f0,p0,250e3):.1f} / ±750k {shoulder(f0,p0,750e3):.1f} dBc")
    print(f"    MP-DPD: ±250k {shoulder(fd,pd,250e3):.1f} / ±750k {shoulder(fd,pd,750e3):.1f} dBc")
    _fig(results, (f0, p0, obo0), (fd, pd))


def _fig(results, nodpd, dpd):
    try:
        import matplotlib; matplotlib.use("Agg"); import matplotlib.pyplot as plt, os
    except Exception as e:
        print("(no matplotlib:", e, ")"); return
    os.makedirs("docs/figures", exist_ok=True)
    fig, ax = plt.subplots(1, 2, figsize=(12, 4.4))
    # 左: OBO vs shoulder
    obos = [r[0] for r in results]
    ax[0].plot(obos, [r[1] for r in results], "o-", color="#d1242f", label="±250kHz")
    ax[0].plot(obos, [r[2] for r in results], "s-", color="#1a7f37", label="±750kHz")
    ax[0].axhline(-37, ls="--", color="#d1242f", alpha=.6); ax[0].axhline(-48, ls="--", color="#1a7f37", alpha=.6)
    ax[0].axvline(11, ls=":", color="#888"); ax[0].axvline(7, ls=":", color="#888")
    ax[0].text(11, -68, "25W\n@2W", fontsize=8, ha="center", color="#555")
    ax[0].text(7, -68, "10W\n@2W", fontsize=8, ha="center", color="#555")
    ax[0].set_xlabel("output backoff OBO [dB]"); ax[0].set_ylabel("shoulder [dBc]")
    ax[0].set_title("Backoff vs spectral shoulder\n(mask: -37/-48 dBc)")
    ax[0].legend(fontsize=8); ax[0].grid(alpha=.3); ax[0].invert_xaxis()
    # 右: スペクトル(OBO~7dB) 無DPD vs DPD + マスク
    f0, p0, obo0 = nodpd; fd, pd = dpd
    ax[1].plot(f0/1e3, p0, color="#888", lw=1, label=f"no DPD (OBO {obo0:.0f}dB)")
    ax[1].plot(fd/1e3, pd, color="#1a7f37", lw=1, label="MP-DPD")
    for off, lim in MASK:
        ax[1].plot([off/1e3, off/1e3], [lim, 3], "--", color="#8250df", lw=1.2)
        ax[1].plot([-off/1e3, -off/1e3], [lim, 3], "--", color="#8250df", lw=1.2)
        ax[1].plot([off/1e3-60, off/1e3+60], [lim, lim], "-", color="#8250df", lw=1.4)
        ax[1].plot([-off/1e3-60, -off/1e3+60], [lim, lim], "-", color="#8250df", lw=1.4)
    ax[1].set_xlim(-1200, 1200); ax[1].set_ylim(-75, 5)
    ax[1].set_xlabel("offset [kHz]"); ax[1].set_ylabel("PSD [dBc]")
    ax[1].set_title("Spectrum vs real mask (-37/-48 dBc)")
    ax[1].legend(fontsize=8); ax[1].grid(alpha=.3)
    fig.tight_layout(); fig.savefig("docs/figures/backoff_mask.png", dpi=130)
    print("figure saved: docs/figures/backoff_mask.png")


if __name__ == "__main__":
    main()
