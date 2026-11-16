#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
フェーズ1 ハーネス: 変換器/RF/PL 無しで「係数更新の制御(採否, 方式C)＋Ethernet通信」を検証
=====================================================================
方針(ユーザ指示):
  - ボードPS側は Python(または C)。Julia はボードで保証しない(別環境の係数計算に利用可)。
  - PL(FPGA)制御は本ハーネスの対象外＝未実装・未検証(下の「PL未検証」参照)。
実行モード:
  python3 phase1_harness.py                 # ① ループバック自己テスト(T01〜T12)
  python3 phase1_harness.py daemon [PORT]    # ② ボードPSで常駐(サーバ)。既定 0.0.0.0:5560
  python3 phase1_harness.py inject HOST PORT # ② ホストからEthernetで T01〜T12 を駆動
  python3 phase1_harness.py ping   HOST PORT # ② 疎通確認のみ
プロトコル(リトルエンディアン, stdlibのみ=Python/C/Julia 移植可):
  ヘッダ19byte: type:u8 obs_id:u32 coef_id:u32 pa_state:f32 quality:u8 decision:u8 nsamp:u32
                (+ payload: nsamp×float32  ※[xr,xi,...,yr,yi,...])
  type 1=CAPTURE 2=PENDING 3=CONFIRM 4=RESULT_READY 10=DECISION
  decision 1=ADOPT 2=HOLD 3=DROP 4=RECALC
