# フェーズ3/4: パッチ一覧と診断レポータ運用

## フェーズ3: パッチ

### 実装済み(本ブランチのコミット)

| # | 種別 | 内容 | 仮説 | 期待効果 | 回帰リスク |
|---|---|---|---|---|---|
| P1 | fix | **FD スムージング戦略の設定プラミング修正**: `upper_phy_factories.cpp` が TD 戦略文字列で FD 戦略を選んでいた(dead knob)。`pusch_channel_estimator_fd_strategy` を参照するよう修正 | 2 | `fd_strategy: mean/none` が効くようになり切り分け実験が可能に。既定値では挙動不変 | 極小(既定パス不変。文字列比較の対象変更のみ) |
| P2 | fix | **チャネル推定の連続マスク判定 off-by-one 修正**: `port_channel_estimator_average_impl.cpp:222` の `end - begin` → 全サブキャリア一致判定に。全 1 マスク(データシンボル)で高速パスが有効になり、「穴 1 個」マスクの誤分類(潜在バッファ不整合)を排除 | 2 | データシンボル毎のチャネル係数コピーが直接パスに(CPU 削減)。潜在 OOB 除去 | 小(出力同一。pxsch_bler_test で rank1/rank2 リグレッションなし確認済み) |
| P3 | feat | **PUSCH 診断計装**(フェーズ2、`docs/diagnostics/03_instrumentation_design.md`)。単一フラグ `expert_phy.pusch_diagnostics_enabled` | A,1,3,4,6,8,9 | 三者 SINR・レイヤ別 SINR・cond・ポート別雑音・LLR 統計が info ログ 1 行に | フラグ off で挙動不変(分岐のみ)。on 時も計測は既存バッファの読み出しのみ |
| P4 | test | `pxsch_bler_test` に `-d`(診断収集)を追加。BLER カーブ実験のハーネス | 検証 | rank1/rank2 の BLER-vs-SINR 曲線と診断量をオフラインで取得可能 | テスト専用コード |
| P5 | tool | `scripts/ocudu_link_diag.py`(フェーズ4 レポータ、下記) | 全部 | 測定ログの自動仕分け | RAN バイナリ非接触 |

### 提案(未実装。効果順・実測での裏取り後に適用推奨)

| # | 種別 | 内容 | 仮説 | 期待効果 | リスク/備考 |
|---|---|---|---|---|---|
| R1 | CONFIG | `expert_phy: pusch_channel_equalizer_algorithm: mmse`(2レイヤ測定時) | 1 | ill-conditioned RE での雑音強調緩和(数 dB) | 設定のみ。CPU 微増 |
| R2 | CONFIG | `pusch: olla_max_snr_offset: 10.0` + `olla_target_bler: 0.05` | A,5 | 系統的 SNR 過大報告時にも MCS が追従(BLER 20%→5%目標) | 設定のみ。SNR 変動が大きい環境では MCS が下がりすぎる方向のリスク |
| R3 | CONFIG 実験 | `expert_phy: pusch_sinr_calc_method: evm` | A | LA がデータシンボル実測 SINR を参照 → 受信ギャップを自動吸収 | EVM は復調成功前提の推定なので極低 SINR で楽観化に注意。実験で確認 |
| R4 | CONFIG | `pusch: rv_sequence: [0, 2, 3, 1]` | 5 | 再送を IR 合成化(現状 rv=0 固定の Chase)。BLER>0 環境の実効スループット向上 | 設定のみ。上流実装は対応済み |
| R5 | CODE | UL SNR→MCS テーブルの再校正(+3dB シフト or 帯域依存化)。`mcs_calculator.cpp:57-66` | A | OLLA 依存を減らし初期 MCS を適正化 | 全帯域幅に影響。pxsch_bler_test で帯域別カーブを取ってから |
| R6 | CODE | OLLA 更新スキップ条件の緩和: `ue_link_adaptation_controller.cpp:65-67` で「码率制約で MCS を下げた送信」も学習対象に(olla_mcs との差 1 まで許容等) | 5 | UCI 多重・小グラント時も OLLA が学習し続ける | スケジューラ挙動変更。シミュレーションでの確認要 |
| R7 | CODE | SRS ベース UL rank 判定の実雑音化+cond ゲート: `ue_channel_state_manager.cpp:69-71,87-137` | 4 | 悪条件チャネルで rank2 を掴まない | rank 上げ渋りのリスク。閾値 config 化とセットで |
| R8 | CODE(中規模) | **MMSE-IRC**: DMRS 残差からポート間干渉共分散 R を推定し W=(HᴴR⁻¹H+I)⁻¹HᴴR⁻¹。挿入点: `dmrs_pusch_estimator`(残差保存)+ `channel_equalizer_generic_impl`(共分散受け渡し API 拡張) | 1 | 着色干渉下の 2レイヤ BLER 改善(4Rx の余剰 DoF 活用) | 大きめ。まず計装で干渉の実在確認(llr/evm/E3 シグネチャ)後に着手 |

