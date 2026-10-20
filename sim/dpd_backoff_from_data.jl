# dpd_backoff_from_data.jl
# =====================================================================
# 実測 I/Q データ → バックオフ vs ACLR 予測（5W・64QAM など任意信号）
# ---------------------------------------------------------------------
# 用途:
#   実測した PA の入出力 I/Q（複素ベースバンド, 時間整合・利得整合済）から、
#   (1) PA 挙動モデル(MP/GMP)を最小二乗で同定（= そのアンプの実特性）、
#   (2) 出力バックオフ(OBO)を掃引して近接/遠方 ACLR(肩)を計算、
#   (3) 目標 ACLR/マスクを満たす OBO を求め、平均出力 5W のデバイス Psat に換算、
#   (4) MP-DPD を適用した場合の改善も同時に評価。
#
#   ★ 代表 Saleh モデルでなく「あなたのアンプの実測特性」で計算する点が肝。
#
# 依存: LinearAlgebra, Statistics, Printf, DelimitedFiles, FFTW
#   （FFTW が無ければ:  julia -e 'import Pkg; Pkg.add("FFTW")' ）
#
# 実行:
#   julia dpd_backoff_from_data.jl meas.csv
#   （meas.csv: ヘッダ1行 + 列  x_re,x_im,y_re,y_im  の複素ベースバンド I/Q）
#
# ※この環境では Julia を導入できない（egress 制限）。julia 導入済の
#   ご自身のマシン(例: Windows, Julia 1.12)で実行してください。
# =====================================================================

using LinearAlgebra, Statistics, Printf, DelimitedFiles, FFTW

# ======================  ① ここだけ設定  ============================
const FS_HZ      = 2.0e6        # ★ I/Q のサンプリング周波数 [Hz]（観測帯域=信号の3〜5倍を推奨）
const PAVG_W     = 5.0          # ★ 平均出力の目標 [W]（今回 5W）
# 目標スペクトルマスク: (オフセット[Hz], 上限[dBc]) を必要なだけ並べる
const MASK       = [(250.0e3, -40.0), (750.0e3, -48.0)]   # ← 160m 運用の規定に合わせて書換え
const MEAS_HALF  = 20.0e3       # 肩を測る窓の半幅 [Hz]
# モデル次数（実務の出発点: 7次/深さ5）。tap は包絡線メモリのサンプル間隔。
const ORDER=7; const MEMORY=5; const TAP=1
# =====================================================================

# ---------- MP / GMP 基底 ----------
function mp_basis(x::Vector{ComplexF64}; order=ORDER, memory=MEMORY, tap=TAP)
    N=length(x); ax=abs.(x); ks=1:2:order
    cols=Vector{Vector{ComplexF64}}()
    for m in 0:memory
        d=m*tap
        xm =[i>d ? x[i-d]  : 0im for i in 1:N]
        axm=[i>d ? ax[i-d] : 0.0 for i in 1:N]
        for k in ks; push!(cols, xm .* axm.^(k-1)); end
    end
    hcat(cols...)
end

# ---------- PA モデル同定: y ≈ Φ(x)·θ ----------
function identify_pa(x,y)
    Φ=mp_basis(x); θ=Φ\y
    nmse=10*log10(sum(abs2, y .- Φ*θ)/sum(abs2,y))
    θ, nmse
end
pa_model(x,θ)=mp_basis(x)*θ    # 同定済モデルで PA 応答を予測

