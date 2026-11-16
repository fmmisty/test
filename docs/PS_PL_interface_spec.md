# PS↔PL インターフェース仕様（DPD 係数自動更新）

対象：Zynq-7000（PS=ARM Linux＋Julia常駐、PL=FPGA）。`sim/dpd_auto_update.jl` と整合。
位置づけ：PL設計とPSソフトの**取り決め（契約）**。本書はRTLでも性能保証でもない。値は設計例で、実機で確定する。
最終更新：2026-11-16

---

## 0. 役割分担（再掲）
| 側 | 担当 |
|---|---|
| **PL (FPGA)** | DPDデータパス、歪評価、歪変化検出、CAPTURE（観測I/Q確定・品質判定）、係数バンク（現用/待機）、境界同期切替 |
| **PS (ARM Linux + Julia)** | 係数推定（MP/ILA, θ=Φ\y）、採否制御（採用/保留/破棄/再計算）、タグ管理 |

---

## 1. 制御レジスタマップ（AXI-Lite, 32bit語, `REG_BASE` 起点）
`sim/dpd_auto_update.jl` の `R_*` と一致。語offset＝バイトoffset/4。

| off | 名称 | 方向 | 用途・ビット定義 |
|---:|---|---|---|
| 0 | STATUS | PL→PS | bit0=CAPTURE_READY, bit1=SWAP_BOUNDARY, bit2=OBS_INVALID, 上位=FSM状態 |
| 1 | EVENT | PS→PL | bit0=再CAPTURE要求(RECALC), bit1=強制定期要求 |
| 2 | OBS_ID | PL→PS | 観測世代ID（CAPTUREごと++、単調増加） |
| 3 | REQ_ID | PS→PL | 係数計算要求ID（PSが採番） |
| 4 | COEF_ID | 双方向 | **現用係数ID**。PL=切替時に更新/PS=採用時に書く。基準(base)照合に使用 |
| 5 | PA_STATE | PL→PS | 動作状態量（float32をビット列で格納。例：正規化した温度/平均出力の合成指標） |
| 6 | QUALITY | PL→PS | 0=無効（過渡/飽和/同期喪失）、≠0=有効。採否判定の前提 |
| 7 | NSAMP | PL→PS | 本窓の総サンプル数（x と y の合計。前半x/後半y） |
| 8 | SWAP | PS→PL | 1=待機バンク有効化要求（境界で原子的切替） |
| 9 | BANK_DONE | PL→PS | 1=切替完了（SWAP応答） |
| 10 | RESULT_ID | PS→PL | 適用した結果の識別（ログ・診断） |
| 11 | FAULT | PL→PS | 異常コード（CRC不一致/転送失敗/タイムアウト等） |
| 12 | HOLD_DEADLINE | PS→PL | 保留の期限（tick/クロック）。超過で安全側（現係数維持） |
| 13 | RETRY_MAX | PS→PL | 再計算上限回数（livelock防止） |

> 仕様：PL→PS のフラグは**レベル保持**（パルス非依存）。PS が STATUS の該当ビットを**書き戻してクリア**（一度だけ受理）。CDCは非同期FIFO/2段同期。

---

## 2. 観測I/Q バッファ（DDR / CMA, `IQ_BASE` 起点）
- 形式：インタリーブ `float32` `[I0,Q0,I1,Q1,...]`。
- 配置：**前半＝送信基準 x（整合後）、後半＝帰還 y（時間・位相・利得整合後）**。各 `NSAMP/2` サンプル。
- PL が CAPTURE 完了時に書き込み、`STATUS.CAPTURE_READY=1`。PS は mmap で読み出し。
- DMA/CMA領域は非キャッシュ or 読取前にキャッシュ無効化（整合に注意）。
- 1窓 ≤ `IQ_NMAX`（例 8192）。評価窓長は実験で決定。

---

