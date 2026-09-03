# 参考資料集 — 6.5 GHz(M バンド) GaN HEMT アンプ / 64QAM 歪み補償(DPD)

本ファイルは、設計調査レポート（`6p5GHz_HEMT_PA_歪み補償_設計調査レポート.md`）の裏付けとなる **論文・GitHub 実装・放送系公的資料・デバイス資料**を集約したもの。ご依頼「ラジオ／テレビ朝日など・論文・GitHub・海外論文・情報集め」に対応。

> ⚠ 一次資料での裏取り前提。数値は各出典の代表値。`sedi.co.jp` / `cdiweb.com` / `everythingrf.com` 等は本作業環境のネットワークから直接取得がブロックされたため、リンクのみ掲載（社内網・メーカー窓口で入手）。

最終更新: 2026-09-03

---

## 1. GitHub / オープンソース実装（すぐ試せる）

DPD の挙動モデルと適応アルゴリズムを自前で回して EVM/ACLR を評価するのに有用。**本件（周波数アジャイル M バンド）では、これらをベースに「周波数別の係数セット」や「都度再適応」を足す**のが現実的な出発点。

| リポジトリ | 言語 | 内容 | 本件での使い所 |
|---|---|---|---|
| **lab-emi/OpenDPD** — https://github.com/lab-emi/OpenDPD | PyTorch | PA モデリング＋NN-DPD の end-to-end 学習・ベンチマーク基盤。**公開 I/Q データセット付き** | ニューラルネット DPD の評価基盤。実測データが無い段階の検討に最適 |
| **OpenDPDv2**（arXiv 2507.06849） — https://arxiv.org/abs/2507.06849 | PyTorch | 量子化＋動的時間スパース性で推論エネルギー 4.5x 削減、ACPR −51.8 dBc / EVM −35.2 dB | 実装時の省電力化・量子化の指針 |
| **ctarver/ILA-DPD** — https://github.com/ctarver/ILA-DPD | MATLAB | 間接学習(ILA)による **GMP DPD**。定番・可読性高 | MP/GMP＋ILA の実装リファレンス（本件の第一候補モデル） |
| **michael-koller-91/generalized_memory_polynomial** — https://github.com/michael-koller-91/generalized_memory_polynomial | Python | 交差項あり/なしの一般化メモリ多項式 | GMP の係数構造・同定の学習用 |
| **isaacmacario2/DPD** — https://github.com/isaacmacario2/DPD | Python | ILA モデルの DPD 実装 | Python で ILA を回す最小例 |
| **abdalrahimnaser/neural_net_digital_predistortion** — https://github.com/abdalrahimnaser/neural_net_digital_predistortion | Python | ニューラルネット DPD | NN-DPD の入門実装 |
| GitHub Topics: `dpd`（Python, star 順） — https://github.com/topics/dpd?l=python&o=asc&s=stars | — | DPD 関連リポジトリ一覧 | 他実装の横断調査 |
| （商用参考）MATLAB `comm.DPD` — https://www.mathworks.com/help/comm/ref/comm.dpd-system-object.html | MATLAB | メモリ多項式 DPD の System object | 手軽な検証・比較用 |

---

## 2. 海外論文（英語・基礎〜応用）

### 2.1 DPD のモデル（基礎・必読3本）
> DPD でメモリー効果を補正するなら、まずこの 3 本。特に ①GMP が実装の基準。

- **① D. R. Morgan ほか, "A Generalized Memory Polynomial Model for Digital Predistortion of RF Power Amplifiers," IEEE TSP, 2006** — GMP の原典。MP に過去・未来の包絡線との交差項を追加。広帯域 DPD で最も参照される。
  https://www.researchgate.net/publication/3319867
- **② S. Boumaiza and F. M. Ghannouchi, "Thermal Memory Effects Modeling and Compensation in RF Power Amplifiers and Predistortion Linearizers," IEEE TMTT, 2003** — 素子温度変化による遅いメモリー効果のモデル化と補償。GaN 自己発熱に直結。
  DOI: 10.1109/TMTT.2003.820157
- **③ H. Ku and J. S. Kenney, "Behavioral Modeling of Nonlinear RF Power Amplifiers Considering Memory Effects," IEEE TMTT, Vol.51, No.12, pp.2495–2504, 2003** — 入力履歴を含む PA モデルを実測から作る考え方。実機較正の基礎。
- **A digital predistorter for power amplifier with memory effect** — メモリー効果を扱う DPD（＝「デジタルにもメモリー効果」の論点）。
  https://www.researchgate.net/publication/229926972
