#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
スペクトル矩形度 vs PAPR トレードオフ（送信 RRC パルス整形のロールオフ掃引）
==============================================================================

目的:
  「規格マスクより綺麗な長方形」を送信フィルタで作るとき、
  ロールオフ β を小さくするほど**急峻な矩形**になるが**PAPR が上がる**、
  という基本トレードオフを数値と図で示す。

  - 変調: 単一キャリア 64QAM（狭帯域・音声リンク相当）
  - 整形: ルートレイズドコサイン(RRC), ロールオフ β を掃引
  - 指標:
      * PAPR [dB]（0.01% CCDF 近似 = 実質ピーク）
      * シェイプファクタ SF = B(-30dB)/B(-3dB)（1 に近いほど矩形）
  - 図: β 別スペクトル＋規格マスク(例)を重畳

依存: numpy, matplotlib(任意)

音声チャネルの実値（占有帯域・シンボルレート・マスク）が分かれば、
下の PARAMS を差し替えるだけで本物のマージン評価になる。
"""
import numpy as np

rng = np.random.default_rng(31)

# ===== 実値が分かればここを差し替え（音声チャネル） =====
PARAMS = dict(
    sym_rate_hint="(音声: 実シンボルレートを入れると Hz 軸になる)",
    rolloffs=[0.1, 0.22, 0.35, 0.5],   # 掃引するロールオフ β
    sps=8,                              # 1シンボルあたりサンプル数
    span=12,                            # RRC フィルタ長(シンボル)
    n_sym=8000,
)


def rrc(beta, sps, span):
    """ルートレイズドコサイン フィルタ係数。"""
    N = span * sps
    t = (np.arange(-N / 2, N / 2 + 1)) / sps
    h = np.zeros_like(t)
    for i, ti in enumerate(t):
        if abs(ti) < 1e-8:
            h[i] = 1 - beta + 4 * beta / np.pi
        elif beta > 0 and abs(abs(ti) - 1 / (4 * beta)) < 1e-6:
            h[i] = (beta / np.sqrt(2)) * (
                (1 + 2 / np.pi) * np.sin(np.pi / (4 * beta)) +
                (1 - 2 / np.pi) * np.cos(np.pi / (4 * beta)))
        else:
            h[i] = (np.sin(np.pi * ti * (1 - beta)) +
                    4 * beta * ti * np.cos(np.pi * ti * (1 + beta))) / \
                   (np.pi * ti * (1 - (4 * beta * ti) ** 2))
    return h / np.sqrt(np.sum(h ** 2))


def gen_64qam(n):
    m = 8
    lv = np.arange(-(m - 1), m, 2)
    pts = (lv[None, :] + 1j * lv[:, None]).ravel()
    pts /= np.sqrt((np.abs(pts) ** 2).mean())
    return pts[rng.integers(0, pts.size, n)]


def papr_db(x):
    p = np.abs(x) ** 2
    peak = np.percentile(p, 99.99)     # 0.01% CCDF 近似
    return 10 * np.log10(peak / p.mean())


def psd_db(x, osr):
    N = 1 << int(np.floor(np.log2(x.size)))
    X = np.fft.fftshift(np.fft.fft(x[:N] * np.hanning(N)))
    p = 20 * np.log10(np.abs(X) + 1e-12)
    p -= p.max()
    f = np.linspace(-0.5, 0.5, N) * osr   # シンボルレート基準
    return f, p


def shape_factor(f, p):
    """SF = B(-30dB)/B(-3dB)。1 に近いほど矩形(ブリックウォール)。"""
    def bw(th):
        idx = np.where(p > th)[0]
        return (f[idx].max() - f[idx].min()) if idx.size else np.nan
    b3, b30 = bw(-3), bw(-30)
    return b30 / b3 if b3 else np.nan


def main():
    sps, span, n = PARAMS['sps'], PARAMS['span'], PARAMS['n_sym']
    syms = gen_64qam(n)
    up = np.zeros(n * sps, dtype=complex); up[::sps] = syms

    print("\n===== 送信 RRC ロールオフ β: 矩形度 vs PAPR =====")
    print(f"{'β':>6}{'PAPR[dB]':>12}{'SF=B-30/B-3':>16}{'評価':>18}")
    print("-" * 54)
    results = []
    for beta in PARAMS['rolloffs']:
        h = rrc(beta, sps, span)
        x = np.convolve(up, h, mode='same')
        x /= np.sqrt((np.abs(x) ** 2).mean())
        f, p = psd_db(x, sps)
        sf = shape_factor(f, p)
        pa = papr_db(x)
        note = "急峻な矩形/PAPR高" if beta <= 0.15 else \
               ("丸い/PAPR低" if beta >= 0.45 else "中間")
        print(f"{beta:>6.2f}{pa:>12.2f}{sf:>16.2f}{note:>16}")
        results.append((beta, x, f, p, pa, sf))
    print("-" * 54)
    print("β↓ で矩形に近づく(SF→1)が PAPR は増加 → DPD/バックオフ余裕とのバランスで決める")
    _fig(results)
    return results


def _fig(results):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        import os
    except Exception as e:
        print("(no matplotlib:", e, ")"); return
    os.makedirs("docs/figures", exist_ok=True)
    fig, ax = plt.subplots(1, 2, figsize=(12, 4.4))
    colors = ["#d1242f", "#e8710a", "#1a7f37", "#1f6feb"]
    for (beta, x, f, p, pa, sf), c in zip(results, colors):
        ax[0].plot(f, p, lw=1.1, color=c,
                   label=f"β={beta} (PAPR {pa:.1f}dB, SF {sf:.2f})")
    # 規格マスク(例): 帯域端 ±0.5 シンボルレート
    e = 0.5
    mf = [-2, -0.9, -0.9, -e, -e, e, e, 0.9, 0.9, 2]
    mm = [-55, -55, -33, -33, 2, 2, -33, -33, -55, -55]
    ax[0].plot(mf, mm, "--", color="#8250df", lw=1.6, label="spec mask (example)")
    ax[0].set_xlim(-2, 2); ax[0].set_ylim(-70, 5)
    ax[0].set_xlabel("relative freq (x symbol rate)")
    ax[0].set_ylabel("normalized PSD [dB]")
    ax[0].set_title("Tx RRC roll-off: spectrum vs mask\n(smaller beta = sharper rectangle)")
    ax[0].legend(fontsize=7); ax[0].grid(alpha=0.3)

    betas = [r[0] for r in results]
    paprs = [r[4] for r in results]
    sfs = [r[5] for r in results]
    ax2 = ax[1]; ax3 = ax2.twinx()
    ax2.plot(betas, paprs, "o-", color="#d1242f", label="PAPR [dB]")
    ax3.plot(betas, sfs, "s--", color="#1a7f37", label="shape factor (→1: rectangular)")
    ax2.set_xlabel("RRC roll-off  beta")
    ax2.set_ylabel("PAPR [dB]", color="#d1242f")
    ax3.set_ylabel("shape factor B(-30)/B(-3)", color="#1a7f37")
    ax2.set_title("Trade-off: rectangularity vs PAPR")
    ax2.grid(alpha=0.3)
    fig.tight_layout()
    fig.savefig("docs/figures/spectrum_shaping.png", dpi=130)
    print("figure saved: docs/figures/spectrum_shaping.png")


if __name__ == "__main__":
    main()
