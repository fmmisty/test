#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
A/B/C 係数更新方式の ACLR/EVM/NMSE 評価（シミュレーション）
=====================================================================
★全数値はシミュレーション（挙動＋信号モデル）。実測値は未取得（実機で別途測定）。
  原本明細書・既存RTLは変更しない。

共通条件（3方式で同一）:
  - 信号: 64QAM 単一キャリア, RRC α=0.2
  - PA: Wiener型（線形メモリFIR → Saleh静的非線形）, 状態 g が動作条件(圧縮/温度)を表す
  - メモリー効果: 線形FIRタップで付与
  - 状態変化: イベントで g が階段状に変化（過渡CAP tick は観測無効）
  - 演算遅延: L tick
方式:
  A 定期更新のみ         : 周期 T で更新（有効性は見ない）
  B イベント無条件更新    : 各イベントで即更新（過渡=未整定の観測でfit→誤り）
  C 保留・再判定(本発明)  : 有効観測の確定を待って更新、保留中は前の良い係数を維持、同条件は統合
指標: ACLR[dBc], EVM[%], NMSE[dB], 誤適用件数, 更新遅延[tick]（時間平均/件数）
出力: 結果表(print) ＋ スペクトル図 docs/figures/abc_aclr_spectrum.png
"""
import numpy as np, os
import matplotlib; matplotlib.use("Agg"); import matplotlib.pyplot as plt
from matplotlib import font_manager as fm
for _p in ["/usr/share/fonts/opentype/ipafont-gothic/ipag.ttf","/usr/share/fonts/truetype/fonts-japanese-gothic.ttf"]:
    if os.path.exists(_p):
        fm.fontManager.addfont(_p); plt.rcParams["font.family"]=fm.FontProperties(fname=_p).get_name(); break
plt.rcParams["axes.unicode_minus"]=False
rng = np.random.default_rng(2026)

# ---- 信号 ----
RS=1.0; ALPHA=0.2; OSR=8; FS=RS*OSR
NSYM=8000
def rrc(b,sps,span=12):
    N=span*sps;t=np.arange(-N/2,N/2+1)/sps;h=np.zeros_like(t)
    for i,ti in enumerate(t):
        if abs(ti)<1e-8:h[i]=1-b+4*b/np.pi
        elif b>0 and abs(abs(ti)-1/(4*b))<1e-6:h[i]=b/np.sqrt(2)*((1+2/np.pi)*np.sin(np.pi/(4*b))+(1-2/np.pi)*np.cos(np.pi/(4*b)))
        else:h[i]=(np.sin(np.pi*ti*(1-b))+4*b*ti*np.cos(np.pi*ti*(1+b)))/(np.pi*ti*(1-(4*b*ti)**2))
    return h/np.sqrt(np.sum(h**2))
H_RRC=rrc(ALPHA,OSR)
lv=np.arange(-7,8,2);CONST=(lv[None,:]+1j*lv[:,None]).ravel();CONST/=np.sqrt((np.abs(CONST)**2).mean())
SYM=CONST[rng.integers(0,64,NSYM)]
up=np.zeros(NSYM*OSR,complex); up[::OSR]=SYM
X=np.convolve(up,H_RRC,mode='same'); X/=np.sqrt((np.abs(X)**2).mean())

# ---- PA: Wiener(メモリFIR)→Saleh(状態g) ----
H_MEM=np.array([1.0, 0.12-0.05j, 0.05+0.03j])        # 線形メモリ(メモリー効果)
def PA(u,g):
    v=np.convolve(u,H_MEM,mode='same')
    r=np.abs(g*v); A=2.0*r/(1+r**2); P=1.4*r**2/(1+r**2)
    return A*np.exp(1j*(np.angle(g*v)+P))/g

# ---- MP基底 / DPD(ILA) ----
def mp(x,K=5,M=3):
    ax=np.abs(x);c=[]
    for m in range(M+1):
        xm=np.roll(x,m);xm[:m]=0;axm=np.roll(ax,m);axm[:m]=0
        for k in range(1,K+1,2):c.append(xm*axm**(k-1))
    return np.column_stack(c)
_wcache={}
def dpd_w(g):
    key=round(g,3)
    if key in _wcache: return _wcache[key]
    y=PA(X,g); G=np.vdot(X,y)/np.vdot(X,X); z=y/G
    P=mp(z);A=P.conj().T@P;A+=1e-3*np.trace(A)/A.shape[0]*np.eye(A.shape[0])
    w=np.linalg.solve(A,P.conj().T@X); _wcache[key]=w; return w

# ---- 指標: ACLR / EVM / NMSE ----
def psd(y,nfft=4096):
    N=(len(y)//nfft)*nfft; seg=y[:N].reshape(-1,nfft); w=np.hanning(nfft)
    S=np.mean(np.abs(np.fft.fftshift(np.fft.fft(seg*w,axis=1),axes=1))**2,axis=0)
    f=np.fft.fftshift(np.fft.fftfreq(nfft,1/FS)); return f,S
OCC=RS*(1+ALPHA)
def aclr_db(f,S):
    ib=(f>-OCC/2)&(f<OCC/2)
    up=(f>OCC/2)&(f<OCC/2+OCC); lo=(f<-OCC/2)&(f>-OCC/2-OCC)
    pin=S[ib].sum(); return 10*np.log10(max(S[up].sum(),S[lo].sum())/pin+1e-30)
def evm_pct(y):
    mf=np.convolve(y,H_RRC,mode='same')
    rs=mf[::OSR][:NSYM]
    n=min(len(rs),NSYM); rs=rs[:n]; ref=SYM[:n]
    gc=np.vdot(rs,ref)/np.vdot(rs,rs)        # 複素利得整合(最小二乗)
    e=ref-gc*rs; return 100*np.sqrt(np.mean(np.abs(e)**2)/np.mean(np.abs(ref)**2))
def nmse_db(y):
    G=np.vdot(X,y)/np.vdot(X,X); return 10*np.log10(np.sum(np.abs(y-G*X)**2)/np.sum(np.abs(G*X)**2))
_mcache={}
def measure(g_applied,g_now,dpd=True):
    key=(round(g_applied,3),round(g_now,3),dpd)
    if key in _mcache: return _mcache[key]
    if dpd:
        w=dpd_w(g_applied); u=mp(X)@w; u*=np.sqrt(np.mean(np.abs(X)**2)/np.mean(np.abs(u)**2))
    else:
        u=X.copy()
    y=PA(u,g_now); f,S=psd(y)
    r=dict(aclr=aclr_db(f,S), evm=evm_pct(y), nmse=nmse_db(y), f=f, S=S)
    _mcache[key]=r; return r

# ---- 状態変化シナリオ(3方式共通) ----
TICKS=600; L=12; CAP=4; TOL=0.02; T_PERIOD=25
def scenario():
    g=0.26; cur=np.full(TICKS,g); evs=[]; prev=g; t=0; gprev=np.full(TICKS,g)
    while True:
        t+=int(rng.integers(14,42))
        if t>=TICKS-CAP-L-2: break
        step=rng.choice([-0.012,0.012,0.015,-0.015]) if rng.random()<0.5 else rng.choice([-0.05,0.05,0.07,-0.07])
        ng=float(np.clip(g+step,0.20,0.40)); gprev[t:]=g; cur[t:]=ng; g=ng; evs.append(t)
    return cur,gprev,evs
G,GPREV,EVENTS=scenario(); EVSET=set(EVENTS)
def last_event(t):
    le=-10**9
    for e in EVENTS:
        if e<=t: le=e
        else: break
    return le
def obs_state(t):                      # 観測される状態: 整定後=現状態, 過渡=中間値(未整定→誤差)
    le=last_event(t)
    if t-le>=CAP: return G[t]
    return 0.5*(GPREV[t]+G[t])

def run_policy(mode):
    applied=0.26; recompute=0; applies=[]   # (apply_tick, value)
    sched=[]                                 # (apply_tick, value, cause_event_tick)
    last_periodic=-10**9; hold_prev=None
    # スケジューリング
    for t in range(TICKS):
        if mode=='A':
            if t-last_periodic>=T_PERIOD:
                last_periodic=t; recompute+=1; sched.append((t+L, obs_state(t), t))
        elif mode=='B':
            if t in EVSET:
                recompute+=1; sched.append((t+L, obs_state(t), t))
        else:  # C: 整定後に取得→適用。保留中は前係数維持。同条件は統合
            if t in EVSET:
                ct=t+CAP
                if ct<TICKS:
                    # 直近スケジュールと状態が近ければ統合(再計算しない)
                    if sched and abs(G[ct]-sched[-1][1])<=TOL:
                        pass
                    else:
                        recompute+=1; sched.append((ct+L, G[ct], t))
    # 適用の時系列
    g_applied=np.full(TICKS,0.26)
    sched.sort()
    si=0; cur_app=0.26
    for t in range(TICKS):
        while si<len(sched) and sched[si][0]<=t:
            cur_app=sched[si][1]; applies.append((t,cur_app)); si+=1
        g_applied[t]=cur_app
    # 誤適用・遅延
    misapply=0
    for at,val in applies:
        if abs(val-G[at])>TOL: misapply+=1
    lat=[]
    for e in EVENTS:
        tgt=G[min(e, TICKS-1)]
        found=None
        for t in range(e,TICKS):
            if abs(g_applied[t]-tgt)<=TOL: found=t-e; break
        if found is not None: lat.append(found)
    # 品質(時間平均): ACLR/EVM/NMSE
    ac=[];ev=[];nm=[]
    for t in range(40,TICKS,3):
        m=measure(g_applied[t],G[t]); ac.append(m['aclr']);ev.append(m['evm']);nm.append(m['nmse'])
    ac=np.array(ac);ev=np.array(ev);nm=np.array(nm)
    return dict(recompute=recompute,misapply=misapply,
                lat=(np.mean(lat) if lat else float('nan')),
                aclr=ac.mean(),aclr_w=ac.max(),evm=ev.mean(),evm_w=ev.max(),
                nmse=nm.mean(),nmse_w=nm.max(),g_applied=g_applied)

print("="*70)
print("A/B/C ACLR/EVM/NMSE 評価  ※全てシミュレーション値（実測は未取得）")
print(f"64QAM α={ALPHA}, Wienerメモリ+Saleh, イベント{len(EVENTS)}回, L={L}, CAP={CAP}")
print("="*70)
res={}
for m in ('A','B','C'): res[m]=run_policy(m)
# 無DPD基準(現状態に対しDPD無し)の時間平均
base_ac=np.mean([measure(0,G[t],dpd=False)['aclr'] for t in range(40,TICKS,12)])
base_ev=np.mean([measure(0,G[t],dpd=False)['evm'] for t in range(40,TICKS,12)])
nm_name={'A':'A 定期のみ','B':'B イベント無条件','C':'C 保留・再判定(本案)'}
hdr=f"{'方式':<20}{'ACLR平均':>9}{'ACLR最悪':>9}{'EVM平均':>8}{'EVM最悪':>8}{'NMSE':>8}{'誤適用':>7}{'遅延':>7}{'再演算':>7}"
print(hdr); print("-"*len(hdr.encode('ascii','ignore'))+ "-"*20)
print(f"{'(参考)無DPD':<18}{base_ac:>9.1f}{'—':>9}{base_ev:>8.1f}{'—':>8}{'—':>8}{'—':>7}{'—':>7}{'—':>7}")
for m in ('A','B','C'):
    r=res[m]
    print(f"{nm_name[m]:<18}{r['aclr']:>9.1f}{r['aclr_w']:>9.1f}{r['evm']:>8.2f}{r['evm_w']:>8.2f}{r['nmse']:>8.1f}{r['misapply']:>7}{r['lat']:>7.1f}{r['recompute']:>7}")
print("-"*80)
print("※全てシミュレーション値[dBc/%/dB/件/tick]。ACLR/EVM/NMSE/誤適用/遅延は小さいほど良い。")
print("  平均=時間平均、最悪=期間中の最大(劣化のピーク)。Cは誤適用0・遅延最小・再演算最小で、品質はA/Bと同等。")

# ---- スペクトル図: イベント直後のストレス点で各方式の適用係数を比較 ----
# A=まだ旧状態(stale), B=過渡でfit(誤), C=整定後に正しく適用 となる tick を選ぶ
snap=None
for e in EVENTS:
    t=e+CAP+L+1
    if t>=TICKS: continue
    gnow=G[t]
    okC=abs(res['C']['g_applied'][t]-gnow)<=TOL
    badA=abs(res['A']['g_applied'][t]-gnow)>TOL
    badB=abs(res['B']['g_applied'][t]-gnow)>TOL
    if okC and badA and badB: snap=t; break
if snap is None: snap=EVENTS[len(EVENTS)//2]+CAP+L+1
gnow=G[snap]
plt.figure(figsize=(9,4.6))
def plot_psd(gap,label,color,dpd=True):
    m=measure(gap,gnow,dpd=dpd); f,S=m['f'],m['S']
    p=10*np.log10(S/S.max()+1e-20); plt.plot(f,p,color=color,lw=1.1,
        label=f"{label}  ACLR={aclr_db(f,S):.1f}dBc, EVM={m['evm']:.1f}%")
plot_psd(0,"無DPD","#8a9998",dpd=False)
plot_psd(res['A']['g_applied'][snap],"A 定期のみ","#a5701f")
plot_psd(res['B']['g_applied'][snap],"B 無条件","#b03a3a")
plot_psd(res['C']['g_applied'][snap],"C 本案","#0e7c86")
# 隣接チャネル帯の目印
for s in (1,-1):
    plt.axvspan(s*OCC/2, s*(OCC/2+OCC), color="#8250df", alpha=0.05)
plt.axvline(OCC/2,ls=":",color="#8250df",lw=0.8); plt.axvline(-OCC/2,ls=":",color="#8250df",lw=0.8)
plt.xlim(-3,3); plt.ylim(-70,3)
plt.xlabel("正規化周波数 (f/Rs)"); plt.ylabel("PSD [dBc]")
plt.title(f"図: イベント直後(状態不一致点)のスペクトル比較 ※シミュレーション\n紫帯=隣接チャネル(ACLR測定帯)")
plt.legend(fontsize=8,loc="upper right"); plt.grid(alpha=0.3)
import os; os.makedirs("docs/figures",exist_ok=True)
plt.tight_layout(); plt.savefig("docs/figures/abc_aclr_spectrum.png",dpi=130)
print(f"\nスペクトル図: docs/figures/abc_aclr_spectrum.png (snapshot tick={snap}, g_now={gnow:.3f})")
print(f"  適用係数: A={res['A']['g_applied'][snap]:.3f}  B={res['B']['g_applied'][snap]:.3f}  C={res['C']['g_applied'][snap]:.3f}  (現状態={gnow:.3f})")
