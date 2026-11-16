# dpd_model_from_data.jl
# =====================================================================
# 測定データから PA モデル同定 & DPD 係数抽出（メモリ多項式 MP / GMP）
# ---------------------------------------------------------------------
# 用途:
#   実測した PA の入出力 I/Q データ（複素ベースバンド）から、
#   (1) PA の挙動モデル（MP/GMP）を最小二乗で同定し、
#   (2) 間接学習(ILA)で DPD（プリディストータ）係数を抽出する。
#
# Julia を選ぶ理由: 線形代数（\ 演算子=最小二乗）が速く数式に近く書ける。
#
# 依存: 標準ライブラリのみ（LinearAlgebra, DelimitedFiles, Statistics, Printf）。
#   スペクトル/ACLR まで出すなら FFTW を追加（末尾コメント参照）。
#
# 実行:
#   julia dpd_model_from_data.jl                 # 合成データで自己テスト
#   julia dpd_model_from_data.jl meas.csv        # 実測CSV(x_re,x_im,y_re,y_im)
#
# ※この環境では Julia ランタイムを導入できない（egress 制限）ため、
#   ご自身のマシン（julia インストール済）で実行してください。
# =====================================================================

using LinearAlgebra, Statistics, Printf, DelimitedFiles

# ---------- メモリ多項式(MP) / 一般化メモリ多項式(GMP) 基底 ----------
# 列: x[n-m]·|x[n-m]|^(k-1)  （奇数次 k=1,3,5,..、メモリ m=0..M）
# GMP は交差項 x[n-m]·|x[n-m-l]|^(k-1) を追加（Morgan 2006）。
function mp_basis(x::Vector{ComplexF64}; order::Int=7, memory::Int=5,
                  tap::Int=1)
    N = length(x); ax = abs.(x)
    ks = 1:2:order
    cols = Vector{Vector{ComplexF64}}()
    for m in 0:memory
        d = m*tap
        xm  = [i>d ? x[i-d]  : 0.0+0im for i in 1:N]
        axm = [i>d ? ax[i-d] : 0.0     for i in 1:N]
        for k in ks
            push!(cols, xm .* axm.^(k-1))
        end
    end
    return hcat(cols...)
end

function gmp_basis(x::Vector{ComplexF64}; order::Int=7, memory::Int=5,
                   lag::Int=1, tap::Int=1)
    Φ = mp_basis(x; order=order, memory=memory, tap=tap)
    N = length(x); ax = abs.(x)
    ks = 3:2:order                      # 交差項は3次以上
    cols = Vector{Vector{ComplexF64}}()
    for m in 0:memory, l in 1:lag
        d = m*tap; dl = (m+l)*tap
        xm  = [i>d  ? x[i-d]   : 0.0+0im for i in 1:N]
        axl = [i>dl ? ax[i-dl] : 0.0     for i in 1:N]
        for k in ks
            push!(cols, xm .* axl.^(k-1))
        end
    end
    return isempty(cols) ? Φ : hcat(Φ, hcat(cols...))
end

basis(x; model=:mp, kw...) = model === :gmp ? gmp_basis(x; kw...) : mp_basis(x; kw...)

# ---------- (1) PA モデル同定: y ≈ Φ(x)·θ を最小二乗 ----------
function identify_pa(x, y; model=:mp, kw...)
    Φ = basis(x; model=model, kw...)
    θ = Φ \ y                            # 最小二乗（Julia の \ ）
    ŷ = Φ*θ
    nmse = 10*log10(sum(abs2, y .- ŷ) / sum(abs2, y))
    return θ, nmse
end

