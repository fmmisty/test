#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
係数更新ポリシー A/B/C の制御レベル比較（特許第12稿 §37.2 対応）
=====================================================================
注意: これは「制御ロジック」の挙動モデル。ACLR/EVM の絶対値は実機測定で設定する
      (明細書どおり)。ここで示すのは A/B/C の相対差:
        ・再演算回数     ・誤適用件数(=条件不一致 or 無効観測で適用)
        ・不整合tick(設計状態と現状態がズレて過ごした時間=マスク割れの目安)
        ・応答遅延(イベント→正しい係数適用まで)

モデルの要点(特許の争点を再現):
  - PA状態 g はイベントで変化。イベント直後 CAP tick は過渡で観測が無効
    (この間に取得した観測でfitした係数は誤り)。
  - 係数計算は L tick 後に結果が返る。計算中に後続イベントが起こり得る。
  - A: 定期要求のみ(取得観測の有効性を見ない/無条件採用)
  - B: 毎イベントで即再計算(過渡の無効観測でfit)・無条件採用
  - C: 後続観測が有効確定するまで係数計算/採用を保留、同一結果を再判定、
       状態が同じなら要求統合(本発明案)
"""
import numpy as np
rng=np.random.default_rng(1116)

TICKS=600; L=12; CAP=4; TOL=0.02; T_PERIOD=25
def scenario():
    g=0.26; cur=np.full(TICKS,g); evs=[]; t=0
    while True:
        t+=int(rng.integers(12,45))
        if t>=TICKS-CAP-L-2: break
        # 半分は小変化(統合できる), 半分は大変化
        if rng.random()<0.5: step=rng.choice([-0.012,0.012,0.015,-0.015])  # 小(統合候補)
        else:                 step=rng.choice([-0.05,0.05,0.07,-0.07])      # 大
        g=float(np.clip(g+step,0.20,0.40)); cur[t:]=g; evs.append(t)
    return cur, evs
G,EVENTS=scenario(); EVSET=set(EVENTS)
def last_event_before(t):
    le=-10**9
    for e in EVENTS:
        if e<=t: le=e
        else: break
    return le
def obs_valid(t):                 # イベント直後 CAP tick は過渡=無効
    return (t-last_event_before(t))>=CAP

def run(policy):
    g_design=0.26; coef_id=0
    recompute=misapply=0
    inflight=None   # (arrive, g_obs, obs_valid_at_capture, base_coef_id, obs_tick)
    hold=None       # C: 保留結果 (g_obs, base_coef_id, from_obs_tick)
    pend=None       # C: 後続イベントの観測 (obs_tick, confirm_tick)
    last_periodic=-10**9
    resp=[]         # イベント→正しい係数反映 の遅延
    pending_event_t=None
    def start(tick):
        nonlocal recompute
        recompute+=1
        gt=G[tick]
        return dict(arrive=tick+L, g_obs=gt, valid=obs_valid(tick),
                    base=coef_id, otick=tick)
    for t in range(TICKS):
        gnow=G[t]; ev=(t in EVSET)
        if ev: pending_event_t=t
        # ---- 要求発行 ----
        if policy=='A':
            if t-last_periodic>=T_PERIOD and inflight is None:
                last_periodic=t; inflight=start(t)       # 有効性を見ない
        elif policy=='B':
            if ev:
                inflight=start(t)                         # 毎イベント即時・過渡でもfit(置換)
        else: # C
            if ev:
                # 進行中計算と状態が近ければ統合(新規計算しない)
                if inflight is not None and abs(G[t]-inflight['g_obs'])<=TOL and inflight['base']==coef_id:
                    pass                                  # 要求統合
                else:
                    pend=dict(otick=t, confirm=t+CAP)     # 後続観測を確定待ちに
        # ---- Cの後続観測が有効確定したら計算開始 ----
        if policy=='C' and pend and t>=pend['confirm']:
            if inflight is None and obs_valid(t):
                inflight=start(pend['otick'] if obs_valid(pend['otick']) else t)
            pend=None
        # ---- 結果到着 ----
        if inflight and t>=inflight['arrive']:
            r=inflight; inflight=None
            if policy in ('A','B'):
                if (not r['valid']) or abs(r['g_obs']-gnow)>TOL: misapply+=1
                g_design=r['g_obs']; coef_id+=1
                if pending_event_t is not None and abs(g_design-gnow)<=TOL:
                    resp.append(t-pending_event_t); pending_event_t=None
            else: # C
                if r['base']!=coef_id:
                    pass                                   # 破棄
                elif not obs_valid(t) or (pend and t<pend['confirm']):
                    hold=dict(g_obs=r['g_obs'],base=r['base'])  # 保留
                elif abs(r['g_obs']-gnow)<=TOL and r['valid']:
                    g_design=r['g_obs']; coef_id+=1        # 採用
                    if pending_event_t is not None: resp.append(t-pending_event_t); pending_event_t=None
                else:
                    inflight=start(t)                      # 条件不一致→再計算(有効観測で)
        # ---- C: 保留結果の再判定 ----
        if policy=='C' and hold is not None and obs_valid(t):
            if hold['base']==coef_id and abs(hold['g_obs']-gnow)<=TOL:
                g_design=hold['g_obs']; coef_id+=1; hold=None
                if pending_event_t is not None: resp.append(t-pending_event_t); pending_event_t=None
            else:
                hold=None
                if inflight is None: inflight=start(t)     # 破棄→再計算
        # 記録
        if t==0: over=0
        over = over + (1 if abs(g_design-gnow)>TOL else 0)
    return dict(recompute=recompute, misapply=misapply, over=over,
                resp=(np.mean(resp) if resp else float('nan')))

print(f"シナリオ: {TICKS}tick, イベント{len(EVENTS)}回(小変化≈統合候補/大変化 混在), 演算遅延L={L}, 過渡CAP={CAP}, 許容TOL={TOL}")
print(f"{'方式':<30}{'再演算':>8}{'誤適用':>8}{'不整合tick':>12}{'平均応答':>10}")
nm={'A':'A 定期のみ(有効性無視)','B':'B 毎イベント無条件採用','C':'C 保留+再判定+統合(本案)'}
for p in ('A','B','C'):
    r=run(p)
    print(f"{nm[p]:<28}{r['recompute']:>8}{r['misapply']:>8}{r['over']:>12}{r['resp']:>10.1f}")
print("\n解釈: Bは毎イベント再計算で回数最多かつ過渡観測で誤適用が出る。Aは応答が遅く不整合tickが増える。")
print("      Cは要求統合で再演算を抑え、無効観測を保留して誤適用0を狙う。ACLR/EVMは実機で測定(明細書どおり)。")
