# UL/DL BLER・スループット調査 — サマリと索引

対象環境: OCUDU gNB(OFH split 7.2)+ 44R14-N77a(4T4R)、n77 / 100MHz / SCS30k /
TDD DDDSUUDDDD、OTA。適用コンフィグ: `gnb_ru_ofh_tdd_n77_100mhz_4x4.yml`(pusch.max_rank: 1)。

実測アンカー: rank1・MCS27・報告 SNR ≈31.2dB・RSRP ≈-35dBm で **UL BLER 18〜29%、MCS 据え置き**
(参考 DL: ri3・mcs26-27・BLER 4-8%)。

## ドキュメント構成

| ファイル | 内容 |
|---|---|
| [01_ul_dl_code_map.md](01_ul_dl_code_map.md) | フェーズ0: UL PUSCH 受信チェーン/DL リンクアダプのコードマップ(ファイル:行)。SNR 報告経路、n-TA/ta4 窓、既存メトリクスの全体像 |
| [02_static_review.md](02_static_review.md) | フェーズ1: 仮説A・1〜10 の静的レビュー+**実機バイナリでの BLER 実験(E1〜E5)**。判定根拠の本体 |
| [03_instrumentation_design.md](03_instrumentation_design.md) | フェーズ2: PUSCH 診断計装の設計(単一フラグ、出力フォーマット、判読ガイド) |
| [04_patches_and_reporter.md](04_patches_and_reporter.md) | フェーズ3/4: 適用済み・提案パッチ一覧(仮説/期待効果/リスク付き)と、診断レポータ `scripts/ocudu_link_diag.py` の運用手順・検証実験手順 |

## 調査でわかったこと(要点)

### 1. 実測アンカーの説明は閉じた(仮説A)

- 同梱の `pxsch_bler_test`(実機と同一の既定構成: ZF・FD=filter・post-eq SINR)で実測した
  **MCS27 の実際の所要 SINR は ≈19.2dB**。スケジューラの SNR→MCS テーブル値は 16.04dB で
  約 3dB 楽観(テーブル自体に「20MHz SISO ZMQ 校正の暫定値」と明記)。
- BLER 18〜29% は LDPC の急峻な崖の縁 = **実効 SINR ≈19dB** を意味する。報告 31.2dB との
  **差 ≈12dB が「報告 SNR に映らない受信劣化」**。
- UL OLLA は既定で有効だが **`olla_max_ul_snr_offset` 既定 ±5dB で飽和**し、12dB 級の系統誤差は
  構造的に吸収できない → MCS が下がらない。加えて「送信 MCS ≠ OLLA 提案 MCS のとき更新スキップ」
  (`ue_link_adaptation_controller.cpp:65-67`)が学習をさらに止める。
- 劣化 12dB の正体は実験で絞り込み: 白色雑音(E2)・深フェード/レイヤ分離悪化(E4/E5)は
  **post-eq SINR に正直に映る**ため該当しない。「SNR 高いまま BLER 高」を再現できたのは
  **データ RE の局所破壊(バースト/狭帯域干渉・RU 内部障害の類)のみ**(E3: SINR 26dB・EVM 3% の
  まま BLER 21%、LDPC max-iter 頻発を伴う)。OTA の位相ドリフト(仮説9)も同型の署名を作り得る。
  → 新計装の `sinr_evm`(データシンボル実測)とシンボル別 EVM で判別可能。

### 2. コードのバグ(発見・修正済み)

- **FD スムージング戦略の設定プラミングバグ**(仮説2): `upper_phy_factories.cpp` が
  TD 戦略の文字列で FD 戦略を選択しており、`pusch_channel_estimator_fd_strategy` は dead knob
  だった。既定値どうしでは偶然無害(=今回の BLER の原因ではない)が、切り分け実験を阻害。→ 修正済み。
- **チャネル推定の連続マスク判定 off-by-one**: 高速パスが決して発動せず(性能損)、
  「穴 1 個」のマスクを誤って連続扱いする潜在バッファ不整合。→ 修正済み
  (rank1/rank2 でリグレッションなしを確認)。

### 3. 宿題のクローズ(仮説10: n-TA)