- **An Open-Loop Digital Predistorter Based on Memory Polynomial Inverses** — 開ループ MP 逆特性。
  https://www.researchgate.net/publication/230371934
- **A digital predistortion system based on a generalized memory polynomial for GaN power amplifiers** — GaN 特化の GMP DPD。
  https://www.researchgate.net/publication/385392196

### 2.2 C 帯 GaN Doherty ＋ DPD（本件に近い周波数・変調）
- **A Fully Integrated C-Band GaN MMIC Doherty Power Amplifier**（IEEE, doc 8723483）— 0.25μm GaN-HEMT, 5G massive MIMO, **DPD 適用で ACPR −46 dBc**。
  https://ieeexplore.ieee.org/document/8723483/
- **Broadband GaN MMIC Doherty PA Using Continuous-Mode Combining for 5G Sub-6 GHz** — **64QAM 対応, DPD 後 ACLR < −45 dBc, 4.5–6.5 GHz で 32.2–34.3 dBm**。本件の周波数上限に近い。
  https://www.researchgate.net/publication/358347959
- **Design of a Compact GaN MMIC Doherty PA and System-Level Analysis With X-Parameters for 5G** — 64QAM/8dB PAPR/5.0 GHz で Pavg 30.2 dBm, PAE 32%, **DPD 後 ACPR < −45 dBc**。X パラによる非線形モデル化が参考。
  https://www.researchgate.net/publication/328767084

### 2.3 高度な DPD（広帯域・アレー・省電力）
- **OpenDPD: An Open-Source End-to-End Learning & Benchmarking Framework**（arXiv 2401.08318）
  https://arxiv.org/pdf/2401.08318
- **MP-DPD: Low-Complexity Mixed-Precision Neural Network DPD**（arXiv 2404.15364）
  https://arxiv.org/pdf/2404.15364
- **DeltaDPD: Dynamic Temporal Sparsity in RNN for Energy-Efficient Wideband DPD**
  https://www.researchgate.net/publication/391676523
- **Piecewise Digital Predistortion for mmWave Active Antenna Arrays**（arXiv 2003.06348）— 区分（周波数/電力で分割）DPD。**周波数アジャイル運用の「区分係数」設計の参考**。
  https://arxiv.org/pdf/2003.06348
- **On the Robustness of ACLR and EVM Performance in Hybrid Digital/Analog Predistorters** — MP＋LUT ハイブリッドで ACLR < −50 dBc。
  https://www.researchgate.net/publication/385877132

### 2.4 線形化・効率化のレビュー
- **Review of efficiency enhancement techniques and linearization techniques for power amplifier**
  https://www.researchgate.net/publication/349459732
- **A review of different structures of power amplifiers to improve linearity and efficiency**
  https://www.researchgate.net/publication/386119468
- **Linearity of GaN HEMT RF power amplifiers – a circuit perspective**
  https://www.researchgate.net/publication/261304997

---

## 3. 放送系・国内資料（ラジオ／テレビ朝日 等が使う FPU/STL の枠組み）

放送局（NHK・テレビ朝日など民放）は、番組素材伝送(FPU)・回線(STL/TTL)で M/N 等のマイクロ波帯を使用。**各局固有の GaN アンプ技術論文は公開が限られる**が、制度・規格・システム概要は下記で押さえられる。送信機・FPU の実機はメーカー（池上通信機・NEC 等）が供給。

### 3.1 制度・周波数（総務省）
- **総務省 各システムの概要（FPU）** — 放送事業用マイクロ波帯 B〜G バンド区分（**M: 6.570–6.870 GHz / N: 7.425–7.750 GHz**）。
  https://www.tele.soumu.go.jp/resource/j/research/result/r05/R05_C_sanko.pdf
- **総務省 映像 FPU（C バンド）システム概要** — https://www.soumu.go.jp/main_content/000074201.pdf
- **総務省 情報通信審議会 放送システム委員会 資料** — https://www.soumu.go.jp/main_content/000559092.pdf
- **総務省 電波利用ポータル（周波数割当）** — https://www.tele.soumu.go.jp/j/adm/freq/search/myuse/use/

### 3.2 規格（ARIB）
- **ARIB STD-B33** — テレビ番組素材伝送 FPU の OFDM デジタル伝送（QPSK〜64QAM）。本件の変調規格。
  https://www.arib.or.jp/kikaku/kikaku_hoso/desc/std-b33.html
