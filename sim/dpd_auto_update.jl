#!/usr/bin/env julia
# dpd_auto_update.jl  ── Zynq PS(ARM Linux) 常駐デーモン：係数自動更新
# =====================================================================
# 役割: PL(FPGA)が CAPTURE した観測I/Q を受け取り、MP/ILA で補償係数を推定し、
#       採否制御(採用/保留/破棄/再計算, 特許第12稿 方式C)を経て待機バンクへ反映。
#
# 依存: 標準ライブラリのみ(LinearAlgebra, Mmap, Printf)。← armhf/aarch64 どちらでも可。
#       外部パッケージ無し＝ボードでのネット取得不要・RAM節約・起動安定。
#
# 実行(実機):   sudo julia dpd_auto_update.jl            # /dev/mem を mmap
# 実行(ホスト検証): DPD_MOCK=1 julia dpd_auto_update.jl   # ファイルでPLを模擬
#
# 高速化: 初回のみ `julia --project -e 'using PackageCompiler;
#   create_sysimage([]; sysimage_path="dpd.so")'` で sysimage を作り、
#   `julia -Jdpd.so dpd_auto_update.jl` で起動＝JITを事前化。
# =====================================================================
using LinearAlgebra, Mmap, Printf

# ---------------- 設定(実機のアドレスマップに合わせる) ----------------
const MOCK      = get(ENV, "DPD_MOCK", "0") == "1"
const REG_BASE  = 0x43C00000           # AXI 制御レジスタ物理アドレス(例, PL設計に合わせる)
const IQ_BASE   = 0x30000000           # 観測I/Q DDR(DMA/CMA)物理アドレス(例)
const IQ_NMAX   = 8192                 # 1窓の最大サンプル数
const ORDER     = 7                    # MP 非線形次数
const MEMORY    = 5                    # MP メモリ深さ  (係数数 = ceil(K/2)*(M+1))
const TAP       = 1
const TOL_STATE = 0.02                 # 動作状態一致の許容(例: 正規化状態量)
const CAP_READY = :from_PL             # 観測有効性は PL(QUALIFY) が quality フラグで通知

# レジスタ・オフセット(語単位, PL設計と対応させる)
const R_STATUS=0; const R_EVENT=1; const R_OBS_ID=2; const R_REQ_ID=3
const R_COEF_ID=4; const R_PA_STATE=5; const R_QUALITY=6; const R_NSAMP=7
const R_SWAP=8; const R_BANK_DONE=9; const R_RESULT_ID=10; const R_FAULT=11
# STATUS ビット
const ST_CAPTURE_READY = 0x1
const ST_SWAP_BOUNDARY = 0x2

# ---------------- PL アクセス(実機 mmap / ホスト模擬) ----------------
mutable struct PL
    regs::Vector{UInt32}
    iq::Vector{ComplexF32}
end
function open_pl()::PL
    if MOCK
        # ホスト検証: レジスタ64語とI/Qバッファをメモリ上に用意(別プロセスの模擬PLが書く想定)
        return PL(zeros(UInt32,64), zeros(ComplexF32, IQ_NMAX))
    else
        fd = open("/dev/mem", "r+")
        regs = Mmap.mmap(fd, Vector{UInt32}, (64,),  Int(REG_BASE))
        raw  = Mmap.mmap(fd, Vector{Float32}, (2*IQ_NMAX,), Int(IQ_BASE))
        iq   = reinterpret(ComplexF32, raw)            # [I0,Q0,I1,Q1,...] → 複素
        return PL(regs, iq)
    end
end
rdreg(pl,i)  = pl.regs[i+1]
wrreg(pl,i,v)= (pl.regs[i+1] = UInt32(v))

# ---------------- MP / ILA 係数推定(θ=Φ\y) ----------------
function mp_basis(x::Vector{ComplexF64})
    N=length(x); ax=abs.(x); ks=1:2:ORDER; cols=Vector{Vector{ComplexF64}}()
    for m in 0:MEMORY
        d=m*TAP
        xm =[i>d ? x[i-d]  : 0im for i in 1:N]
        axm=[i>d ? ax[i-d] : 0.0 for i in 1:N]
        for k in ks; push!(cols, xm .* axm.^(k-1)); end
    end
    hcat(cols...)
