#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
6.5 GHz(M バンド) GaN HEMT アンプ向け DPD メモリー効果デモ
================================================================

目的:
  - GaN HEMT の PA を「メモリ効果あり」の一般化メモリ多項式(GMP)で模擬。
  - 64QAM/OFDM 信号(占有帯域 ≒17.5MHz, FPU相当)を通し、EVM/ACLR を評価。
  - 3 条件を比較して「メモリー効果が本当に出るか」を定量的に示す:
      (1) DPD なし
      (2) メモリレス DPD (静的多項式のみ)
      (3) メモリ付き DPD (メモリ多項式, ILA同定)
    → メモリレス DPD が取り切れない残留歪み = メモリー効果の証拠。

依存: numpy, matplotlib(任意, 図出力用)
出力: コンソールの数値表 + docs/figures/*.png
"""

import numpy as np

rng = np.random.default_rng(20260903)


# ----------------------------------------------------------------------
# 1. 信号生成: 64QAM OFDM (FPU: 占有帯域 ≒17.5MHz を模擬)
# ----------------------------------------------------------------------
def gen_ofdm_64qam(n_sym=180, n_sc=1024, cp=128, occ_ratio=0.7, osr=8):
    """64QAM OFDM ベースバンド信号を生成し、osr 倍にオーバーサンプル。"""
    # 64QAM コンスタレーション (Gray, 正規化)
    m = 8
    levels = np.arange(-(m - 1), m, 2)  # -7..7
    pts = (levels[None, :] + 1j * levels[:, None]).ravel()
    pts /= np.sqrt((np.abs(pts) ** 2).mean())

    n_used = int(n_sc * occ_ratio)
    n_used -= n_used % 2
    tx_sym = []
    data_sym = []
    for _ in range(n_sym):
        f = np.zeros(n_sc, dtype=complex)
        d = pts[rng.integers(0, pts.size, n_used)]
        data_sym.append(d)
        # 占有部分を中心に配置(DC 除く)
        half = n_used // 2
        f[1:half + 1] = d[:half]
        f[-half:] = d[half:]
        t = np.fft.ifft(f) * np.sqrt(n_sc)
        t = np.concatenate([t[-cp:], t])  # CP 付加
        tx_sym.append(t)
    x = np.concatenate(tx_sym)
    # オーバーサンプリング(ゼロ挿入 + FFT ローパス)
    X = np.fft.fft(x)
    Xup = np.zeros(x.size * osr, dtype=complex)
    Xup[:x.size // 2] = X[:x.size // 2]
    Xup[-(x.size - x.size // 2):] = X[x.size // 2:]
    xup = np.fft.ifft(Xup) * osr
    # 電力正規化(rms=1)
    xup /= np.sqrt((np.abs(xup) ** 2).mean())
    meta = dict(n_sc=n_sc, cp=cp, osr=osr, n_used=n_used, pts=pts,
                data_sym=data_sym, n_sym=n_sym)
    return xup.astype(complex), meta


# ----------------------------------------------------------------------
# 2. PA モデル: 一般化メモリ多項式(GMP) — メモリ効果あり
#    y[n] = Σ_k Σ_m a_km |x[n-m]|^k x[n-m]  (+ 交差項)
#    GaN の圧縮(AM/AM)・AM/PM・長期メモリ(トラップ的)を含む係数を設定。
# ----------------------------------------------------------------------
class GaNPA:
    """Wiener 型 GaN PA モデル: 線形メモリ FIR → Saleh 静的非線形(AM/AM, AM/PM)。
    - Saleh は常に圧縮的(有界)なので DPD が安定に収束する。
    - メモリは FIR h[] が担い、これにより AM/AM が「多価(ヒステリシス)」になる
      = メモリー効果。h を強める/弱めるでメモリー寄与を制御できる。
    """
    def __init__(self, backoff_scale=0.18, mem_strength=0.3, tap_spacing=8):
        self.s = backoff_scale               # 動作点(大きいほど飽和)
        # Saleh パラメータ(AM/AM 圧縮, AM/PM 位相)
        self.aa, self.ba = 2.0, 1.0          # A(r)=aa r/(1+ba r^2)
        self.ap, self.bp = 1.6, 1.0          # Φ(r)=ap r^2/(1+bp r^2) [rad]
        # メモリ FIR: タップをベースバンド周期(=tap_spacing サンプル)で配置し、
        # 占有帯域(≒17.5MHz)内で応答が変化する「本物のメモリ効果」を作る。
        # 非対称な複素タップにより ACLR の上下非対称も再現。
        taps = np.array([1.0,
                         mem_strength * (0.30 - 0.18j),
                         mem_strength * (-0.16 + 0.11j),
                         mem_strength * (0.07 - 0.04j)])
        h = np.zeros((len(taps) - 1) * tap_spacing + 1, dtype=complex)
        h[::tap_spacing] = taps
        self.h = h

    def _saleh(self, v):
        r = np.abs(v)
        A = self.aa * r / (1 + self.ba * r ** 2)
        P = self.ap * r ** 2 / (1 + self.bp * r ** 2)
        ph = np.angle(v)
        return A * np.exp(1j * (ph + P))

    def small_signal_gain(self):
        return self.aa  # r→0 で A(r)/r → aa, 位相→0

    def __call__(self, x):
        v = np.convolve(self.s * x, self.h, mode='full')[:x.size]  # メモリ
        y = self._saleh(v)
        return y / self.s   # 動作点で規格化(小信号利得 ≈ aa)


# ----------------------------------------------------------------------
# 3. DPD: メモリ多項式基底 + 間接学習(ILA) による係数同定
#    memory=0 → メモリレス DPD, memory>0 → メモリ付き DPD
# ----------------------------------------------------------------------
def mp_basis(x, order=7, memory=2, tap_spacing=1):
    """メモリ多項式(MP)基底 (奇数次 1,3,5,.. × メモリ 0..memory)。
        列: x[n-m]·|x[n-m]|^{p-1}
    tap_spacing でメモリタップ間隔を指定(PA のメモリ時定数に整合させる)。"""
    ks = range(1, order + 1, 2)
    cols = []
    ax = np.abs(x)
    for m in range(memory + 1):
        d = m * tap_spacing
        xm = np.roll(x, d); xm[:d] = 0
        axm = np.roll(ax, d); axm[:d] = 0
        for k in ks:
            cols.append(xm * axm ** (k - 1))
    return np.column_stack(cols)


def gmp_basis(x, order=7, memory=2, lag=1, tap_spacing=1):
    """一般化メモリ多項式(GMP)基底 = MP + 遅延包絡線との交差項(Morgan 2006)。
        交差項: x[n-m]·|x[n-m-l]|^{p-1}   (l=1..lag, p=3,5,..)
    遅延した包絡線 |x[n-m-l]| が現在の信号に与える影響を補正する。"""
    cols = [mp_basis(x, order, memory, tap_spacing)]      # 基本の MP 項
    ax = np.abs(x)
    ks = range(3, order + 1, 2)                            # 交差項は 3 次以上
    for m in range(memory + 1):
        d = m * tap_spacing
        xm = np.roll(x, d); xm[:d] = 0
        for l in range(1, lag + 1):
            dl = (m + l) * tap_spacing
            axl = np.roll(ax, dl); axl[:dl] = 0
            for k in ks:
                cols.append(xm * axl ** (k - 1))
    return np.column_stack(cols)


def _basis(x, model, order, memory, lag, tap_spacing):
    if model == 'gmp':
        return gmp_basis(x, order, memory, lag, tap_spacing)
    return mp_basis(x, order, memory, tap_spacing)


def identify_dpd(pa, x, order=7, memory=2, tap_spacing=1, model='mp', lag=1,
                 iters=1, lam=1e-5):
    """間接学習(ILA): ポストインバースをリッジ回帰で同定 → プリディストータに転用。
    正規化領域(PA 出力を小信号利得 G で割る)で「Phi(z)·w ≈ u」を解く。
    model='mp'|'gmp'。tap_spacing は PA のメモリ時定数に整合させる。"""
    G = _lin_gain(pa)
    u = x.copy()                       # 現在の PA 入力(初期=無補正)
    w = None
    for _ in range(iters):
        z = pa(u) / G                  # 正規化 PA 出力
        Phi = _basis(z, model, order, memory, lag, tap_spacing)
        A = Phi.conj().T @ Phi
        A += lam * np.trace(A) / A.shape[0] * np.eye(A.shape[0])
        w = np.linalg.solve(A, Phi.conj().T @ u)
        u = _basis(x, model, order, memory, lag, tap_spacing) @ w
    return w, G


def apply_dpd(x, w, order=7, memory=2, tap_spacing=1, model='mp', lag=1):
    return _basis(x, model, order, memory, lag, tap_spacing) @ w


def _lin_gain(pa):
    """小振幅での複素小信号利得(線形項 a0[0]*s/s = 1 相当)を数値推定。"""
    t = 1e-3 * np.exp(1j * np.linspace(0, 2 * np.pi, 256, endpoint=False))
    y = pa(t)
    return np.vdot(t, y) / np.vdot(t, t)


# ----------------------------------------------------------------------
# 4. 評価指標: ACLR, EVM
# ----------------------------------------------------------------------
def aclr(x, osr, occ_frac=0.7, guard=0.02):
    """隣接チャネル漏洩比(dB, 下側/上側の悪い方)。"""
    N = 1 << int(np.floor(np.log2(x.size)))
    X = np.fft.fftshift(np.fft.fft(x[:N]))
    psd = np.abs(X) ** 2
    f = np.linspace(-0.5, 0.5, N)          # 正規化周波数(fs 基準)
    bw = occ_frac / osr                     # 主チャネル帯域(正規化)
    def band(c):
        m = (f > c - bw / 2) & (f < c + bw / 2)
        return psd[m].sum()
    main = band(0.0)
    adj = min(main / band(+ (bw + guard)), main / band(- (bw + guard)))
    return 10 * np.log10(adj)


def evm_ofdm(x_rx, x_tx, meta):
    """受信信号を OFDM 復調し、送信シンボル(data_sym)との EVM(%) を計算。
    定数の複素利得(振幅・位相)はコンスタレーション領域で最小二乗補正する。"""
    osr, n_sc, cp = meta['osr'], meta['n_sc'], meta['cp']
    L = meta['n_sym'] * (n_sc + cp)
    # 帯域内成分の厳密ダウンサンプル(gen のアップサンプルの逆変換)
    Xup = np.fft.fft(x_rx[:L * osr]) / osr
    X = np.concatenate([Xup[:L // 2], Xup[-(L - L // 2):]])
    rx = np.fft.ifft(X)
    sym_len = n_sc + cp
    half = meta['n_used'] // 2
    rxd_all, d_all = [], []
    for i in range(meta['n_sym']):
        seg = rx[i * sym_len + cp:(i + 1) * sym_len]
        if seg.size < n_sc:
            break
        F = np.fft.fft(seg) / np.sqrt(n_sc)
        rxd_all.append(np.concatenate([F[1:half + 1], F[-half:]]))
        d_all.append(meta['data_sym'][i])
    rxd = np.concatenate(rxd_all); d = np.concatenate(d_all)
    # 最小二乗の複素利得: rxd ≈ alpha * d
    alpha = np.vdot(d, rxd) / np.vdot(d, d)
    err = rxd / alpha - d
    return 100 * np.sqrt((np.abs(err) ** 2).mean() / (np.abs(d) ** 2).mean())


# ----------------------------------------------------------------------
# 5. メイン
# ----------------------------------------------------------------------
def _amam_dispersion(pa, x):
    """AM/AM の縦分散(%) = メモリー効果の直接指標。メモリなし PA なら ~最小。"""
    g = _lin_gain(pa).real
    ain = np.abs(x); aout = np.abs(pa(x) / g)
    bins = np.linspace(0, ain.max() * 0.9, 40)
    idx = np.digitize(ain, bins)
    sp = []
    for b in range(5, 35):
        m = idx == b
        if m.sum() > 50:
            sp.append(np.std(aout[m]) / (np.mean(aout[m]) + 1e-9))
    return 100 * np.mean(sp)


def aclr_sides(y, osr, occ_frac=0.7, guard=0.02):
    """下側/上側 ACLR(dB) を個別に返す。差 = スペクトル非対称 = メモリー効果。"""
    N = 1 << int(np.floor(np.log2(y.size)))
    X = np.fft.fftshift(np.fft.fft(y[:N])); psd = np.abs(X) ** 2
    f = np.linspace(-0.5, 0.5, N); bw = occ_frac / osr
    band = lambda c: psd[(f > c - bw / 2) & (f < c + bw / 2)].sum()
    main = band(0.0)
    return (10 * np.log10(main / band(-(bw + guard))),
            10 * np.log10(main / band(+(bw + guard))))


def main():
    x, meta = gen_ofdm_64qam()
    osr = meta['osr']
    papr = 10 * np.log10(np.abs(x).max() ** 2 / (np.abs(x) ** 2).mean())

    # === パート A: メモリー効果は出るか(メモリなし PA vs メモリあり PA) ===
    pa_nomem = GaNPA(mem_strength=0.0, tap_spacing=osr)
    pa = GaNPA(tap_spacing=osr)                        # メモリあり(既定 mem=0.3)
    print("\n===== パートA: メモリー効果の有無(PA 単体の特性) =====")
    print(f"{'PA モデル':<18}{'AM/AM 縦分散':>14}{'ACLR下/上[dB]':>18}{'非対称[dB]':>12}")
    for name, p in [("メモリなし PA", pa_nomem), ("メモリあり PA", pa)]:
        g = _lin_gain(p).real
        lo, hi = aclr_sides(p(x) / g, osr)
        print(f"{name:<18}{_amam_dispersion(p, x):>12.2f}%{f'{lo:.1f}/{hi:.1f}':>18}{abs(lo - hi):>11.1f}")
    print("  → メモリあり PA は AM/AM が縦に広がり(多価/ヒステリシス)、ACLR も上下非対称")
    print("    = これがメモリー効果。メモリなし PA では出ない。")

    # === パート B: DPD でメモリー効果が効くか(メモリあり PA を線形化) ===
    g = _lin_gain(pa).real
    y0 = pa(x) / g
    w_ml, _ = identify_dpd(pa, x, order=7, memory=0, tap_spacing=osr)
    y1 = pa(apply_dpd(x, w_ml, 7, 0, osr)) / g
    w_mp, _ = identify_dpd(pa, x, order=7, memory=3, tap_spacing=osr, model='mp')
    y2 = pa(apply_dpd(x, w_mp, 7, 3, osr, model='mp')) / g
    w_gmp, _ = identify_dpd(pa, x, order=7, memory=3, tap_spacing=osr,
                            model='gmp', lag=1)
    y3 = pa(apply_dpd(x, w_gmp, 7, 3, osr, model='gmp', lag=1)) / g
    rows = [("DPD なし", evm_ofdm(y0, x, meta), aclr(y0, osr)),
            ("メモリレス DPD", evm_ofdm(y1, x, meta), aclr(y1, osr)),
            ("MP DPD (メモリ)", evm_ofdm(y2, x, meta), aclr(y2, osr)),
            ("GMP DPD (交差項)", evm_ofdm(y3, x, meta), aclr(y3, osr))]

    print(f"\n===== パートB: 64QAM/OFDM(≒17.5MHz, PAPR≈{papr:.1f}dB) の DPD 評価 =====")
    print("-" * 50)
    print(f"{'条件':<16}{'EVM [%]':>12}{'ACLR [dB]':>14}")
    print("-" * 50)
    for name, ev, ac in rows:
        print(f"{name:<16}{ev:>12.2f}{ac:>14.1f}")
    print("-" * 50)
    print("64QAM 目標: EVM ≦ 8%,  ACLR ≧ ~30 dB")

    print("\n>>> メモリー効果の検証(結論):")
    print(f"    メモリレス DPD : EVM {rows[1][1]:.2f}%  (静的補正のみ → 残留)")
    print(f"    MP DPD        : EVM {rows[2][1]:.2f}%  (メモリ補正で解消)")
    print(f"    GMP DPD       : EVM {rows[3][1]:.2f}%  (交差項でさらに/同等)")
    if rows[1][1] - rows[2][1] > 1.0:
        print("    ⇒ メモリレス DPD では取り切れない残留を、MP/GMP が解消。")
        print("       メモリー効果は明確に『出て』おり、DPD にメモリ項が必須と確認。")
    else:
        print("    ⇒ 差は小さい(モデル設定ではメモリ寄与が軽微)。")

    _make_figures(x, pa, pa_nomem, y0, y1, y3, g, osr)  # 図は memoryless vs GMP
    return rows


def _make_figures(x, pa, pa_nomem, y0, y1, y2, g, osr):
    """英語ラベルで作図(CJK フォント不足の文字化けを回避)。"""
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        import os
    except Exception as e:
        print("(matplotlib not available, skip figures:", e, ")")
        return
    os.makedirs("docs/figures", exist_ok=True)
    fig, ax = plt.subplots(1, 3, figsize=(15, 4.3))
    idx = rng.integers(0, x.size, 5000)

    # 図1: AM/AM (メモリなし) — 単一曲線
    g0 = _lin_gain(pa_nomem).real
    ax[0].scatter(np.abs(x)[idx], np.abs(pa_nomem(x) / g0)[idx], s=2,
                  alpha=0.25, color="#1f6feb")
    ax[0].plot([0, 1.1], [0, 1.1], "k--", lw=1, label="ideal linear")
    ax[0].set_title("AM/AM: memoryless PA\n(single-valued curve)")
    ax[0].set_xlabel("|input|"); ax[0].set_ylabel("|output|"); ax[0].legend(fontsize=8)

    # 図2: AM/AM (メモリあり) — 縦に分散(=メモリー効果)
    ax[1].scatter(np.abs(x)[idx], np.abs(pa(x) / g)[idx], s=2,
                  alpha=0.25, color="#d1242f")
    ax[1].plot([0, 1.1], [0, 1.1], "k--", lw=1, label="ideal linear")
    ax[1].set_title("AM/AM: PA WITH memory\n(vertical spread = memory effect)")
    ax[1].set_xlabel("|input|"); ax[1].set_ylabel("|output|"); ax[1].legend(fontsize=8)
    xmax = np.abs(x)[idx].max() * 1.05
    for a in ax[:2]:
        a.set_xlim(0, xmax); a.set_ylim(0, xmax)

    # 図3: スペクトル(DPD 前後)
    def psd_db(s):
        N = 1 << int(np.floor(np.log2(s.size)))
        S = np.fft.fftshift(np.fft.fft(s[:N] * np.hanning(N)))
        p = 20 * np.log10(np.abs(S) + 1e-12)
        f = np.linspace(-0.5, 0.5, N) * osr
        return f, p - p.max()
    for s, lb, c in [(y0, "no DPD", "#888888"),
                     (y1, "memoryless DPD", "#e8710a"),
                     (y2, "GMP DPD (memory)", "#1a7f37")]:
        f, p = psd_db(s)
        ax[2].plot(f, p, label=lb, lw=1.1, color=c)
    ax[2].set_xlim(-3, 3); ax[2].set_ylim(-70, 3)
    ax[2].set_xlabel("relative freq (x channel BW)")
    ax[2].set_ylabel("normalized PSD [dB]")
    ax[2].set_title("Output spectrum (memory PA)\n+ DPD linearization")
    ax[2].legend(fontsize=8); ax[2].grid(alpha=0.3)
    fig.tight_layout()
    fig.savefig("docs/figures/dpd_memory_result.png", dpi=130)
    print("figure saved: docs/figures/dpd_memory_result.png")


if __name__ == "__main__":
    main()