# ---------- (2) DPD 係数抽出: 間接学習(ILA) ----------
# 正規化出力 z=y/G に対し Φ(z)·w ≈ x を解き、u=Φ(x)·w を予歪入力とする。
function identify_dpd(x, y; model=:mp, iters::Int=1, λ=1e-5, kw...)
    G = (y ⋅ x) / (x ⋅ x)               # 小信号複素利得の推定
    z = y ./ G
    w = nothing
    for _ in 1:iters
        Φ = basis(z; model=model, kw...)
        A = Φ'Φ; A += λ*tr(A)/size(A,1)*I   # 弱いリッジで安定化
        w = A \ (Φ'x)
        # 予歪入力 u=Φ(x)·w を作り、PA 応答は実機/モデルで評価（ここでは係数のみ返す）
    end
    return w, G
end

# ---------- 合成データ生成（自己テスト用: Wiener型 GaN PA） ----------
function synth_data(N=200_000; s=0.2, mem=0.3, tap=8)
    rng = 1:N
    x = (randn(N) .+ im*randn(N)); x ./= sqrt(mean(abs2, x))
    # メモリFIR（ベースバンド周期タップ）
    h = zeros(ComplexF64, 3*tap+1); h[1]=1
    h[1+tap]=mem*(0.30-0.18im); h[1+2tap]=mem*(-0.16+0.11im); h[1+3tap]=mem*(0.07-0.04im)
    v = similar(x)
    @inbounds for n in 1:N
        acc = 0.0+0im
        for (j,hj) in enumerate(h)
            n-j+1 >= 1 && (acc += hj * (s*x[n-j+1]))
        end
        v[n] = acc
    end
    # Saleh 静的非線形（有界・圧縮）
    aa,ba,ap,bp = 2.0,1.0,1.6,1.0
    r = abs.(v)
    A = aa .* r ./ (1 .+ ba .* r.^2)
    P = ap .* r.^2 ./ (1 .+ bp .* r.^2)
    y = (A .* exp.(im*(angle.(v) .+ P))) ./ s
    return x, y
end

# ---------- メイン ----------
function main()
    if length(ARGS) >= 1 && isfile(ARGS[1])
        D = readdlm(ARGS[1], ',', Float64; skipstart=1)   # x_re,x_im,y_re,y_im
        x = D[:,1] .+ im*D[:,2]; y = D[:,3] .+ im*D[:,4]
        println("測定データ読込: ", ARGS[1], "  N=", length(x))
    else
        println("（合成データで自己テスト。実測は CSV: x_re,x_im,y_re,y_im を引数に）")
        x, y = synth_data()
    end

    println("\n===== PA モデル同定（データ→モデル, NMSE が小さいほど良い）=====")
    for (mdl, memo) in ((:mp,"MP 7次/深さ5"), (:gmp,"GMP 交差項"))
        θ, nmse = identify_pa(x, y; model=mdl, order=7, memory=5, tap=8, lag=1)
        @printf("%-14s  係数数=%3d  NMSE=%6.2f dB\n", memo, length(θ), nmse)
    end

    println("\n===== DPD 係数抽出（間接学習 ILA）=====")
    w, G = identify_dpd(x, y; model=:mp, order=7, memory=5, tap=8, iters=1)
    @printf("MP DPD 係数数=%d,  推定小信号利得|G|=%.3f (%.1f dB)\n",
            length(w), abs(G), 20log10(abs(G)))
    println("→ 得られた w を送信側 Φ(x)·w に適用すれば予歪。実機出力で NMSE/ACLR を再評価する。")
end

main()

# =====================================================================
# 拡張メモ:
#  - スペクトル/ACLR: `using FFTW` を足し、fftshift(fft(·)) で PSD を計算。
#  - AI 埋め込み（ニューラルネット DPD）: `using Flux` で
#      model = Chain(Dense(nin,32,tanh), Dense(32,32,tanh), Dense(32,2))
#    のように実部/虚部回帰。時系列メモリは入力に過去サンプルを並べるか
#    Flux の RNN/GRU を使う（OpenDPD 相当）。三菱系でも AI 埋め込み DPD を研究。
#  - 実測データ形式: 送信 I/Q(x) と観測受信 I/Q(y) を時間整合・利得整合して CSV 化。
# =====================================================================
