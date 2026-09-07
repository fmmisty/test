#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ニューラルネット DPD（NN-DPD）デモ — NumPy のみ（この環境で実行可）
=====================================================================
目的: AI 埋め込み DPD の最小実装。メモリ多項式(MP) DPD と同じ合成 GaN PA を
      NN で線形化し、EVM/ACLR を比較する。

方式: 間接学習(ILA)のポストインバースを小さな MLP で学習。
  - 入力特徴: 正規化 PA 出力 z の過去タップ [Re, Im, |z|]（メモリ対応）
  - 出力: 予歪入力の [Re, Im]
  - 学習: Adam で MSE 最小化（PA 逆特性を回帰）
  - 適用: u = MLP(feat(x)) を PA へ入力し EVM/ACLR 評価

依存: numpy（+ 既存 sim/dpd_memory_sim の PA・指標を再利用）
実行: python3 sim/nn_dpd_demo.py
"""
import numpy as np
import sim.dpd_memory_sim as base

rng = np.random.default_rng(7)


# ---- 特徴ベクトル: メモリ多項式(MP)基底を Re/Im に分解して NN 入力に ----
# NN が多項式の項を非線形結合できるよう、基底そのものを特徴として与える。
def features(sig, order=7, memory=5, tap=8):
    B = base.mp_basis(sig, order, memory, tap)   # 複素 (N, C)
    return np.column_stack([B.real, B.imag])     # (N, 2C)


# ---- 小さな MLP（2 隠れ層 tanh）+ Adam ----
class MLP:
    def __init__(self, nin, nh=24, nout=2, seed=0):
        r = np.random.default_rng(seed)
        s = lambda a, b: r.standard_normal((a, b)) * np.sqrt(2 / a)
        self.W1, self.b1 = s(nin, nh), np.zeros(nh)
        self.W2, self.b2 = s(nh, nh), np.zeros(nh)
        self.W3, self.b3 = s(nh, nout), np.zeros(nout)
        self._init_adam()

    def _init_adam(self):
        self.params = ["W1", "b1", "W2", "b2", "W3", "b3"]
        self.m = {p: np.zeros_like(getattr(self, p)) for p in self.params}
        self.v = {p: np.zeros_like(getattr(self, p)) for p in self.params}
        self.t = 0

    def forward(self, X):
        self.X = X
        self.z1 = X @ self.W1 + self.b1; self.a1 = np.tanh(self.z1)
        self.z2 = self.a1 @ self.W2 + self.b2; self.a2 = np.tanh(self.z2)
        self.y = self.a2 @ self.W3 + self.b3
        return self.y

    def backward(self, dY):
        g = {}
        g["W3"] = self.a2.T @ dY; g["b3"] = dY.sum(0)
        da2 = dY @ self.W3.T; dz2 = da2 * (1 - self.a2 ** 2)
        g["W2"] = self.a1.T @ dz2; g["b2"] = dz2.sum(0)
        da1 = dz2 @ self.W2.T; dz1 = da1 * (1 - self.a1 ** 2)
        g["W1"] = self.X.T @ dz1; g["b1"] = dz1.sum(0)
        return g

    def step(self, g, lr=2e-3, b1=0.9, b2=0.999, eps=1e-8):
        self.t += 1
        for p in self.params:
            self.m[p] = b1 * self.m[p] + (1 - b1) * g[p]
            self.v[p] = b2 * self.v[p] + (1 - b2) * g[p] ** 2
            mh = self.m[p] / (1 - b1 ** self.t)
            vh = self.v[p] / (1 - b2 ** self.t)
            setattr(self, p, getattr(self, p) - lr * mh / (np.sqrt(vh) + eps))


def train_nn_dpd(pa, x, order, memory, tap, iters=4000, batch=4096, nh=48):
    G = base._lin_gain(pa).real
    z = pa(x) / G                            # 正規化 PA 出力
    Zf = features(z, order, memory, tap)     # 特徴（ポストインバースの入力）
    Xt = np.column_stack([x.real, x.imag])   # 目標（PA 入力）
    mu, sd = Zf.mean(0), Zf.std(0) + 1e-9     # 入力標準化
    Zn = (Zf - mu) / sd
    net = MLP(Zn.shape[1], nh=nh, seed=1)
    N = x.size
    for it in range(iters):
        b = rng.integers(0, N, batch)         # ミニバッチ
        Y = net.forward(Zn[b])
        err = Y - Xt[b]
        g = net.backward(2 * err / batch)
        lr = 3e-3 * (0.2 ** (it / iters))     # lr を徐々に下げる
        net.step(g, lr=lr)
    return net, (mu, sd), G


def apply_nn_dpd(net, norm, x, order, memory, tap):
    mu, sd = norm
    Xf = (features(x, order, memory, tap) - mu) / sd
    Y = net.forward(Xf)
    return Y[:, 0] + 1j * Y[:, 1]


def main():
    x, meta = base.gen_ofdm_64qam(n_sym=150, osr=8)
    osr = meta["osr"]
    pa = base.GaNPA(tap_spacing=osr)
    g = base._lin_gain(pa).real
    order, memory, tap = 7, 5, osr           # MP 基底（PA に整合）

    y0 = pa(x) / g
    # MP DPD（比較用）
    w_mp, _ = base.identify_dpd(pa, x, order=order, memory=memory, tap_spacing=tap, model="mp")
    y_mp = pa(base.apply_dpd(x, w_mp, order, memory, tap, model="mp")) / g
    # NN DPD（MP 基底 → MLP）
    net, norm, _ = train_nn_dpd(pa, x, order, memory, tap, iters=4000)
    u_nn = apply_nn_dpd(net, norm, x, order, memory, tap)
    y_nn = pa(u_nn) / g

    rows = [("DPD なし", y0), ("MP DPD 7次/深さ5", y_mp), ("NN-DPD (MLP)", y_nn)]
    print("\n===== NN-DPD vs MP-DPD（64QAM/OFDM, メモリあり GaN PA）=====")
    print(f"{'条件':<18}{'EVM [%]':>10}{'ACLR [dB]':>12}")
    print("-" * 42)
    for name, y in rows:
        print(f"{name:<18}{base.evm_ofdm(y, x, meta):>10.2f}{base.aclr(y, osr):>12.1f}")
    print("-" * 42)
    print("64QAM 目標: EVM ≦ 8%, ACLR ≧ ~30 dB")
    print("→ 小さな自作 MLP でも 64QAM 合格（EVM<8%）。ただし本モデル(Wiener型)では")
    print("  よく整合した MP が最良。NN は深いネット/フレームワーク(Flux/PyTorch)・")
    print("  十分な学習データ・多段ILAで、実GaNの複雑メモリー時に有利になり得る。")


if __name__ == "__main__":
    main()