## 3. ハンドシェイク手順（1更新サイクル）
```
PL: CAPTURE → x,y を DDR へ, OBS_ID/COEF_ID(=base)/PA_STATE/QUALITY/NSAMP 更新 → STATUS.CAPTURE_READY=1
PS: READY検出 → I/Q と meta 読込 → STATUS.CAPTURE_READY をクリア(一度だけ受理)
PS: QUALITY==0 なら破棄/再観測(採否に進まない)
PS: θ=Φ\y で係数推定 → 採否制御(方式C)
      ├ base≠現COEF_ID → DROP
      ├ 後続観測未確定   → HOLD(現係数維持, HOLD_DEADLINE監視)
      ├ 条件一致        → 待機バンク書込 → SWAP=1
      └ 条件不一致      → EVENT.bit0=1 (PLへ再CAPTURE要求, RETRY_MAX以内)
PL: SWAP=1 かつ frame境界 → 待機バンクを原子的に有効化, 旧バンク保持, BANK_DONE=1, COEF_ID++
PS: BANK_DONE検出 → SWAP=0, RESULT_ID記録
PL: VERIFY窓で更新前後の歪比較 → 悪化なら ROLLBACK(旧バンク再有効化, FAULT記録)
```
- **single outstanding**：係数計算要求は常に1件のみ未処理（REQ_ID/RESULT_ID で対応確認）。結果は WAIT 相当でのみ受理。
- **同時到来**（CAPTURE_READY と結果が同クロック）：両フラグ登録後に1回判定（§patent[0085]）。

---

## 4. 世代・ID 管理（用語を一義化）
| 概念 | 採番 | 固定する時点 | 用途 |
|---|---|---|---|
| OBS_ID | PL | **観測窓キャプチャ時** | 結果と元観測の紐付け |
| REQ_ID | PS | 計算要求発行時 | 要求と結果の対応 |
| COEF_ID (base_coef_id) | 切替時PL／要求時PS記録 | **fitに使った観測窓のキャプチャ時に動作していた係数セット** | 基準係数照合（混用防止） |

- 比較は**等値（タグ一致）中心**。順序比較が要る箇所は32bitラップ安全な modular 比較。
- 「世代が古い」理由だけで破棄しない（等値＋動作状態差＋歪指標で判定, §patent[0013]）。

---

## 5. 未確定・安全側の規則（前回レビュー反映）
- **HOLD_DEADLINE 超過** → DROP＋RECALC（現係数維持）。無限保留禁止。
- **RETRY_MAX 超過** → 再計算打切り、現係数維持、FAULT記録（livelock防止）。
- **pendキュー深さ**（後続イベント保持数）を定義し、溢れは黙って捨てない（最古 or 最新を規則で選ぶ）。
- **QUALITY==0 の観測で fit しない**（過渡/飽和での誤学習を防ぐ）。
- **VERIFY は同条件ゲート**：切替直後の過渡と通常窓を無条件比較しない。

---

## 5.1 計算中イベント・切替の詳細FSM（T10／T11 対応, 設計案）

phase1_harness.py で SKIP とした **T10（係数転送中にイベント）／T11（切替後の悪化→ROLLBACK）** は、PL側の転送・境界同期・VERIFY に依存する。以下にその制御詳細を定める（RTL未実装・数値未確定）。

### 5.1.1 STAGE（待機バンク転送）中にイベントが発生＝T10
転送は原子的でない期間を含むため、**転送中の係数は決して有効化しない**。

| 現状態 | 入力（優先順） | 次状態／動作 |
|---|---|---|
| STAGE | `distortion_event`（転送中に新イベント） | STAGE 継続（転送は中断しない）。`stage_event=1` を記録 |
| STAGE | `bank_write_done` ＆ `integrity_ok` ＆ `stage_event==0` | VERIFY：`swap_boundary` で原子的に切替 |
| STAGE | `bank_write_done` ＆ `integrity_ok` ＆ `stage_event==1` | **RECONCILE へ戻す**：切替境界の直前に `condition_ok` を再評価。OKなら VERIFY、NGなら DROP（現係数維持）＋必要なら RECALC |
| STAGE | `integrity_fail` / `crc_fail` / 転送タイムアウト | DROP：待機バンクを無効化、現係数維持、FAULT記録 |

