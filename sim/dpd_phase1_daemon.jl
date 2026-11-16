#!/usr/bin/env julia
# dpd_phase1_daemon.jl ── フェーズ1 PS側デーモン(Julia版, Raspberry Pi 64bit 等)
# =====================================================================
# phase1_harness.py と同一プロトコル・同一の採否制御(方式C)。
# Pi(aarch64)で Julia 公式バイナリが動く前提。依存=標準ライブラリのみ(Sockets, Printf)。
#
# 実行:   julia dpd_phase1_daemon.jl [PORT]        # 既定 5560
# テスト: ホスト等から  python3 phase1_harness.py inject <pi-ip> 5560
#         (pingも可。ロジックは Python版=14/14 PASS と同一。Julia実行はPiで要確認。)
#
# プロトコル(リトルエンディアン, 19byteヘッダ):
#   type:u8 obs:u32 coef:u32 pa:f32 quality:u8 decision:u8 nsamp:u32 (+ payload f32×nsamp)
#   type 1=CAPTURE 2=PENDING 3=CONFIRM 4=RESULT_READY 10=DECISION
#   decision 1=ADOPT 2=HOLD 3=DROP 4=RECALC
# =====================================================================
using Sockets, Printf, LinearAlgebra

const TOL=0.02; const CALC_TIMEOUT=0.30; const HOLD_DEADLINE=0.30
const CAPTURE=1; const PEND=2; const CONF=3; const RDY=4; const DEC=10
const ADOPT=1; const HOLD=2; const DROP=3; const RECALC=4
dname(d)= d==ADOPT ? "ADOPT" : d==HOLD ? "HOLD" : d==DROP ? "DROP" : d==RECALC ? "RECALC" : "-"

# 19byte ヘッダの読み書き(リトルエンディアン=Python struct "<BIIfBBI" と一致)
function read_msg(sock)
    hdr = read(sock, 19)                         # 19バイト揃うまで待つ
    length(hdr) < 19 && return nothing
    io = IOBuffer(hdr)
    t   = read(io, UInt8);  obs = read(io, UInt32); coef = read(io, UInt32)
    pa  = read(io, Float32); q  = read(io, UInt8);  dec  = read(io, UInt8)
    n   = read(io, UInt32)
    payload = n>0 ? read(sock, Int(n)*4) : UInt8[]
    (t=Int(t), obs=obs, coef=coef, pa=Float64(pa), q=Int(q), dec=Int(dec), iq=payload)
end
function send_dec(sock; obs=UInt32(0), coef=UInt32(0), pa=0.0f0, dec=0)
    io=IOBuffer()
    write(io, UInt8(DEC)); write(io, UInt32(obs)); write(io, UInt32(coef))
    write(io, Float32(pa)); write(io, UInt8(0)); write(io, UInt8(dec)); write(io, UInt32(0))
    write(sock, take!(io)); flush(sock)
end

# ---- 係数推定(MP/ILA, θ=Φ\y). 標準ライブラリの線形代数で解く ----
function mp_basis(x::Vector{ComplexF64}; K=5, M=3)
    N=length(x); ax=abs.(x); cols=Vector{Vector{ComplexF64}}()
    for m in 0:M, k in 1:2:K
        xm =[i>m ? x[i-m]  : 0im for i in 1:N]
        axm=[i>m ? ax[i-m] : 0.0 for i in 1:N]
        push!(cols, xm .* axm.^(k-1))
    end
    hcat(cols...)
