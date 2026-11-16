# dpd_backoff_from_data.jl
# =====================================================================
# 実測 I/Q データ → バックオフ vs スペクトルマスク 評価（RBW整合）
# ---------------------------------------------------------------------
# 手順（この順で評価する）:
#   STEP0  PA前の送信波形 x がマスクから何dB下か（= 有限長フィルタの漏れ分）
#   STEP1  実測 I/Q(x,y) から PA 挙動モデル(MP)を同定（そのアンプの実特性）
#   STEP2  実測 y そのもののマスク余裕（キャプチャ運用点, 外挿なし）
#   STEP3  OBO 掃引で DPDなし/MP-DPD のマスク余裕（モデルベース, 要実機再測）
#
#   ★ 測定は「RBW 3 kHz」「基準レベル整合」で行う（規定のマスク測定に合わせる）。
#   ★ フィルタ漏れ(STEP0)と PA再成長(STEP2/3)は分けて読む。
#
# 依存: LinearAlgebra, Statistics, Printf, DelimitedFiles, FFTW
# 実行: julia dpd_backoff_from_data.jl meas.csv   (x_re,x_im,y_re,y_im)
#   ※Julia はご自身のマシンで（この環境は egress 制限で導入不可）。
# =====================================================================

using LinearAlgebra, Statistics, Printf, DelimitedFiles, FFTW

# ======================  ① ここだけ設定  ============================
const FS_HZ    = 2.0e6                       # ★ I/Q のサンプリング周波数 [Hz]
const RS_HZ    = 80.0e3                      # ★ シンボルレート（現行80ksym/s）
const ALPHA    = 0.2                         # ロールオフ
const PAVG_W   = 5.0                         # 平均出力目標 [W]
# スペクトルマスク: (離調[Hz], 上限[dBc])
const MASK     = [(50.0e3, -37.0), (150.0e3, -48.0)]
const RBW_HZ   = 3.0e3                       # ★ マスク測定の分解能帯域幅（規定に合わせる）
# 基準レベル(dBcの分母): :peak_rbw = RBW内ピーク電力 / :total = 占有帯域の総電力
const REF_MODE = :peak_rbw                   # ★ 規定の基準に合わせて選ぶ
const OCC_HZ   = RS_HZ*(1+ALPHA)             # 占有帯域(理想) = Rs(1+α)。:total の積分範囲
# モデル
const ORDER=7; const MEMORY=5; const TAP=1
# 理想帯域端 f_edge = Rs(1+α)/2 （参考表示）
# =====================================================================

# ---------- MP 基底 / 同定 / DPD ----------
function mp_basis(x::Vector{ComplexF64}; order=ORDER, memory=MEMORY, tap=TAP)
    N=length(x); ax=abs.(x); ks=1:2:order; cols=Vector{Vector{ComplexF64}}()
    for m in 0:memory
        d=m*tap
        xm =[i>d ? x[i-d]  : 0im for i in 1:N]; axm=[i>d ? ax[i-d] : 0.0 for i in 1:N]
        for k in ks; push!(cols, xm .* axm.^(k-1)); end
    end
    hcat(cols...)
end
function identify_pa(x,y); Φ=mp_basis(x); θ=Φ\y; θ, 10*log10(sum(abs2,y.-Φ*θ)/sum(abs2,y)); end
pa_model(x,θ)=mp_basis(x)*θ
function identify_dpd(x,y; λ=1e-3)
    G=(y⋅x)/(x⋅x); z=y./G; Φ=mp_basis(z); A=Φ'Φ; A+=λ*tr(A)/size(A,1)*I; (A\(Φ'x)), G
end
apply_dpd(x,w)=mp_basis(x)*w

# ---------- PSD（線形, 平均ペリオドグラム）----------
function psd_lin(y; nfft=16384)
    N=(length(y)÷nfft)*nfft
    N==0 && (nfft=2^floor(Int,log2(length(y))); N=(length(y)÷nfft)*nfft)
    seg=reshape(@view(y[1:N]), nfft, :)
    w=0.5 .- 0.5 .*cos.(2π.*(0:nfft-1)./(nfft-1)); wn=sum(w.^2)
    S=zeros(nfft)
    for c in 1:size(seg,2); S .+= abs2.(fftshift(fft(seg[:,c].*w))); end
    S ./= (size(seg,2)*wn)                    # 窓補正つき
    fftshift(fftfreq(nfft, FS_HZ)), S
end
# RBW整合: 離調 off を中心に RBW 幅の電力（上下の悪い方）を、基準レベルで割って dBc。
function mask_dbc(f,S,off; rbw=RBW_HZ, ref=REF_MODE)
    binHz=f[2]-f[1]
    bandpow(lo,hi)=sum(@view S[(f.>=lo).&(f.<hi)])*binHz
    po=max(bandpow(off-rbw/2,off+rbw/2), bandpow(-off-rbw/2,-off+rbw/2))
    refp = ref===:total ? bandpow(-OCC_HZ/2, OCC_HZ/2) : begin
        w=max(1,round(Int,rbw/binHz)); cs=cumsum(S); best=0.0
        for i in 1:length(S)-w; best=max(best,(cs[i+w]-cs[i])*binHz); end; best
    end
    10*log10(po/refp + 1e-30)