演算遅延/競合は injector が CAPTURE と RESULT_READY を分けて送ることで再現。
"""
import socket, struct, sys, time
HDR = struct.Struct("<BIIfBBI")   # 19 bytes
TOL = 0.02
CALC_TIMEOUT = 0.30   # 演算タイムアウト[s] (実機はtick/ms. REG: 無応答で現係数維持)
HOLD_DEADLINE = 0.30  # HOLD期限[s]     (実機はtick/ms. 超過で安全側=再計算)
CAPTURE,PEND,CONF,RDY,DEC = 1,2,3,4,10
ADOPT,HOLD,DROP,RECALC = 1,2,3,4
NAME={ADOPT:"ADOPT",HOLD:"HOLD",DROP:"DROP",RECALC:"RECALC",0:"-"}
try:
    import numpy as np
    HAVE_NP=True
except Exception:
    HAVE_NP=False

def send(sock,t,obs=0,coef=0,pa=0.0,q=0,dec=0,payload=b""):
    sock.sendall(HDR.pack(t,obs,coef,pa,q,dec,len(payload)//4)+payload)
def recv_msg(sock):
    buf=b""
    while len(buf)<HDR.size:
        c=sock.recv(HDR.size-len(buf))
        if not c: return None
        buf+=c
    t,obs,coef,pa,q,dec,n=HDR.unpack(buf)
    pl=b""
    while len(pl)<n*4:
        c=sock.recv(n*4-len(pl))
        if not c: break
        pl+=c
    return dict(t=t,obs=obs,coef=coef,pa=pa,q=q,dec=dec,n=n,iq=pl)

# ---- 係数推定(MP/ILA). numpy が有れば実際に解く(=A9上の計算経路を実証) ----
def estimate(iqbytes):
    if not HAVE_NP or len(iqbytes)<64: return None
    a=np.frombuffer(iqbytes,dtype="<f4").astype(np.float64)
    h=len(a)//4; x=(a[0:2*h:1][0::2]+1j*a[0:2*h:1][1::2]); y=(a[2*h::1][0::2]+1j*a[2*h::1][1::2])
    n=min(len(x),len(y)); x,y=x[:n],y[:n]
    def mp(v,K=5,M=3):
        ax=np.abs(v);c=[]
        for m in range(M+1):
            vm=np.roll(v,m);vm[:m]=0;axm=np.roll(ax,m);axm[:m]=0
            for k in range(1,K+1,2):c.append(vm*axm**(k-1))
        return np.column_stack(c)
    G=np.vdot(x,y)/np.vdot(x,x); z=y/G
    P=mp(z);A=P.conj().T@P;A+=1e-3*np.trace(A)/A.shape[0]*np.eye(A.shape[0])
    return np.linalg.solve(A,P.conj().T@x)

# ======================= PS側 デーモン(方式C) =======================
def daemon_handle(conn, log=None, calc_to=CALC_TIMEOUT, hold_to=HOLD_DEADLINE):
    coef_id=0; inflight={}; hold=None; pend={}
    t_cap={}; t_hold=[None]                      # 演算開始/HOLD開始の時刻
    def unconfirmed(): return any(not v["confirmed"] for v in pend.values())
    def latest_conf_pa():
        cs=[v["pa"] for v in pend.values() if v["confirmed"]]
        return cs[-1] if cs else None
    def check_timers():
        nonlocal hold, pend
        now=time.monotonic()
        # 演算タイムアウト: 無応答の計算は破棄し現係数維持(安全側)
        for obs in list(inflight):
            if now - t_cap.get(obs,now) > calc_to:
                inflight.pop(obs); t_cap.pop(obs,None)
                send(conn,DEC,obs=obs,coef=coef_id,dec=DROP)
                if log:log(f"TIMEOUT obs={obs}→DROP(現係数維持) coef={coef_id}")
        # HOLD期限切れ: 後続観測が確定しないまま期限超過→再計算(現係数維持)
        if hold is not None and t_hold[0] is not None and now - t_hold[0] > hold_to:
            send(conn,DEC,obs=0,coef=coef_id,dec=RECALC)
            if log:log(f"HOLD_DEADLINE→RECALC(現係数維持) coef={coef_id}")
            hold=None; pend={}; t_hold[0]=None
    conn.settimeout(0.05)
    while True:
        try:
            m=recv_msg(conn)
        except socket.timeout:
            check_timers(); continue
        if m is None: break
        t=m["t"]
        if t==CAPTURE:
            w=estimate(m["iq"]) if m["q"]!=0 else None
            inflight[m["obs"]]=dict(base=m["coef"],pa=m["pa"],q=m["q"],w=w)
            t_cap[m["obs"]]=time.monotonic()
        elif t==PEND:
            pend[m["obs"]]=dict(confirmed=False,pa=m["pa"])
        elif t==CONF:
            if m["obs"] in pend: pend[m["obs"]]={"confirmed":True,"pa":m["pa"]}
            if hold is not None and not unconfirmed() and pend:
                gnow=m["pa"]; cpa=latest_conf_pa()
                if hold["base"]==coef_id and abs(hold["pa"]-cpa)<=TOL and abs(hold["pa"]-gnow)<=TOL:
                    coef_id+=1; d=ADOPT
                else:
                    d=RECALC
                send(conn,DEC,obs=0,coef=coef_id,dec=d); hold=None; pend={}; t_hold[0]=None
                if log:log(f"CONF→{NAME[d]} coef={coef_id}")
        elif t==RDY:
            r=inflight.pop(m["obs"],None); t_cap.pop(m["obs"],None); gnow=m["pa"]
            if r is None: d=DROP
            elif r["q"]==0: d=DROP                         # 無効観測でfit→破棄
            elif r["base"]!=coef_id: d=DROP                # 基準係数ID不一致
            elif unconfirmed():                            # 後続観測未確定→保留
                hold=dict(base=r["base"],pa=r["pa"],w=r["w"]); d=HOLD; t_hold[0]=time.monotonic()
            elif abs(r["pa"]-gnow)<=TOL: coef_id+=1; d=ADOPT   # 採用
            else: d=RECALC                                 # 条件不一致→再計算
            if d!=HOLD: pend={}; t_hold[0]=None            # 終端判定でpendクリア(本ハーネス簡略)
            send(conn,DEC,obs=m["obs"],coef=coef_id,dec=d)
            if log:log(f"RDY obs={m['obs']}→{NAME[d]} coef={coef_id}")

# ======================= テスト駆動(injector) =======================
def iq_frame(n=256,g=0.28):
    if not HAVE_NP: return b"\x00"*(16*n)   # numpy無しでもバイト長を満たす(推定はスキップ)
    rng=np.random.default_rng(0)
    lv=np.arange(-7,8,2);pts=(lv[None,:]+1j*lv[:,None]).ravel();pts/=np.sqrt((np.abs(pts)**2).mean())
    sy=pts[rng.integers(0,64,n)];x=sy/np.sqrt((np.abs(sy)**2).mean())
    r=np.abs(g*x);y=(2*r/(1+r**2))*np.exp(1j*(np.angle(g*x)+1.4*r**2/(1+r**2)))/g
    a=np.empty(2*n,dtype="<f4");a[0::2]=x.real;a[1::2]=x.imag
    b=np.empty(2*n,dtype="<f4");b[0::2]=y.real;b[1::2]=y.imag
    return a.tobytes()+b.tobytes()

def run_tests(sock):
    IQ=iq_frame(); results=[]; cur=[0]
    def dec():
        m=recv_msg(sock); cur[0]=m["coef"]; return m["dec"]
    def chk(name,got,exp): results.append((name,got==exp,NAME.get(got),NAME.get(exp)))
    # T01 正常採用
    send(sock,CAPTURE,obs=1,coef=cur[0],pa=0.28,q=1,payload=IQ)
    send(sock,RDY,obs=1,pa=0.28); chk("T01 正常→採用",dec(),ADOPT)
    # T08 基準係数ID不一致→破棄
    send(sock,CAPTURE,obs=2,coef=0,pa=0.28,q=1,payload=IQ)
    send(sock,RDY,obs=2,pa=0.28); chk("T08 ID不一致→破棄",dec(),DROP)
    # T05 無効観測→破棄
    send(sock,CAPTURE,obs=3,coef=cur[0],pa=0.30,q=0,payload=IQ)
    send(sock,RDY,obs=3,pa=0.30); chk("T05 無効観測→破棄",dec(),DROP)
    # T02 計算中イベント, 結果到着時 後続未確定→保留
    send(sock,CAPTURE,obs=4,coef=cur[0],pa=0.30,q=1,payload=IQ)
    send(sock,PEND,obs=5,pa=0.30)
    send(sock,RDY,obs=4,pa=0.30); chk("T02 後続未確定→保留",dec(),HOLD)
    # T03 O1確定&条件一致→保留結果を採用
    send(sock,CONF,obs=5,pa=0.30); chk("T03 確定&一致→採用",dec(),ADOPT)
    # T04 条件不一致→再計算
    send(sock,CAPTURE,obs=6,coef=cur[0],pa=0.30,q=1,payload=IQ)
    send(sock,RDY,obs=6,pa=0.37); chk("T04 条件不一致→再計算",dec(),RECALC)
    # T06 計算中にE1,E2連続→イベント消失せず, 両確定後に採用
    send(sock,CAPTURE,obs=7,coef=cur[0],pa=0.37,q=1,payload=IQ)
    send(sock,PEND,obs=8,pa=0.37); send(sock,PEND,obs=9,pa=0.37)
    send(sock,RDY,obs=7,pa=0.37); chk("T06 複数pend→保留",dec(),HOLD)
    send(sock,CONF,obs=8,pa=0.37)                 # 1つ目確定(まだ未確定残→決定なし)
    send(sock,CONF,obs=9,pa=0.37); chk("T06 両確定→採用",dec(),ADOPT)
    # T07 確定が結果より先(順序競合)→1回の採用
    send(sock,CAPTURE,obs=10,coef=cur[0],pa=0.37,q=1,payload=IQ)
    send(sock,PEND,obs=11,pa=0.37); send(sock,CONF,obs=11,pa=0.37)   # 先に確定
    send(sock,RDY,obs=10,pa=0.37); chk("T07 順序競合→採用1回",dec(),ADOPT)
    # T09a 演算タイムアウト(RDYを送らない)→期限後に破棄・現係数維持
    send(sock,CAPTURE,obs=13,coef=cur[0],pa=0.37,q=1,payload=IQ)
    time.sleep(CALC_TIMEOUT+0.15); chk("T09a 演算タイムアウト→破棄",dec(),DROP)
    # T09b HOLD期限切れ(CONFを送らない)→期限後に再計算・現係数維持
    send(sock,CAPTURE,obs=14,coef=cur[0],pa=0.37,q=1,payload=IQ)
    send(sock,PEND,obs=15,pa=0.37)
    send(sock,RDY,obs=14,pa=0.37); chk("T09b-保留",dec(),HOLD)
    time.sleep(HOLD_DEADLINE+0.15); chk("T09b HOLD期限→再計算",dec(),RECALC)
    # T12 重複(古いbase)→二重適用なし
    send(sock,CAPTURE,obs=12,coef=cur[0],pa=0.37,q=1,payload=IQ); send(sock,RDY,obs=12,pa=0.37)
    chk("T12a 採用",dec(),ADOPT)
    send(sock,CAPTURE,obs=16,coef=0,pa=0.37,q=1,payload=IQ); send(sock,RDY,obs=16,pa=0.37)
    chk("T12b 二重適用なし",dec(),DROP)
    return results

SKIPS=[("T10 係数転送中イベント","PL転送・境界同期依存=未実装"),
       ("T11 切替後の悪化→ROLLBACK","PL VERIFY/バンク切替依存=未実装")]

def print_results(res, title):
    print(title); print("-"*60); ok=0
    for name,good,got,exp in res:
        print(f"  {'PASS' if good else 'FAIL':4} {name:24} got={got} exp={exp}"); ok+=good
    for name,why in SKIPS:
        print(f"  SKIP {name:24} ({why})")
    print("-"*60); print(f"  {ok}/{len(res)} PASS, {len(SKIPS)} SKIP(PL未検証)")
    return ok,len(res)

def main():
    args=sys.argv[1:]
    if args and args[0]=="daemon":
        port=int(args[1]) if len(args)>1 else 5560
        srv=socket.socket(); srv.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
        srv.bind(("0.0.0.0",port)); srv.listen(1)
        print(f"[daemon] listening 0.0.0.0:{port}  numpy={HAVE_NP}")
        while True:
            conn,addr=srv.accept(); print(f"[daemon] connect {addr}")
            try: daemon_handle(conn, log=lambda s:print("   ",s))
            except Exception as e: print("[daemon] err",e)
            finally: conn.close(); print("[daemon] closed")
    elif args and args[0]=="inject":
        host,port=args[1],int(args[2]); s=socket.create_connection((host,port))
        res=run_tests(s); print_results(res, f"フェーズ1 T01〜T12 (Ethernet {host}:{port})"); s.close()
    elif args and args[0]=="ping":
        host,port=args[1],int(args[2]); s=socket.create_connection((host,port),timeout=5)
        send(s,CAPTURE,obs=1,coef=0,pa=0.28,q=1,payload=iq_frame()); send(s,RDY,obs=1,pa=0.28)
        m=recv_msg(s); print("疎通OK" if m and m["dec"]==ADOPT else f"応答異常 {m}"); s.close()
    else:
        import threading
        srv=socket.socket(); srv.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
        srv.bind(("127.0.0.1",0)); port=srv.getsockname()[1]; srv.listen(1)
        threading.Thread(target=lambda:daemon_handle(srv.accept()[0]),daemon=True).start()
        s=socket.create_connection(("127.0.0.1",port)); res=run_tests(s)
        print_results(res, f"フェーズ1 自己テスト(ループバックTCP, 方式C, numpy={HAVE_NP})"); s.close()
if __name__=="__main__": main()