規則：
- **切替は必ず frame 境界で原子的**（半完成バンクを使わない）。`bank_write_done & integrity_ok(CRC/範囲/量子化) & swap_boundary` が全て立つまで切替禁止。
- 転送中イベントがあった場合、**切替の直前に condition を再照合**（イベントで適用条件が無効化されていないか）。無効なら切替を取りやめ（＝T10 の要件「中途半端な係数を使わない」）。

### 5.1.2 切替後の VERIFY と ROLLBACK＝T11
切替後、更新前後の歪指標を**同一条件**で比較し、悪化時は旧係数へ戻す。

| 現状態 | 条件 | 次状態／動作 |
|---|---|---|
| VERIFY | 同条件の比較窓が成立（`cond_stable`）＆ 歪指標が許容内／改善 | COMMIT：新係数を継続、`t_hold`/旧バンク保持を解除 |
| VERIFY | 同条件比較で **有意に悪化**（`D_after − D_before > ΔD_fail`） | ROLLBACK：旧バンク再有効化、FAULT記録、原因（係数ID等）保持 |
| VERIFY | 比較窓の途中で**新イベント**（条件が動いた） | 比較窓を**破棄して再取得**（過渡と通常を混ぜない）。期限 `VERIFY_DEADLINE` 超過は安全側で ROLLBACK |
| VERIFY | 評価不能のまま `VERIFY_DEADLINE` 超過 | ROLLBACK（安全側）、FAULT記録 |

規則：
- **同条件ゲート**：切替直後の過渡を除外し、送信条件・PA_STATE が切替前後で `|ΔPA_STATE| ≤ TOL_STATE` の窓のみ比較に用いる。
- **旧係数は VERIFY 完了まで保持**（ROLLBACK 可能）。COMMIT 後に解放。
- ROLLBACK は**原子的**（frame 境界）で、COMMIT と同じ切替機構を逆向きに使う。

### 5.1.3 追加レジスタ（本節で参照）
| 名称 | 幅 | 用途 |
|---|---|---|
| STAGE_EVENT | 1 | 転送中にイベント発生（T10 の再照合トリガ） |
| VERIFY_DEADLINE | 32 | VERIFY の期限（tick/ms）。超過で安全側 ROLLBACK |
| DELTA_D_FAIL | 32 | 悪化判定しきい値 ΔD_fail（歪指標差） |
| COND_STABLE | 1 | 同条件比較窓が成立（PLが判定） |

### 5.1.4 検証（実装後に実行）
- **T10**：STAGE 中に `distortion_event` を注入 → 中途半端な係数が適用されないこと、切替直前の再照合で採用/取りやめが分岐すること。
- **T11**：切替後に歪を悪化させる係数を与え → 同条件比較で ROLLBACK、評価不能では期限後に安全側遷移すること。

> 本節は設計案（FSM・レジスタ・しきい値は未確定）。RTL は別途実装し、テストベンチで T10／T11 を再現する。既存RTL正本は変更しない。

---

## 6. 実機で確定する数値（未定）
評価窓長 / QUALITY・PA_STATE の量子化幅 / TOL_STATE（状態一致許容）/ HOLD_DEADLINE / RETRY_MAX / pend深さ / 係数の固定小数点語長・CRC方式 / frame境界の定義 / 観測帯域とADC / **VERIFY_DEADLINE** / **ΔD_fail（悪化判定しきい値）** / 同条件判定（COND_STABLE）の条件。

---

## 7. 対応ファイル
- PS常駐：`sim/dpd_auto_update.jl`（本仕様の `R_*`・バッファ配置と一致）
- 効果検証：`sim/dpd_update_policy_sim.py`（方式A/B/Cの採否制御）
- 係数推定参照：`sim/dpd_model_from_data.jl`, `sim/dpd_backoff_from_data.jl`