end
evalmask(y)= [(off, mask_dbc(psd_lin(y)..., off), lim) for (off,lim) in MASK]
function report(tag, y)
    @printf("%-26s", tag)
    for (off,v,lim) in evalmask(y)
        @printf("  ±%.0fk %6.1f dBc(≤%.0f)%s", off/1e3, v, lim, v<=lim ? "○" : "×")
    end
    println()
end

# ---------- バックオフ掃引（同定モデルを PA として使用）----------
function sweep(x̂, θ; w=nothing, scales=0.08:0.03:0.5)
    Psat=maximum([mean(abs2, pa_model(s.*x̂, θ)) for s in 0.1:0.1:1.2])
    for s in scales
        u = w===nothing ? s.*x̂ : (uu=apply_dpd(s.*x̂,w); uu.*sqrt(mean(abs2,s.*x̂)/mean(abs2,uu)))
        y=pa_model(u,θ); obo=-10*log10(mean(abs2,y)/Psat)
        f,S=psd_lin(y); vals=[mask_dbc(f,S,off) for (off,_) in MASK]
        ok=all(vals[i]<=MASK[i][2] for i in eachindex(MASK))
        @printf("  OBO %5.1f dB :", obo)
        for (i,(off,_)) in enumerate(MASK); @printf("  ±%.0fk %6.1f", off/1e3, vals[i]); end
        println(ok ? "   ○" : "")
    end
end

function main()
    fe=RS_HZ*(1+ALPHA)/2
    @printf("設定: Rs=%.0f ksym/s, α=%.2f → 理想帯域端 ±%.1f kHz / 占有≈±%.1f kHz\n",
            RS_HZ/1e3, ALPHA, fe/1e3, OCC_HZ/2e3)
    @printf("マスク測定: RBW=%.1f kHz, 基準=%s\n", RBW_HZ/1e3, REF_MODE)
    @printf("マスク: "); for (o,l) in MASK; @printf("±%.0fk≤%.0fdBc  ",o/1e3,l); end; println("\n")

    if isempty(ARGS) || !isfile(ARGS[1])
        println("※ meas.csv(x_re,x_im,y_re,y_im) を引数に。まだ無いので STEP0 の読み方だけ表示して終了。")
        println("   STEP0: y の代わりに送信波形 x を evalmask して「PA前の余裕」を先に確認する。")
        return
    end
    D=readdlm(ARGS[1], ',', Float64; skipstart=1)
    x=ComplexF64.(D[:,1].+im.*D[:,2]); y=ComplexF64.(D[:,3].+im.*D[:,4])
    @printf("読込: %s  N=%d  Fs=%.3f MHz\n\n", ARGS[1], length(x), FS_HZ/1e6)

    println("── STEP0: PA前の送信波形 x のマスク余裕（＝有限長フィルタの漏れ分）──")
    report("送信波形 x (PA前)", x)
    println("  → ここが既にマスクに近い/超えるなら、まず送信フィルタ(長さ・阻止域)の問題。PAの前に対策。\n")

    x̂ = x ./ sqrt(mean(abs2,x))
    θ, nmse = identify_pa(x̂, y)
    @printf("── STEP1: PA モデル同定(MP %d次/深さ%d): 係数=%d, NMSE=%.2f dB ──\n", ORDER,MEMORY,length(θ),nmse)

    println("\n── STEP2: 実測 y のマスク余裕（キャプチャ運用点, 外挿なし）──")
    report("実測 y (PA後)", y)

    w,G = identify_dpd(x̂, y)
    @printf("\n── STEP3: バックオフ掃引（モデルベース, |G|=%.3f）──\n", abs(G))
    println(" [DPDなし]"); sweep(x̂,θ)
    println(" [MP-DPD]");  sweep(x̂,θ; w=w)
    println("\n※ STEP3 はモデル外挿を含む。運用レベル付近で I/Q を取れば STEP2 が最も確か。")
    println("※ 最終値は実機のスペアナ・マスク測定(RBW・基準レベルを規定どおり)で確定。")
    println("※ 5W換算: 必要OBOが決まれば Psat≈5W×10^(OBO/10)。数値は実機確定後に。")
end
main()

# =====================================================================
# データ準備メモ:
#  - x=送信BB I/Q, y=カプラ→ダウンコン→ADC の観測 I/Q。時間整合・利得整合して CSV 化。
#  - Fs は近接(±50k)+遠方(±150k)+RBW を十分含む帯域（≥ 数百kHz〜MHz）。
#  - RBW_HZ / REF_MODE / OCC_HZ を規定のマスク測定条件に合わせる（基準レベルを一致させる）。
#  - STEP0(フィルタ漏れ) と STEP2/3(PA再成長) を分けて読む。
# =====================================================================