## フェーズ4: 診断レポータ `scripts/ocudu_link_diag.py`

RAN バイナリと独立したスタンドアロン解析スクリプト(標準ライブラリのみ、オフライン batch)。

### 測定時の準備(ホットパス非接触)

```yaml
# gNB 設定に追加(診断一式)
expert_phy:
  pusch_diagnostics_enabled: true
log:
  filename: /tmp/gnb.log
  all_level: warning
  phy_level: info          # PUSCH 1 行/送信(diag 付き)
metrics:
  enable_json: true        # :8001 の WebSocket(下記でキャプチャ)
  enable_ru_metrics: true  # OFH 窓カウンタ
  enable_log: true         # ul_olla/dl_olla 入り metrics ログ
```
```bash
# JSON metrics のキャプチャ(測定中回しっぱなし)
websocat ws://<gnb>:8001 > metrics.jsonl     # or: python3 docker/telegraf/ws_adapter.py
```

### 解析(測定後)

```bash
python3 scripts/ocudu_link_diag.py --json metrics.jsonl --log /tmp/gnb.log
python3 scripts/ocudu_link_diag.py --selftest    # ルール回帰テスト(アンカー症状含む)
```

出力: 観測窓×症状ごとに `[バケツ] 窓 リンク: 症状 / 根拠(数値) / 次アクション` を 1 件ずつ、
末尾にバケツ別集計と推奨アクション上位 3 件。
バケツ: `ENV/RU`(ユーザ操作・コード不要)/ `CONFIG`(設定キー)/ `CODE`(OCUDU 修正、
該当ファイル指し示し)/ `CAPACITY`(理論上限、対処不要)。
DL はコード帰着先を gNB 側 LA/スケジューラ/プリコーディングに限定して表示する。

### 主な判定ルール(実装値。閾値は CLI で調整可)

- 報告 SNR が MCS 要求+6dB 超なのに BLER 高 & `sinr_evm` 情報なし → **[CODE] UL OLLA/LA 非追従**
  (`ul_olla` があれば飽和判定を付記)
- `sinr_eq − sinr_evm > 3dB` & BLER 高 → **受信ギャップ**として細分:
  OFH late/early/missed>0 → **[CONFIG→CODE] ta4/輸送** /
  ポート別 nvar/RSRP 差 >6dB → **[ENV/RU] ブランチ校正** /
  cond >10dB → **[ENV+CODE] 空間相関・RI 楽観** /
  CFO 変動 >200Hz → **[CODE→ENV] 位相追従(td_strategy: interpolate 実験)** /
  それ以外 → **[ENV/RU] バースト/狭帯域干渉(E3 型)**(LDPC max-iter が裏付け)
- 観測 MCS がテーブル選択レンジ+2 超 → **[CODE] LA 不追従**
- SNR と BLER が整合 → **[ENV/RU]**(RSRP < -90dBm はリンクバジェット注記)
- PRB≈100% & BLER<2% & 高 MCS → **[CAPACITY]**(UL 2/10 スロット上限)
- PRB<70% & BSR 滞留 & BLER 低 → **[CONFIG/CODE] スケジューラ/BSR**
- 中低 SINR で LLR 飽和率 >0.6 → **[CODE] デマップスケーリング**
- DL: CQI≥12 & BLER 高 → **[CODE] DL OLLA** / CQI<7 整合 → **[ENV]** / 中間 → RI/PMI 確認

### 最初の検証実験(手順)

1. **rank1 曲線(装置なしのドライラン)**: 本リポジトリでビルドした
   `pxsch_bler_test -P 4 -L 1 -m <MCS> -S <SINR> -B 273 -d` を MCS/SINR で掃引し、
   実装の BLER-vs-SINR 曲線(教科書比較の基準)を得る。
   実測済み: MCS27 の崖 ≈19.2dB(単一タップ・4Rx)。
2. **実機 rank1**: max_rank:1・UE 固定(高 SNR)のまま `pusch: min_ue_mcs=max_ue_mcs=<MCS>` で
   MCS を手動で振り、各点 1〜2 分測定。レポータに `--log/--json` を食わせる。
   - 曲線が 1. より大きく右(高 SNR 側)→ 受信機起因(レポータは受信ギャップ系に分類)。
   - 曲線は 1. と整合、だが自動 MCS 選択時に BLER 高止まり → リンクアダプ起因
     (レポータは [CODE] OLLA 非追従に分類)。
   - アンカー症状(SNR31dB/MCS27/BLER18-29%)はセルフテスト同梱: `--selftest` が
     [CODE] リンクアダプ非追従(diag 無し)/ 受信ギャップ細分(diag 有り)に落ちることを保証。
3. その後 `pusch: max_rank: 2` に上げ、rank2 の上乗せ分(レイヤ別 SINR 差・cond・
   post-eq SINR 低下量)だけを比較する。rank1 の異常を先に潰すこと(原因が混ざるため)。