# ---------- MP-DPD 係数（間接学習 ILA, 1パス）----------
function identify_dpd(x,y; λ=1e-3)
    G=(y⋅x)/(x⋅x); z=y./G
    Φ=mp_basis(z); A=Φ'Φ; A+=λ*tr(A)/size(A,1)*I
    w=A\(Φ'x); w, G
end
apply_dpd(x,w)=mp_basis(x)*w

# ---------- PSD と 肩(ACLR) ----------
function psd_dbc(y; nfft=4096)
    N=(length(y)÷nfft)*nfft; nfft=min(nfft,max(N,1))
    N=(length(y)÷nfft)*nfft
    seg=reshape(@view(y[1:N]), nfft, :)
    w=0.5 .- 0.5 .* cos.(2π .* (0:nfft-1) ./ (nfft-1))   # Hann
    S=zeros(nfft)
    for c in 1:size(seg,2)
        S .+= abs2.(fftshift(fft(seg[:,c].*w)))
    end
    S ./= size(seg,2)
    f=fftshift(fftfreq(nfft, FS_HZ))
    p=10 .* log10.(S ./ maximum(S) .+ 1e-20)             # ピーク=0 dBc
    f, p
end
function shoulder(f,p,off; half=MEAS_HALF)
    band(c)=10*log10(mean(10 .^ (p[(f.>c-half).&(f.<c+half)]./10)))
    max(band(off), band(-off))
end

# ---------- バックオフ掃引（同定モデルを PA として使用）----------
# 単位rms入力 x̂ をスケール s で駆動 → y=model(s·x̂)。
function sweep(x̂, θ; w=nothing, scales=0.08:0.03:0.5)
    # Psat 推定: 高駆動側の最大平均出力
    Psat=maximum([mean(abs2, pa_model(s.*x̂, θ)) for s in 0.1:0.1:1.2])
    rows=[]
    for s in scales
        u = w===nothing ? s.*x̂ : begin
                uu=apply_dpd(s.*x̂, w)
                uu .* sqrt(mean(abs2, s.*x̂)/mean(abs2, uu))   # 入力電力を合わせる
            end
        y=pa_model(u, θ)
        pout=mean(abs2,y); obo=-10*log10(pout/Psat)
        f,p=psd_dbc(y)
        sh=[shoulder(f,p,off) for (off,_) in MASK]
        push!(rows,(obo, sh))
    end
    rows
end

# ---------- メイン ----------
function main()
    isempty(ARGS) && (println("使い方: julia dpd_backoff_from_data.jl meas.csv  (x_re,x_im,y_re,y_im)"); return)
    D=readdlm(ARGS[1], ',', Float64; skipstart=1)
    x=ComplexF64.(D[:,1].+im.*D[:,2]); y=ComplexF64.(D[:,3].+im.*D[:,4])
    @printf("読込: %s  N=%d  Fs=%.3f MHz\n", ARGS[1], length(x), FS_HZ/1e6)

    # 入力を単位rmsへ（掃引の基準）
    x̂ = x ./ sqrt(mean(abs2,x))

    θ, nmse = identify_pa(x̂, y)
    @printf("\nPA モデル同定(MP %d次/深さ%d): 係数=%d, NMSE=%.2f dB\n", ORDER,MEMORY,length(θ),nmse)

    # 実測そのものの ACLR（キャプチャ時の運用点）
    f0,p0=psd_dbc(y)
    print("実測 y の肩: ")
    for (off,lim) in MASK; @printf("±%.0fkHz %.1f dBc(目標%.0f)  ", off/1e3, shoulder(f0,p0,off), lim); end
    println()

    w,G = identify_dpd(x̂, y)
    @printf("MP-DPD 係数=%d, 推定利得|G|=%.3f (%.1f dB)\n", length(w), abs(G), 20log10(abs(G)))

    println("\n===== バックオフ掃引（左:無DPD / 右:MP-DPD）=====")
    offs=join([@sprintf("±%.0fk", o/1e3) for (o,_) in MASK], " ")
    @printf("%8s | %-22s | %-22s\n", "OBO[dB]", "無DPD "*offs, "MP-DPD "*offs)
    nod=sweep(x̂,θ); wd=sweep(x̂,θ; w=w)
    pass_nod=pass_dpd=nothing
    for (a,b) in zip(nod,wd)
        obo,sh0=a; _,shd=b
        ok0=all(sh0[i]<=MASK[i][2] for i in eachindex(MASK))
        okd=all(shd[i]<=MASK[i][2] for i in eachindex(MASK))
        s0=join([@sprintf("%6.1f",v) for v in sh0]," "); sd=join([@sprintf("%6.1f",v) for v in shd]," ")
        @printf("%8.1f | %s %s | %s %s\n", obo, s0, ok0 ? "○" : "×", sd, okd ? "○" : "×")
        ok0 && pass_nod===nothing && (pass_nod=obo)
        okd && pass_dpd===nothing && (pass_dpd=obo)
    end

    println("\n===== 結論（平均 $(PAVG_W) W 運用への換算）=====")
    for (name,obo) in (("無DPD",pass_nod), ("MP-DPD",pass_dpd))
        if obo===nothing
            @printf("  %-7s: この掃引範囲ではマスク未達（範囲を広げるか DPD 強化）\n", name)
        else
            psat=PAVG_W*10^(obo/10)
            @printf("  %-7s: 必要 OBO ≈ %.1f dB  →  デバイス Psat ≈ %.1f W\n", name, obo, psat)
        end
    end
    println("※ 最終値は実機再測（DPD OFF/ON のスペアナ・マスク測定, I/Q 不要）で確定すること。")
end
main()

# =====================================================================
# データ準備メモ（★ここが一番大事）:
#  - x = 送信ベースバンド I/Q、y = 方向性カプラ→ダウンコン→ADC の観測 I/Q。
#  - x と y を「時間整合（分数サンプル）」「利得整合（小信号利得で正規化）」してから CSV 化。
#  - サンプリング Fs は信号帯域の 3〜5 倍以上（近接+遠方の肩まで捉えるため）。
#    例: 占有 ~400kHz なら Fs ≥ 1.5〜2 MHz。FS_HZ をその値に設定。
#  - MASK は 160m 運用で満たすべき (オフセット,dBc) に書き換える。
#  - 観測が飽和/帯域不足だと同定が崩れる（ADCに余裕、アンチエイリアス確認）。
# =====================================================================