end
# ILA: 観測(x=送信, y=帰還)から前段DPD係数 w を同定。z=y/G, w: Φ(z)w≈x。
function estimate_dpd(x,y; λ=1e-3)
    G=(y⋅x)/(x⋅x); z=y./G
    Φ=mp_basis(z); A=Φ'Φ; A+=λ*tr(A)/size(A,1)*I
    w=A\(Φ'x)
    nmse=10*log10(sum(abs2, x .- Φ*w)/sum(abs2,x))
    w, G, nmse
end

# ---------------- 採否制御(方式C) の最小状態 ----------------
mutable struct Ctrl
    coef_id::UInt32            # 現用係数ID
    hold::Union{Nothing,NamedTuple}   # 保留結果 (w, base_coef_id, g_obs)
    pend::Union{Nothing,NamedTuple}   # 後続イベントの観測 (obs_id, confirmed, g_obs)
end
Ctrl() = Ctrl(UInt32(0), nothing, nothing)

# PL へ係数を転送し境界同期で切替(戻り: 採用できたか)
function stage_and_swap!(pl, w)::Bool
    write_bank!(pl, w)                       # 待機バンクへ(CRC/範囲/量子化はPLまたはここで検査)
    wrreg(pl, R_SWAP, 1)                     # 切替要求
    # PL が frame boundary で bank_done を立てるのを待つ(簡略: 短時間ポーリング)
    for _ in 1:1000
        if rdreg(pl, R_BANK_DONE) == 1; wrreg(pl, R_SWAP, 0); return true; end
        MOCK ? break : sleep(0.001)
    end
    wrreg(pl, R_SWAP, 0); return MOCK ? true : false
end
function write_bank!(pl, w)
    # 実機: 待機バンクRAMへ w を固定小数点化して書く(量子化・飽和はここで)。
    # 本雛形では省略(PL側BRAM/DDRのバンク領域に合わせて実装)。
    nothing
end

# ---------------- 1観測の読み取り ----------------
function read_capture(pl)
    n = Int(rdreg(pl, R_NSAMP)); n = clamp(n, 16, IQ_NMAX)
    # 本雛形は iq に [観測x; 観測y] が連結されている前提(前半x, 後半y)。実機DMA配置に合わせる。
    half = n ÷ 2
    x = ComplexF64.(pl.iq[1:half])
    y = ComplexF64.(pl.iq[half+1:2half])
    meta = (obs_id = rdreg(pl,R_OBS_ID), req_id = rdreg(pl,R_REQ_ID),
            base_coef_id = rdreg(pl,R_COEF_ID),
            g_obs = Float64(reinterpret(Float32, rdreg(pl,R_PA_STATE))),
            quality = rdreg(pl,R_QUALITY))       # quality!=0 を有効とする
    x, y, meta
end
state_now(pl) = Float64(reinterpret(Float32, rdreg(pl, R_PA_STATE)))

# ---------------- メインループ ----------------
function main()
    pl = open_pl(); ctrl = Ctrl()
    @printf("[dpd] start (MOCK=%s) MP %d次/深さ%d  coef=%d\n", MOCK, ORDER, MEMORY,
            (ORDER÷2+ORDER%2)*(MEMORY+1))
    while true
        st = rdreg(pl, R_STATUS)
        if (st & ST_CAPTURE_READY) == 0
            MOCK ? (return) : (sleep(0.001); continue)   # UIO割込でも可
        end
        x, y, m = read_capture(pl)
        wrreg(pl, R_STATUS, st & ~ST_CAPTURE_READY)       # 受理(一度だけ)
        gnow = state_now(pl)

        # --- 観測が無効(過渡/飽和/同期喪失)なら使わない ---
        if m.quality == 0
            @printf("[dpd] obs %d INVALID → 保留/再観測\n", m.obs_id); continue
        end

        # --- 係数推定 ---
        w, G, nmse = estimate_dpd(x, y)
        @printf("[dpd] est obs=%d base=%d |G|=%.3f NMSE=%.2fdB\n", m.obs_id, m.base_coef_id, abs(G), nmse)

        # --- 採否制御(方式C) ---
        if m.base_coef_id != ctrl.coef_id
            @printf("[dpd] DROP: 基準係数ID不一致 (%d≠%d)\n", m.base_coef_id, ctrl.coef_id)   # 破棄
        elseif ctrl.pend !== nothing && !ctrl.pend.confirmed
            ctrl.hold = (w=w, base_coef_id=m.base_coef_id, g_obs=m.g_obs)                      # 保留
            @printf("[dpd] HOLD: 後続観測未確定。現係数維持\n")
        elseif abs(m.g_obs - gnow) <= TOL_STATE
            if stage_and_swap!(pl, w)                                                          # 採用
                ctrl.coef_id += 1; wrreg(pl, R_COEF_ID, ctrl.coef_id)
                @printf("[dpd] ADOPT → coef_id=%d\n", ctrl.coef_id)
            else
                @printf("[dpd] swap失敗 → 現係数維持\n")
            end
        else
            @printf("[dpd] 条件不一致 → RECALC(有効観測で再推定要求)\n")                        # 再計算
            wrreg(pl, R_EVENT, 1)   # PLへ再CAPTURE要求(設計に合わせる)
        end

        # --- 保留結果の再判定(後続観測が有効確定していれば) ---
        if ctrl.hold !== nothing && ctrl.pend !== nothing && ctrl.pend.confirmed
            h = ctrl.hold
            if h.base_coef_id == ctrl.coef_id && abs(h.g_obs - ctrl.pend.g_obs) <= TOL_STATE &&
               abs(h.g_obs - state_now(pl)) <= TOL_STATE
                stage_and_swap!(pl, h.w); ctrl.coef_id += 1; wrreg(pl, R_COEF_ID, ctrl.coef_id)
                @printf("[dpd] 保留C採用(統合) → coef_id=%d\n", ctrl.coef_id)
            else
                @printf("[dpd] 保留C破棄 → 再計算\n"); wrreg(pl, R_EVENT, 1)
            end
            ctrl.hold = nothing; ctrl.pend = nothing
        end
    end
end

main()

# =====================================================================
# 実装メモ(実機化で確定する項目):
#  - REG_BASE / IQ_BASE / R_* / ビット定義 は PL(FPGA)のアドレスマップと一致させる。
#  - 観測有効性(quality)・g_obs(PA動作状態)の量子化幅、TOL_STATE は実験で決定。
#  - CAPTURE完了は UIO 割込(/dev/uioN)でも可: poll→read で起床、ポーリング負荷を下げる。
#  - 待機バンク書込(write_bank!)は固定小数点化・CRC・範囲検査を実装。切替はPLが frame境界で原子的に。
#  - HOLD期限・再計算上限・pendキュー深さを設け、無限保留/livelockを防ぐ(前回レビュー参照)。
#  - C言語へ移植する場合も本ファイルが参照実装(同じ採否ロジック)。θ=Φ\y は 24×24 正規方程式でよい。
# =====================================================================
