# OCUDU config プラミング横断監査レポート

- 対象リビジョン: `main` = `91552ed` (terry-hasegawa/ocudu、2026-07-04 時点の remote main 先端)
- 監査範囲: `apps/units/flexible_o_du/o_du_low/`、`apps/units/flexible_o_du/o_du_high/du_high/`、
  `lib/scheduler/config/`、`lib/du/**`、`lib/phy/upper/upper_phy_factories.cpp`
- 手法: フェーズ0でマップ化 → フェーズ1でヒューリスティクス A〜D を機械スキャン(スクリプト)＋全候補をコード読解で個別検証。
  弱い指摘・シロ判定は本文末尾に分離して記載。

---

## 0. TL;DR

| 判定 | 件数 |
|---|---|
| 確定 finding(実装対象に選定) | 3 件 (A1, B1, A5) |
| 確定 finding(後続候補・レポートのみ) | 5 件 (A2, A3, A4, C1, C2) |
| 要確認事項(先に人間の確認が必要) | 2 件 (N1, A4 の NTN 意図) |
| 監査でシロと判定(誤検知として除外) | 16 件 |

**実装済み(このブランチに atomic コミットとして格納。1 コミット = 1 MR 相当):**

1. **A1** `f4_enable_occ` dead-knob: translator が PUCCH F4 の `occ_supported` を設定しない(＋yaml writer 欠落)
2. **B1** `max_mimo_layers = 1` ハードコード: `pdsch_builder_params::max_nof_layers` を無視(#423 と同型)
3. **A5** `du_high_unit_metrics_config::autostart_stdout_metrics` 完全 dead フィールドの削除

各コミットは独立しており、単体で cherry-pick 可能。`audit/patches/` に `git format-patch` 出力、
`audit/issue_draft_<id>.md` / `audit/mr_description_<id>.md` に GUI 起票用ドラフトを格納。

---

## 1. 重要な前提メモ(先に人間の確認が必要)

### N1: fd/td dead-knob(パターンの種 #1)が本ミラーではまだ未修正

タスク記載では「既に修正済み」とされている
`lib/phy/upper/upper_phy_factories.cpp` の fd/td 取り違えは、**本リポジトリの main 先端 (91552ed) にまだ存在する**:

```
lib/phy/upper/upper_phy_factories.cpp:570  if (config.pusch_channel_estimator_td_strategy == "none") {
lib/phy/upper/upper_phy_factories.cpp:572  } else if (config.pusch_channel_estimator_td_strategy == "mean") {
```

fd 戦略の選択に td 文字列を参照しており、`pusch_channel_estimator_fd_strategy` は依然無視される
(CLI validator は td に "none"/"mean" を許さないため、fd は常に `filter` 固定)。
**upstream(GitLab 側 dev)で修正済みなら本ミラーへの sync 待ち。未修正なら最優先の XS 修正候補**
(重複作業を避けるため今回は実装せず、確認待ちとした)。

### #423(既知)の現状確認

`lib/scheduler/config/serving_cell_config_factory.cpp:160` の `.p0_nominal_without_grant = -76` は現存。
`make_default_pusch_config()` は `ran_cell_config`(`ul_cfg_common` 経由で設定値
`p0_nominal_with_grant` を保持)を受け取っているのに直値を使っており、B1 と完全に同型。
#423 は起票済みとのことなので本監査の成果物からは除外(発見手法の検証用リファレンスとして使用)。

---

## 2. フェーズ0: config プラミングマップ

```
[du_low unit]
  du_low_config.h                     宣言 (du_low_unit_config: expert_phy / loggers / expert_execution / hal / metrics)
  du_low_config_cli11_schema.cpp      CLI (--pusch_* / --max_proc_delay / ...)
  du_low_config_validator.cpp         検証 (pdsch_processor_type ∈ {auto,flexible,generic} 等)
  du_low_config_yaml_writer.cpp       yaml 出力 (CLI と 1:1、欠落なし ✓)
  du_low_config_translator.cpp        → odu::du_low_config / upper_phy_factory_configuration (hop 1)
  flexible_o_du_factory.cpp           → fapi adaptor 系 (nof_slots_request_headroom / allow_request_on_empty_ul_tti)
  du_low_hal_factory.cpp              → bbdev/hwacc (hwacc_type / harq_context_size)
  lib/phy/upper/upper_phy_factories.cpp  最終消費 (hop 2) ← fd/td 取り違えはここ (N1)

[du_high unit]
  du_high_config.h                    宣言 (du_high_unit_config: cells_cfg / pucch / pusch / prach / srs / ntn / ...)
  du_high_config_cli11_schema.cpp     CLI (326 オプション) ＋ custom band の band_helper 登録 (:3029-3035)
  du_high_config_validator.cpp        検証
  du_high_config_yaml_writer.cpp      yaml 出力 (305 キー、**64 CLI オプション欠落** → C1)
  du_high_config_translators.cpp      → du_cell_config(.ran = ran_cell_config)/ scheduler_expert_config / MAC (hop 1)
  du_high_logger_registrator.h        → ロガー登録 (du/mac/rlc/... level の消費先)

[scheduler config]
  ran_cell_config_helper.cpp          cell_config_builder_params(_extended) → ran_cell_config 既定生成
                                      (translator が生成後に上書きする既定: -76 / -90 / -100 等)
  serving_cell_config_factory.cpp     ran_cell_config → UE dedicated config 既定生成 (hop 2)
                                      ★ #423 / B1 のハードコードはここ
  bwp_builder_params.h / pucch_resource_builder_params.h / srs_builder_params.h
                                      hop 1→2 間の受け渡しパラメータ (dead-knob の墓場になりやすい)

[lib/du]
  du_ran_resource_manager_impl.cpp    make_default_ue_cell_config() を呼び UE dedicated config を実体化 (:275,308)
  converters/asn1_rrc_config_helpers.cpp  → RRC ASN.1 (CellGroupConfig) エンコード
  du_pdsch_resource_manager.cpp       一部フィールドはここでセル毎に上書き (例: harq_process_num_size_dci_1_1)
```

dead-knob は 2 箇所で発生し得る: **hop 1**(app struct → translator 未読)と
**hop 2**(builder params → factory 未読。fd/td・#423・B1・A1 は全てここ)。

---

## 3. Findings 一覧表

凡例: 種別 A=dead-knob, B=config化漏れ(ハードコード), C=既定の二重管理/writer drift, D=enum/文字列マッピング不一致

| id | file:line | 種別 | 症状 | なぜバグ/リスクか | 影響(挙動が変わる条件) | 提案修正 | 規模 | 独立性 | 確信度 |
|----|-----------|------|------|-------------------|------------------------|----------|------|--------|--------|
| **A1** | `apps/.../du_high/du_high_config_translators.cpp:912-918`(FORMAT_4 ブロック)/ 宣言 `du_high_config.h:450` / CLI `du_high_config_cli11_schema.cpp:1259` | A | `--f4_enable_occ true` が無言で無視される。translator の F4 ブロックが `f4_params.occ_supported`(`pucch_resource_builder_params.h:146`)を設定しない。yaml writer にも `f4_enable_occ` キーが無い(`f4_occ_length` は :410 にある) | F1 の同項目は `f1_params.occ_supported = user_pucch_cfg.f1_enable_occ`(:866)と正しく配線されており、F4 だけ欠落。`occ_supported=false` のままだと `pucch_resource_generator.cpp:121` で `nof_occs=1` となり OCC 多重が生成されず、`mux_capacity_234()`(builder_params.h:394)も 1 を返す | PUCCH F4 使用時に `f4_enable_occ: true` を設定した場合のみ。PRB あたり 1 UE しか多重されず(期待は occ_length=2/4 UE)、`f4_occ_length` も実質 dead 化 | translator FORMAT_4 ブロックに 1 行、yaml writer に 1 行追加 | XS | y | **確定** |
| **B1** | `lib/scheduler/config/serving_cell_config_factory.cpp:280` | B | `make_default_pdsch_serving_cell_config()` が引数 `pdsch_builder_params`(`max_nof_layers` を保持、bwp_builder_params.h:29)を受け取りながら `cfg.max_mimo_layers = 1;` とハードコード | UE へ RRC で `PDSCH-ServingCellConfig.maxMIMO-Layers = 1` が常時通知される(`asn1_rrc_config_helpers.cpp:2798-2800`、`>0` なので常に present)。一方 CSI 側は同じ `max_nof_layers` から RI 制限ビットマップを rank≦max_nof_layers で生成(`csi_helper.cpp:683`)しており、**UE に送る設定が内部矛盾**。maxMIMO-Layers は TS 38.212 §5.4.2.1 の LBRM の X 値であり、gNB は固定の `tbs_lbrm_default`(4レイヤ相当、`sch_constants.h:26`、FAPI `pdsch.cpp:37`)を使うため gNB/UE の LBRM 前提も乖離。フィールドの書き手は工場のこの 1 箇所のみ(全リポ grep で確認) | `nof_antennas_dl > 1`(または `pdsch max_rank > 1`)の全セル。maxMIMO-Layers を尊重する UE は DL rank を 1 に制限(スループット半減〜1/4)。LBRM X=1 で soft buffer を確保する UE は大 TBS 時にレートマッチング不一致の恐れ | `cfg.max_mimo_layers = pdsch_params.max_nof_layers.value_or(1);`(1 行) | XS | y | **確定**(コード上の矛盾として。OTA での実害度合いは要実測) |
| **A5** | `apps/.../du_high/du_high_config.h:1212` | A | `du_high_unit_metrics_config::autostart_stdout_metrics` は宣言のみで、CLI バインドも読み手も無い(リポ全体で参照は宣言 1 箇所) | gnb/du アプリレベルの同名フィールド(`gnb_appconfig.h`/`du_appconfig.h`、こちらは CLI/consumer あり)と紛らわしく、du_high 側に設定しても効かないと誤解を生む dead 宣言 | 挙動影響なし(誰も読まないフィールドの削除) | フィールド削除(1 行) | XS | y | **確定** |
| A2 | `du_high_config.h:60` / CLI `:121` / translator `:1258` → `scheduler_expert_config.h:259` | A | `--high_latency_diagnostics_enabled` は sched expert config の `log_high_latency_diagnostics` に写されるが、**lib/ 内に読み手ゼロ** | ユーザーが有効化しても何も起きない dead-knob(fd/td と同型、hop 2 での死) | 常時(このオプションを使った場合) | 消費側の実装(意図の確認要)または deprecate。CLI 削除は既存 config を壊すため単純削除は不可 | M | y | 確定(dead であること)/ 要確認(本来の意図) |
| A3 | `du_high_config.h:134` / CLI `:812` / validator `:836` / translator `:1236` → `scheduler_expert_config.h:203` | A | `--min_pucch_pusch_prb_distance` は宣言→CLI→検証→translator まで完走するが、**lib/scheduler に読み手ゼロ**(unittest が値をセットするのみ) | PUCCH/PUSCH 間ガード PRB を期待して設定しても無効。検証まで通るため気づけない | 常時(このオプションを使った場合) | scheduler 側の消費実装(リソースグリッド制約、M)または deprecate | M | y | 確定(dead であること)/ 要確認(実装 or 廃止の判断) |
| A4 | `du_high_config_translators.cpp:1199` → `scheduler_expert_config.h:195` | A | NTN セル設定時に `out_cfg.ue.auto_ack_harq = true` がセットされるが、`auto_ack_harq` の読み手が lib/(scheduler/mac/du)にゼロ | NTN で DL HARQ を自動 ACK 扱いにする意図が機能していない可能性。ただし `dl_harq_feedback_disabled` マスクという別機構があり、そちらで代替済みで本フィールドが残骸の可能性もある | NTN セルのみ | NTN の意図確認後、実装 or フィールド削除 | XS〜M | y | 確定(dead であること)/ **要確認**(NTN 機能への実影響) |
| C1 | `du_high_config_yaml_writer.cpp` 全体 | C | CLI 326 オプション中 **64 個が yaml writer に無い**(機械差分+8件目視確認: `f4_enable_occ`, `msg3_delta_power`, `freq_domain_shift`, `ta_target`, `ta_cmd_offset_threshold`, `cfra_enabled`, `rlm_resource_type`, `min_pucch_pusch_prb_distance`。残りは CLI/yaml のキー名不一致による偽陽性の可能性を個別 triage 要) | config dump の round-trip で設定が黙って失われる。writer が仕様追加に追従しない drift | yaml writer 出力を設定ファイルとして再利用した場合 | セクション毎に writer へキー追加(64 件は要個別 triage: 一部は別名記載/意図的除外の可能性) | S | y | 確定(欠落の存在)/ 要確認(64 件の個別判定) |
| C2 | `du_low_config_translator.cpp:36+59, 43+60` | C | `enable_metrics` が 2 回代入、`ldpc_decoder_type="auto"` が 2 回代入 | 無害だが、片方だけ直す将来の修正を誘発する二重管理 | なし(現状同値) | 重複行の削除(2 行) | XS | y | 確定 |
| C3 | `ran_cell_config_helper.cpp:235` / `du_high_config.h:245` | C | `p0_nominal_with_grant = -76` の既定が helper と app config の 2 箇所に直値で存在(translator :672 が上書きするため実害なし)。`:160` の #423 と合わせ、-76 系は 3 箇所に分散 | #423 修正時にこの二重管理も整理対象(既定値の単一情報源化) | なし(translator 経由の本番経路では上書きされる) | #423 の修正に含めるのが自然(単独 MR 不要) | XS | n(#423 に従属) | 確定 |

### 実装対象の選定理由(上位 3 件)

- **A1**: XS・独立・確定・**デフォルト挙動不変**(`f4_enable_occ` 既定 false = `occ_supported` 既定 false)。F1 との対称性から意図が自明。
- **B1**: XS・独立・確定。**デフォルト挙動は複数アンテナ構成で変化する**(それが修正の目的。
  1 アンテナ構成では `max_nof_layers` 自動導出=1 のため不変)。#423 と同型で説明が容易。OTA 検証推奨と MR に明記。
- **A5**: XS・独立・確定・挙動影響ゼロ(削除のみ)。
- A2/A3/A4 は dead であることは確定だが、「実装すべきか廃止すべきか」の設計判断が必要なため実装対象から除外(後続候補)。
- C1 は 64 件の個別 triage が必要なため実装対象から除外(後続候補)。

---

## 4. 監査でシロと判定した候補(誤検知の除外記録)

機械スキャンが挙げ、コード読解で問題なしと確認したもの(再監査時の重複作業防止のため記録):

| 候補 | シロの根拠 |
|---|---|
| `nof_slots_request_headroom` / `allow_request_on_empty_uplink_slot` (du_low) | translator ではなく `flexible_o_du_factory.cpp:66-67` が fapi adaptor config へ配線 |
| `hal_level` / du_high の `du/mac/rlc/f1ap/f1u/gtpu/ntn_level` / `hex_max_size` / `f1ap_json_enabled` | logger_registrator 系ヘッダで消費 |
| `harq_context_size` / `hwacc_type` (bbdev) | `du_low_hal_factory.cpp:75` 等で消費 |
| `custom_freq_bands` 一式 | CLI パース後フック `autoderive_du_high_parameters_after_parsing()` が `band_helper::register_custom_bands()` に登録(cli11_schema.cpp:3029-3035)。translator を通らない設計 |
| `du_high_unit_rach_config::ports` | `flexible_o_du_factory.cpp:72,88` で prach_ports として消費 |
| `preamble_rx_target_pw` / PUCCH `p0_nominal` / `msg3_delta_power` / `freq_domain_shift` | translator :605 / :681 / :674 / :949 で配線済み(後2者は C1 の yaml 欠落のみ該当) |
| `harq_process_num_size_dci_1_1 = n4` (factory:79) | `du_pdsch_resource_manager.cpp:62` がセル毎に上書き。既定値に過ぎない |
| `pdsch_processor_type` の分岐網羅 (D) | validator の許容 {auto,flexible,generic} と translator 分岐が一致 |
| `sinr_type_from_string` (D) | CLI 許容 3 値すべてマッピングあり、不正値は assert |
| `make_default_csi_meas_builder_params()` の `mcs_table=qam64` | 当該フィールドは戻り値の `.csi_params` スライスに含まれず不活性。本番経路は `make_csi_meas_config_builder_params()`(:373)が pdsch 設定から導出 |
| upper_phy factory config の translator 未設定フィールド群 | executor/pool/notifier 等、アプリが直接組み立てる依存物のみ |

---

## 5. 後続候補(実装せず、レポートのみ)

優先順(独立性・費用対効果):

1. **N1 の確認**: fd/td 修正が upstream に存在するか確認。無ければ XS で即修正可能(監査の種と同一内容)。
2. **A2 / A3**: dead-knob 2 件。それぞれ「実装 or deprecate」の設計判断を issue で議論してから。
3. **A4**: NTN の DL HARQ 自動 ACK。NTN 機能オーナーへの確認が先。
4. **C1**: yaml writer の 64 キー欠落。セクション単位で 3〜4 個の小 MR に分割可能。
5. **C2**: du_low translator の重複代入(cosmetic、他修正のついでで可)。
6. **C3**: #423 修正時に -76 の単一情報源化を含める提案。

---

## 6. 実装済み 3 件の検証結果

(このセクションはコミット後に追記 — `audit/patches/` と各 MR 説明文を参照)

---

## 7. 監査手法(再現手順)

- フィールド抽出+参照分類スキャナ: `scan_config_usage.py`(宣言/CLI/yaml/validator/translator/lib/tests に分類、
  「CLI あり・translator/lib 消費ゼロ」を dead-knob 候補として報告)
- 消費先限定クロスチェック: `translator_deadness.py`(名前衝突による偽陰性対策。du_high フィールド×du_high translators、
  scheduler_expert_config×lib/scheduler、du_cell_config×lib/ 等の組で消費ゼロを列挙)
- yaml writer 網羅性: `yaml_completeness.py`(CLI オプション名と yaml キー名の集合差分)
- 全候補についてコード読解で確定/シロ/要確認を判定(機械スキャン単独の指摘は採用していない)