**DU ソース修正は不要**。OFH 構成では DU は UL データパスに n-TA を適用しない。
n-TA は SIB1/RRC で UE に通知されるのみで、値はバンドから固定導出(**FR1 → 25600**、
`band_helper.cpp:1672-1679`)。RU 確定値 25600 と整合。残る変数は ta4 受信窓のみで、
窓外れは既存 OFH メトリクス(`metrics.enable_ru_metrics`: early/on_time/late、
missed_uplink_symbols)で観測できる。

### 4. アーキテクチャ上の事実(2レイヤに効く)

- **MMSE-IRC は存在しない**(仮説1)。複数レイヤ時の雑音は「全ポート最大値のスカラー1個」
  (`channel_equalizer_generic_impl.cpp:528`)。干渉の白色化は不可。
- **既定の等化器は ZF**(`pusch_channel_equalizer_algorithm: zf`)。`mmse` は設定のみで切替可能。
- **UL rank 選択は楽観バイアスあり**(仮説4): SRS の雑音を「受信電力-30dB」と固定仮定し、
  高 rank 優遇バイアス +2dB(`ue_channel_state_manager.cpp:69-137`)。悪条件チャネルでも
  rank2 を掴み得る。
- 仮説3(DMRS/OCC)・6(LLR)・7(BFP9 圧縮)・8(ブランチ間アンバランス)は実装正当/
  主要因ではないと判定(例外条件と根拠は 02 を参照)。

## 追加した道具

### PUSCH 診断計装(フェーズ2)— 単一フラグ

```yaml
expert_phy:
  pusch_diagnostics_enabled: true   # 既定 false。off 時はホットパス不変
log:
  phy_level: info                   # diag は info の PUSCH 行 1 行に載る(debug 不要)
metrics:
  enable_json: true                 # :8001 WebSocket(websocat 等でキャプチャ)
  enable_ru_metrics: true           # OFH 窓カウンタ
  enable_log: true                  # ul_olla/dl_olla 入り metrics ログ
```

出力(1 PUSCH = 1 行): 3種 SINR(`sinr_ce`/`sinr_eq`/`sinr_evm`)・レイヤ別 SINR(`sinr_lyr`)・
condition number(`cond`, 2レイヤ時)・ポート別雑音分散(`nvar_p`)・LLR 飽和率/平均
(`llr_sat`/`llr_avg`)・ポート別 RSRP・EPRE・t_align・CFO。

### 診断レポータ(フェーズ4)

```bash
python3 scripts/ocudu_link_diag.py --json metrics.jsonl --log /tmp/gnb.log
python3 scripts/ocudu_link_diag.py --selftest   # ルール回帰(アンカー症状含む 9 ケース)
```

観測窓ごとに **ENV/RU・CONFIG・CODE・CAPACITY** に分類し「症状/数値根拠/次アクション
(チューニングすべき設定キー or 調査すべきコード箇所)」を 1 行ずつ+末尾サマリを出力。
DL のコード帰着は gNB 側 LA/スケジューラ/プリコーディングに限定。

## 次の一手(推奨順)

1. 計装フラグ一式を有効化して現場 rank1 を再測定 → レポータにかける
   (`sinr_eq` vs `sinr_evm` のギャップ有無で「受信劣化か、リンクアダプか」が即決)。
2. 設定のみの実験: `pusch_sinr_calc_method: evm` / `pusch: olla_max_snr_offset: 10` /
   `pusch: rv_sequence: [0,2,3,1]`。
3. rank2 測定時は `pusch_channel_equalizer_algorithm: mmse` を併用し、`cond` とレイヤ別 SINR で
   分離品質を評価(rank1 の異常を潰してから。順序厳守)。
4. 中期のコード改善(SNR テーブル再校正・OLLA スキップ緩和・RI 実雑音化・MMSE-IRC)は
   04 の提案表 R5〜R8 を参照。

## 成果物コミット

| コミット | 内容 |
|---|---|
| `phy: fix PUSCH channel estimator config plumbing issues` | FD-strategy/連続マスクの 2 バグ修正 |
| `phy: add optional PUSCH link diagnostics` | 計装本体(単一フラグ、15 ファイル) |
| `tests: add PUSCH diagnostics toggle to pxsch_bler_test` | `-d` 実験ハーネス |
| `scripts: add offline UL/DL link diagnoser` | `scripts/ocudu_link_diag.py` |
| `docs: add UL/DL BLER investigation reports` | 本ディレクトリのレポート 4 本 |
