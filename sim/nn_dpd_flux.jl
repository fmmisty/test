# nn_dpd_flux.jl
# =====================================================================
# ニューラルネット DPD（NN-DPD）雛形 — Julia / Flux.jl
# ---------------------------------------------------------------------
# AI 埋め込み DPD の Julia 実装雛形。間接学習(ILA)のポストインバースを
# MLP で学習する。入力は MP 基底（Re/Im 分解）を特徴として与える。
#
# 依存: Flux, LinearAlgebra, Statistics, DelimitedFiles
#   （初回: julia -e 'using Pkg; Pkg.add(["Flux","DSP"])'）
#
# ※この作業環境では Julia を導入できない（egress 制限）ため、
#   ご自身のマシンで実行してください。ロジックは NumPy 版
#   (sim/nn_dpd_demo.py, EVM 7.6%/ACLR 30dB で 64QAM 合格) と同型。
#
# 実行:
#   julia nn_dpd_flux.jl              # 合成データ自己テスト
#   julia nn_dpd_flux.jl meas.csv     # 実測 x_re,x_im,y_re,y_im
# =====================================================================

using Flux, LinearAlgebra, Statistics, Random, DelimitedFiles
Random.seed!(7)

# ---- MP 基底（Re/Im 分解）を NN 特徴に ----
function mp_features(x::Vector{ComplexF64}; order=7, memory=5, tap=8)
    N = length(x); ax = abs.(x); cols = Vector{Vector{Float64}}()
    for m in 0:memory, k in 1:2:order
        d = m*tap
        xm  = [i>d ? x[i-d]  : 0.0+0im for i in 1:N]
        axm = [i>d ? ax[i-d] : 0.0     for i in 1:N]
        t = xm .* axm.^(k-1)
        push!(cols, real.(t)); push!(cols, imag.(t))
    end
    return permutedims(hcat(cols...))          # (features, N) : Flux は列=サンプル
end

# ---- 合成 Wiener 型 GaN PA（自己テスト） ----
function synth(N=200_000; s=0.2, mem=0.3, tap=8)
    x = (randn(N).+im*randn(N)); x ./= sqrt(mean(abs2,x))
    h = zeros(ComplexF64,3tap+1); h[1]=1
    h[1+tap]=mem*(0.30-0.18im); h[1+2tap]=mem*(-0.16+0.11im); h[1+3tap]=mem*(0.07-0.04im)
    v = similar(x)
    @inbounds for n in 1:N
        acc=0.0+0im
        for (j,hj) in enumerate(h); n-j+1>=1 && (acc += hj*(s*x[n-j+1])); end
        v[n]=acc
    end
    aa,ba,ap,bp = 2.0,1.0,1.6,1.0
    r=abs.(v); A=aa.*r./(1 .+ ba.*r.^2); P=ap.*r.^2 ./(1 .+ bp.*r.^2)
    y=(A.*exp.(im*(angle.(v).+P)))./s
    return x,y
end

function main()
    if length(ARGS)>=1 && isfile(ARGS[1])
        D=readdlm(ARGS[1],',',Float64;skipstart=1)
        x=D[:,1].+im*D[:,2]; y=D[:,3].+im*D[:,4]
    else
        println("（合成データ自己テスト）"); x,y=synth()
    end
    G = (y ⋅ x)/(x ⋅ x); z = y./G

    Xf = mp_features(z)                 # 特徴（ポストインバース入力）
    Yt = permutedims(hcat(real.(x), imag.(x)))   # 目標 (2, N)
    μ = mean(Xf,dims=2); σ = std(Xf,dims=2).+1e-9
    Xn = (Xf .- μ)./σ

    nin = size(Xn,1)
    model = Chain(Dense(nin,48,tanh), Dense(48,48,tanh), Dense(48,2))
    opt = Flux.setup(Adam(2e-3), model)
    data = Flux.DataLoader((Xn, Yt), batchsize=4096, shuffle=true)
    for epoch in 1:20
        for (xb,yb) in data
            g = gradient(m->Flux.mse(m(xb), yb), model)[1]
            Flux.update!(opt, model, g)
        end
    end
    # 予歪適用: u = model(features(x))
    Xnx = (mp_features(x) .- μ)./σ
    U = model(Xnx); u = U[1,:] .+ im*U[2,:]
    println("学習完了。u=model(feat(x)) を PA へ入力し EVM/ACLR を実機/モデルで評価する。")
    println("NMSE(post-inverse fit) = ", round(10log10(mean(abs2, model(Xn).-Yt)/mean(abs2,Yt)),digits=2), " dB")
end

main()

# =====================================================================
# メモ:
#  - メモリを明示的に扱うなら Chain の先頭を GRU((nin=>H)) 等の RNN に。
#  - EVM/ACLR は DSP.jl / FFTW.jl でスペクトルを取り評価。
#  - 実機化(FPGA)は量子化(Float16/固定小数点)で。OpenDPD(PyTorch)も参照。
# =====================================================================