end
function estimate(iq::Vector{UInt8})
    length(iq) < 64 && return nothing
    f = reinterpret(Float32, iq)                 # [xr,xi,...,yr,yi,...]
    h = length(f) ÷ 4
    x = ComplexF64.(f[1:2:2h] .+ im.*f[2:2:2h])
    y = ComplexF64.(f[2h+1:2:4h] .+ im.*f[2h+2:2:4h])
    n = min(length(x), length(y)); x=x[1:n]; y=y[1:n]
    G=(y⋅x)/(x⋅x); z=y./G
    Φ=mp_basis(z); A=Φ'Φ; A += 1e-3*tr(A)/size(A,1)*I
    A \ (Φ'x)
end

# ======================= 採否制御(方式C) =======================
function serve(sock)
    coef_id=UInt32(0)
    inflight=Dict{UInt32,Any}(); tcap=Dict{UInt32,Float64}()
    hold=nothing; thold=Ref{Union{Nothing,Float64}}(nothing)
    pend=Dict{UInt32,Any}()
    unconfirmed()= any(!v.confirmed for v in values(pend))
    latest_conf_pa()= (c=[v.pa for v in values(pend) if v.confirmed]; isempty(c) ? nothing : c[end])
    function check_timers()
        now=time()
        for obs in collect(keys(inflight))
            if now - get(tcap,obs,now) > CALC_TIMEOUT
                delete!(inflight,obs); delete!(tcap,obs)
                send_dec(sock; obs=obs, coef=coef_id, dec=DROP)
                @printf("   TIMEOUT obs=%d→DROP(現係数維持) coef=%d\n", obs, coef_id)
            end
        end
        if hold !== nothing && thold[] !== nothing && now - thold[] > HOLD_DEADLINE
            send_dec(sock; coef=coef_id, dec=RECALC)
            @printf("   HOLD_DEADLINE→RECALC(現係数維持) coef=%d\n", coef_id)
            hold=nothing; empty!(pend); thold[]=nothing
        end
    end
    while isopen(sock) && !eof(sock)
        # タイマ用に軽くポーリング(バイトが来るまで check_timers)
        while bytesavailable(sock) < 19
            eof(sock) && return
            check_timers(); sleep(0.01)
        end
        m = read_msg(sock); m === nothing && break
        if m.t == CAPTURE
            w = m.q != 0 ? (try estimate(m.iq) catch; nothing end) : nothing
            inflight[m.obs] = (base=m.coef, pa=m.pa, q=m.q, w=w); tcap[m.obs]=time()
        elseif m.t == PEND
            pend[m.obs] = (confirmed=false, pa=m.pa)
        elseif m.t == CONF
            haskey(pend,m.obs) && (pend[m.obs]=(confirmed=true, pa=m.pa))
            if hold !== nothing && !unconfirmed() && !isempty(pend)
                gnow=m.pa; cpa=latest_conf_pa()
                if hold.base==coef_id && abs(hold.pa-cpa)<=TOL && abs(hold.pa-gnow)<=TOL
                    coef_id+=1; d=ADOPT
                else; d=RECALC; end
                send_dec(sock; coef=coef_id, dec=d); hold=nothing; empty!(pend); thold[]=nothing
                @printf("   CONF→%s coef=%d\n", dname(d), coef_id)
            end
        elseif m.t == RDY
            r = pop!(inflight, m.obs, nothing); delete!(tcap, m.obs); gnow=m.pa
            if r === nothing; d=DROP
            elseif r.q==0; d=DROP                              # 無効観測→破棄
            elseif r.base != coef_id; d=DROP                   # 基準係数ID不一致
            elseif unconfirmed()                               # 後続未確定→保留
                hold=(base=r.base, pa=r.pa, w=r.w); d=HOLD; thold[]=time()
            elseif abs(r.pa-gnow)<=TOL; coef_id+=1; d=ADOPT    # 採用
            else; d=RECALC; end                                # 条件不一致→再計算
            if d != HOLD; empty!(pend); thold[]=nothing; end
            send_dec(sock; obs=m.obs, coef=coef_id, dec=d)
            @printf("   RDY obs=%d→%s coef=%d\n", m.obs, dname(d), coef_id)
        end
    end
end

function main()
    port = length(ARGS)>=1 ? parse(Int, ARGS[1]) : 5560
    srv = listen(IPv4(0), port)
    @printf("[daemon-jl] listening 0.0.0.0:%d  (方式C, stdlibのみ)\n", port)
    while true
        sock = accept(srv)
        @printf("[daemon-jl] connected\n")
        try; serve(sock); catch e; @printf("[daemon-jl] err %s\n", e); finally; close(sock); end
        @printf("[daemon-jl] closed\n")
    end
end
main()