- **ARIB STD-B11** — マイクロ波帯 FPU デジタル伝送。 https://www.arib.or.jp/kikaku/kikaku_hoso/desc/std-b11.html
- **ARIB STD-B43** — ミリ波帯（42/55 GHz）UHDTV 素材伝送。 https://www.arib.or.jp/kikaku/kikaku_hoso/desc/std-b43.html
- **ARIB 検討対象の周波数帯** — https://www.arib.or.jp/service/gyoumu-shuuhasuu.html

### 3.3 解説・用語・実機ベンダ
- **FPU (放送) — Wikipedia**（占有帯域 17.5 MHz／間隔 18 MHz）: https://ja.wikipedia.org/wiki/FPU_(%E6%94%BE%E9%80%81)
- **STL (放送) — Wikipedia**: https://ja.wikipedia.org/wiki/STL_(%E6%94%BE%E9%80%81)
- 池上通信機「Engineer Talks（FPU 編）」— https://www.ikegami.co.jp/column/detail/17/
- 池上通信機「FPU 集中制御システム（毎日放送様）」— https://www.ikegami.co.jp/news/detail/5/

### 3.4 国内チュートリアル・特許（歪み補償）
- **APMC-MWE 2025 チュートリアル「電力増幅器歪み補償技術とフェーズドアレー無線機」** — GaN Doherty＋DPD（Beyond 5G）。日本語で体系的。
  https://apmc-mwe.org/mwe2025/pdf/tut24/TH4A-2.pdf
- Google Patents（プリディストーション/歪み補償の国内特許例）:
  - WO2010007721A1（電力増幅器の非線形歪補正）
  - WO2013054553A1（歪み補償回路＋高周波電力増幅器の送信装置）
  - JP6340207B2（非線形歪み検出＋歪み補償電力増幅器）
  - JP5251565B2（プリディストータ＋遅延調整）
  - JPS60178781A（TV 送信機 電力増幅列の非直線性の予補正）

> 補足: 「テレビ朝日」固有の GaN アンプ技術論文は公開検索では特定できず。放送局の素材伝送/回線は上記 ARIB/総務省の枠組みに準拠し、実機はメーカー供給という位置づけ。局技報（民放技術報告会・映像情報メディア学会 ITE 等）に個別事例がある可能性があり、必要なら次段で ITE/映情メ学会を当たる。

---

## 4. デバイス資料（住友電工 GaN HEMT）

- 住友電工デバイス・イノベーションズ Power GaN HEMT（Radio Link & SATCOM カタログ）— M 帯 **SGK5867 系（5.85–6.75 GHz）**、（参考）N 帯 **SGC7178-100A（7.1–7.8 GHz）**。
  - Microwave Journal「Power GaN HEMT for Radio Link and SATCOM: SGK5867-30A」
  - everythingRF: SGK5867-30A 製品ページ（※本環境から直接取得不可）
  - SEDI 製品ページ `sedi.co.jp` / 配布 PDF `cdiweb.com`（※本環境から直接取得不可）
- 住友電工 技報 71-15「GaN HEMT の開発」— GaN-on-SiC のデバイス背景。
  https://sumitomoelectric.com/sites/default/files/2020-12/download_documents/71-15.pdf

> 注意: SGK5867 系は **6.75 GHz まで**で M バンド上端 6.75–6.87 GHz が仕様外。全域運用時はデバイス/整合の見直しが必要（本文レポート第3章参照）。

---

## 5. 規格（合否基準）

- 64QAM の EVM 要件（≒ 8%）、ACLR（−30 dBc 級〜）: 3GPP TS 36.101（LTE）／5G NR 資料。放送用途は ARIB STD-B33 のスペクトルマスク/変調精度に準拠。

---

## 6. 本件への当てはめ（使い方メモ）

1. **モデル検討**: まず `ctarver/ILA-DPD`（MATLAB, GMP+ILA）または `OpenDPD`（PyTorch, データセット付）で、MP→GMP の補正力を EVM/ACLR で比較。
2. **周波数アジャイル対応**: `Piecewise DPD`（区分係数）の考え方で、M バンドを数点に区切り**周波数別係数セット**を用意 or 都度再適応。
3. **実測連携**: 住友 SGK5867 の実測 AM/AM・AM/PM、S パラ、ロードプルを入手（Q4）してモデルへ投入。
4. **効率**: C 帯 GaN Doherty＋DPD の論文（§2.2）を参考に、Doherty 併用で 6.5 GHz 帯の高効率化。

以上。追加で当たるべき先（ITE/映情メ学会の局技報、住友の正式データシート）があれば次段で収集します。
