# OCUDU DL-PRS 実UE検証（Tier 1）事前調査レポート

- 調査対象: `gitlab.com/ocudu/ocudu` デフォルトブランチ **`dev`** HEAD = **`09b3f6e80a15caf32f0da344cf045030266da55f`**（2026-07-14, "docker: install RDMA deps for mlx5 DPDK PMD"）
  - 注: 上流のデフォルトブランチは `main` ではなく `dev`。本レポート中の file:line は全てこのコミットに対するもの。
- 調査方法: read-only 静的解析のみ（ソース改変なし）。行番号は実ファイルを読んで確認済み。
- タグ凡例: **[確定]** = ソースコード上で確認済み / **[要実機確認]** = 静的解析では判定不能、実機・実UEで要検証 / **[未確認]** = Web 上の記述のみでソース未検証（タスク D のみ）

---

## エグゼクティブサマリ

1. **タスク A（DU 送信ギャップ）**: PRS は PHY・FAPI メッセージ・FAPI→PHY 変換まで**完成済み**。欠落は正確に 5 点 — (1) `dl_sched_result` の PRS コンテナ、(2) `prs_scheduler`（occasion 判定 + grid 予約）、(3) `cell_scheduler` への配線、(4) MAC→FAPI で `add_prs_pdu()` を呼ぶ変換、(5) PRS resource list を運ぶ cell config 配管。CSI-RS がほぼ完全なテンプレートになるが、**CSI-RS と違い grid `fill()` による予約が必須**（PHY に PRS 用 PDSCH レートマッチング経路が無いため）。
2. **タスク B（NRPPa 迂回）**: **NRPPa は Tier 1 クリティカルパスから外せる**。ocudu の NRPPa/F1AP positioning は全て UL-SRS / E-CID / TRP メタ情報中心で、UE の DL-PRS 測定・LPP 報告経路（NAS 透過）に一切介在しない。しかも DU 側は DL-PRS 設定を NRPPa 経由で供給する実装を持たない。
3. **タスク C（単一 O-RU での 1b）**: PHY/FAPI は単一セルから**最大 16 PRS PDU/スロット**を独立パラメータ（n_id_prs, comb offset, symbol, RB, power）で送信可能。2 PRS-set 構成は既存下位層で構築可能 [確定]。UE が同一 PCI/ARFCN の 2 TRP を受理して RSTD を返すかは [要実機確認]。
4. **タスク D（hug0lin Open5GS フォーク）— 前提修正が必要**: フォーク（`hug0lin/open5gs` ブランチ `LMF2`）の実体は **NRPPa ベースの E-CID/CELLID LMF であり、LPP コーデックも AMF の LPP NAS トランスポートも一切存在しない** [確定]。「LPP-over-NAS 輸送路の substrate」としてそのまま使える配管は無く、LPP トランスポート自体から新規構築が必要。

---

## タスク A — DU: DL-PRS 送信の欠落マップ

### 前提確認（ギャップの存在）[確定]

- `lib/scheduler` / `include/ocudu/scheduler` に PRS の痕跡 **0 件**（grep 全域）。
- `include/ocudu/scheduler/result/sched_result.h:35` — `dl_sched_result` は `csi_rs` のみ保持。
- `lib/fapi_adaptor/mac/p7/mac_to_fapi_fastpath_translator.cpp` は CSI-RS を追加する（`:147`）が `add_prs_pdu()` を一度も呼ばない。
- 下位層は完成済み:
  - PHY 入力: `include/ocudu/phy/upper/signal_processors/prs/prs_generator_configuration.h:18-69`
  - FAPI PDU: `include/ocudu/fapi/p7/messages/dl_prs_pdu.h:17-28`、variant 登録 `include/ocudu/fapi/p7/messages/dl_tti_request.h:18`
  - FAPI builder: `include/ocudu/fapi/p7/builders/dl_prs_pdu_builder.h:13-92`、`dl_tti_request_builder.h:86-93 add_prs_pdu()`
  - FAPI→PHY: `lib/fapi_adaptor/phy/p7/pdu_translators/prs.cpp:12-30`、`fapi_to_phy_fastpath_translator.cpp:257-270`
  - PHY 実行: `lib/phy/upper/downlink_processor_multi_executor_impl.cpp:190-219 process_prs()`、factory `lib/phy/upper/upper_phy_factories.cpp:1247-1248`
  - 容量: `include/ocudu/ran/slot_pdu_capacity_constants.h:40`（`MAX_PRS_PDUS_PER_SLOT=16`）、`:77`（DL 合計算入）
- 注: 依頼文アンカーの `fapi_to_phy_fastpath_translator.cpp:257` 等は上流 `dev` では `lib/fapi_adaptor/phy/p7/` 配下（サブディレクトリ `p7` 追加）に存在。内容は同一系統。

### A1) `prs_info`（sched_result への PRS 器）に要るフィールド [確定]

FAPI `dl_prs_pdu`（`include/ocudu/fapi/p7/messages/dl_prs_pdu.h`）と PHY `prs_generator_configuration` への変換（`lib/fapi_adaptor/phy/p7/pdu_translators/prs.cpp`）から逆算:

| `prs_info` フィールド | FAPI `dl_prs_pdu` 消費点 | PHY `prs_generator_configuration` 消費点 | タグ |
|---|---|---|---|
| `const bwp_configuration* bwp_cfg`（scs/cp 供給、`csi_rs_info.bwp_cfg` と同型） | `scs` (:18), `cp` (:19) | `cp` (prs.cpp:18)、`slot` の scs (prs.cpp:17) | [確定] |
| `uint16_t n_id_prs`（0..4095, dl-PRS-SequenceID） | `nid_prs` (:20) | `n_id_prs` (prs.cpp:19) | [確定] |
| `prs_comb_size comb_size` | `comb_size` (:21) | `comb_size` (prs.cpp:20) | [確定] |
| `uint8_t comb_offset` | `comb_offset` (:22) | `comb_offset` (prs.cpp:21) | [確定] |
| `prs_num_symbols duration`（2/4/6/12） | `num_symbols` (:23) | `duration` (prs.cpp:22) | [確定] |
| `uint8_t start_symbol` | `first_symbol` (:24) | `start_symbol` (prs.cpp:23) | [確定] |
| `crb_interval crbs`（StartPRB + Bandwidth, 24..276 RB・4 の倍数） | `crbs` (:25) | `prb_start`/`freq_alloc` (prs.cpp:24-25) | [確定] |
| `std::optional<float> power_offset_db`（PRS EPRE / SSS EPRE） | `prs_power_offset_db` (:26) | `power_offset_dB`（未設定時 0.f, prs.cpp:26-27） | [確定] |
| プリコーディング指定（`pm_index` 相当） | `precoding_and_beamforming` (:27) | `precoding = pm_repo[pm_index]` (prs.cpp:28-29) | [確定] |

重要な注記:
- **muting / repetition factor / resource set ID は FAPI にも PHY にも存在しない** [確定]。本コードでは「1 オケージョン = 1 PDU」に還元されている。muting/repetition はスケジューラ側で「どのスロットで PDU を出す/出さない」ロジックに変換すべきで、`prs_info` には載せない設計が CSI-RS（`csi_rs_info` に period/offset が無く `cell_configuration` 側が保持）と整合。
- 周期・オフセットは cell config 側（CSI-RS では `lib/scheduler/config/cell_configuration.h:49,51` の `zp_csi_rs_list`/`nzp_csi_rs_list`）に置く。PRS も同様に period/offset 付き PRS resource list を cell config に持たせる。[確定]
- `power_offset_db` は Tier 0 では固定値で可。実機の電力較正は [要実機確認]。

### A2) スケジューラ occasion 決定の挿入点 [確定]

CSI-RS の毎スロット emit 経路（CSI-RS 対応表）:

| 段階 | file:line | 内容 |
|---|---|---|
| cell run_slot からの呼び出し | `lib/scheduler/cell_scheduler.cpp:89` `csi_sch.run_slot(res_grid[0])` | SSB(:86) 直後、SI/RA/UE より前 |
| スケジューラ本体 | `lib/scheduler/common_scheduling/csi_rs_scheduler.cpp:67` `run_slot()` | DL 無効スロット早期 return (:72) |
| 周期/offset 判定 (ZP) | `csi_rs_scheduler.cpp:79` `(res_grid.slot - *offset) % *period == 0` | **occasion 判定の本体** |
| 結果 push (ZP) | `csi_rs_scheduler.cpp:80` `res_grid.result.dl.csi_rs.push_back(cached_csi_rs[i])` | |
| 周期/offset 判定・push (NZP) | `csi_rs_scheduler.cpp:88-89` | |
| PDU の事前計算キャッシュ | `csi_rs_scheduler.cpp:56-65`（ctor） | PRS も precompute 推奨 |
| 結果クリア | `lib/scheduler/cell/resource_grid.cpp:334` `result.dl.csi_rs.clear()`（`slot_indication()` :313-323 冒頭） | 隣に `result.dl.prs.clear()` を追加 |

**PRS 挿入箇所**: 新設 `prs_scheduler::run_slot` を `cell_scheduler.cpp:89` 付近（`csi_sch` の隣、UE スケジューラより前）で呼び、`(slot - offset) % period == 0` を PRS resource ごとに判定 → `res_grid.result.dl.prs.push_back()`。[確定]

### A3) MAC→FAPI 変換の追加点 [確定]

| # | 追加物 | CSI-RS counterpart (file:line) |
|---|---|---|
| 1 | `dl_sched_result` に `static_vector<prs_info, MAX_PRS_PDUS_PER_SLOT> prs` | `sched_result.h:35` / 型 `csi_rs_info.h` |
| 2 | `lib/fapi_adaptor/mac/p7/pdu_translators/prs.{h,cpp}` 新規（`convert_prs_mac_to_fapi`） | 同 dir `csi_rs.{h,cpp}`（cpp 全 24 行） |
| 3 | MAC 側 CMakeLists へ prs.cpp 追加 | `lib/fapi_adaptor/mac/p7/pdu_translators/CMakeLists.txt:8-15`（PHY 側 `phy/p7/pdu_translators/CMakeLists.txt:11` には既に有り＝非対称） |
| 4 | `add_prs_pdus_to_dl_request()` + 呼び出し | `mac_to_fapi_fastpath_translator.cpp:85-90`（関数）+ `:147`（呼び出し） |
| 5 | DL_TTI 内 PDU 順序（SSB→CSI-RS→PRS→PDSCH）の index 整合（`dl_res->csi_rs.size()` が PDSCH 変換の index 起点 `:155 nof_csi_pdus`） | `mac_to_fapi_fastpath_translator.cpp:143-157` |
| 6 | capacity check（≤ `MAX_PRS_PDUS_PER_SLOT`） | `slot_pdu_capacity_constants.h:40,77`（定数は既存） |
| 7 | include 追加 | `mac_to_fapi_fastpath_translator.cpp:6` |

補足:
- **FAPI validator 層は本リポジトリに存在しない**（`lib/fapi/validators/` 無し、FAPI は header-only）。妥当性検証は PHY upper（`include/ocudu/phy/upper/downlink_processor.h:199`、`prs_generator_validator.h:23`）が担い PRS 用実装済み → FAPI 側 validator 追加不要。[確定]
- MAC 結果は `mac_dl_sched_result::dl_res`（`const dl_sched_result*`、`include/ocudu/mac/mac_cell_result.h:59-65`）のポインタ渡しなので、`prs_info` を `dl_sched_result` に足せば MAC 層の構造体変更は不要。[確定]
- **precoding の非対称点**: PHY 側で CSI-RS は単位行列を自己生成（`phy/p7/pdu_translators/csi_rs.cpp:47-48 make_identity`）するが、**PRS は pm_repo を引く**（`prs.cpp:28-29`）。よって `convert_prs_mac_to_fapi` は有効な `pm_index` を設定する必要がある。参考パターン: PDCCH の `pdu_translators/pdcch.cpp:41-49`（`pm_mapper.map → set_pmi`）。translator は `pm_mapper` を既に保持（`mac_to_fapi_fastpath_translator.cpp:32,36`）。単一ポート・ワイドバンドで `pm_index=0` 固定で足りるかは [要実機確認]（PHY テストは `pmi_codebook_one_port` を使用: `tests/unittests/fapi_adaptor/phy/p7/pdu_translators/dl_prs_pdu_test.cpp:19-20`）。

### A4) cell config 配管（RRC 側と DU-local 送信設定の分離）

CSI-RS のエンドツーエンド経路（テンプレート）[確定]:

1. apps 構造体: `du_high_unit_csi_config`（`apps/units/flexible_o_du/o_du_high/du_high/du_high_config.h:928-946`、cell へ `:1167-1168`）
2. CLI: `du_high_config_cli11_schema.cpp:626,628,650`
3. validation: `du_high_config_validator.cpp:354-378`
4. apps→du_cell_config: `du_high_config_translators.cpp:302 fill_csi_resources()` → `ran_cell_config.init_bwp.csi`
5. du→scheduler: `lib/du/du_high/du_manager/converters/scheduler_configuration_helpers.cpp:55` **`sched_req.ran = du_cfg.ran;`（丸ごと搬送 → PRS 用の追加コード不要）**
6. scheduler: `lib/scheduler/config/cell_configuration.cpp:17-33,49-50`（`make_zp/nzp_csi_rs_list`）、メンバ `cell_configuration.h:49,51`
7. RRC/ASN.1（UE-dedicated のみ）: `asn1_csi_meas_config_helpers.cpp` は RRC Reconfiguration 用（SIB ではない）

PRS 有効化に足す設定（**DU-local 送信設定のみ。RRC 側は不要**）[確定]:

| 層 | 追加内容 | CSI-RS 対応点 |
|---|---|---|
| apps 構造体 | `du_high_unit_prs_config`（enabled, n_id_prs, comb size/offset, symbols, RB alloc, period/offset, power_offset） | `du_high_config.h:928-946,1167-1168` |
| CLI/YAML | `--prs_*` オプション群 | `du_high_config_cli11_schema.cpp:626-650` ほか |
| validator | comb/symbol 妥当性（既存 helper `prs_valid_num_symbols_and_comb_size` `include/ocudu/ran/prs/prs.h:22-26` 利用可） | `du_high_config_validator.cpp:354-378` |
| translator | apps→`ran_cell_config` へ `std::optional<prs_config>` 設定（`init_bwp` でなく cell 直下が自然 — PRS は BWP 非依存のセルレベル信号） | `du_high_config_translators.cpp:302` / `include/ocudu/scheduler/config/ran_cell_config.h:21-44` |
| du→sched | **追加コード不要**（`:55` の丸ごと代入） | `scheduler_configuration_helpers.cpp:55` |
| scheduler | `cell_configuration` に `prs_list`（precomputed）追加 | `cell_configuration.cpp:17-33,49-50` |

RRC/ASN.1 側 [確定]:
- **DL-PRS 用の RRC ASN.1 構造体は存在しない**。`asn1/rrc_nr` で prs にヒットするのは全て Sidelink PRS (SL-PRS, R18)（例: `include/ocudu/asn1/rrc_nr/dl_ccch_msg_ies.h:642,1871-1937`）で DL 測位 PRS とは無関係。DL-PRS 設定は LMF→UE を LPP で配布するため、**gNB 側 RRC/ASN.1 追加はゼロ**。

### A5) 衝突 / レートマッチ [確定]

CSI-RS の PDSCH 衝突回避は 3 層構造:

| 機構 | file:line | 内容 |
|---|---|---|
| (a) スケジューラ grid 予約 | **無し** | `csi_rs_scheduler::run_slot`（`csi_rs_scheduler.cpp:67-92`）は `fill()` を一切呼ばない |
| (a') MCS 減算による近似救済 | `lib/scheduler/ue_scheduling/ue_cell_grid_allocator.cpp:41-56`（`:45`） | CSI-RS スロットで MCS を経験的に下げるのみ |
| (b) UE 向け rate-match 通知 | ZP-CSI-RS を RRC で設定（`cell_configuration.h:49`）。PHY で ZP は no-op（`fapi_to_phy_fastpath_translator.cpp:191-192`） | |
| (c) PHY の PDSCH RE レートマッチング | `fapi_to_phy_fastpath_translator.cpp:149 generate_csi_re_pattern_list` → `:180` → `:228 convert_pdsch_fapi_to_phy(..., csi_re_patterns, ...)` | PHY が CSI-RS RE を避けて PDSCH マッピング |

**PRS の現状と方針**:
- fastpath translator の PRS 分岐（`:257-270`）は **PRS の RE パターンを `csi_re_patterns` 相当に加えない** → 同一スロットで PDSCH と PRS が重なると PDSCH は PRS RE を避けず衝突する。[確定]
- **Tier 0/1 の推奨**: PHY レートマッチング拡張は不要。**スケジューラで PRS のシンボル×RB を `cell_slot_resource_grid::fill(grant_info)`（`lib/scheduler/cell/resource_grid.h:173`、SSB の使用例 `ssb_scheduler.cpp:80-81`）で予約**すれば、PDSCH アロケータの `collides()` チェック（`ue_cell_grid_allocator.cpp:264-267`）→ `fill`（`:291-293`）により PDSCH が自動回避する。comb による RE 単位の PDSCH 共存をしたい場合のみ PHY 側拡張（`:149/180/228` の PRS 版）が追加で必要 [要実機確認]（共存運用の要否次第、Tier 1 では不要）。
- ここが **CSI-RS テンプレートからの唯一の本質的逸脱点**: CSI-RS は grid 予約しないが、PRS は `fill()` が必須。

### A6) 最小縦串の線引き

**Tier 0（送信実在: 単一セル・単一 PRS resource・muting なし・LMF 不要）に必要な全変更** [確定]:

1. `include/ocudu/scheduler/result/prs_info.h` 新規 + `sched_result.h:35` 付近に `static_vector<prs_info, MAX_PRS_PDUS_PER_SLOT> prs;`
2. `lib/scheduler/cell/resource_grid.cpp:334` 付近に `result.dl.prs.clear();`
3. cell config 配管一式（A4 の表。apps → `ran_cell_config` → `cell_configuration.prs_list`）
4. 新規 `prs_scheduler`（`csi_rs_scheduler` 雛形 + **`dl_res_grid.fill()` 必須**）+ `cell_scheduler.h:89`/`cell_scheduler.cpp:89` への配線
5. MAC→FAPI: `convert_prs_mac_to_fapi` 新規 + `mac_to_fapi_fastpath_translator.cpp:147` 付近で `add_prs_pdu()` 駆動（pm_index 設定含む）

下位（FAPI PDU/builder/FAPI→PHY/PHY generator/容量）は全て既存流用。[確定]

**Tier 1（実 UE + LMF/LPP）で追加が要る分**:
- **gNB 側スケジューラ内部の追加差分は無し** [確定]。PRS 設定は LMF→UE の LPP assistance data で配布されるため、gNB の UE 向け RRC signalling 追加も不要。
- Tier 1 で本質的に必要なのは gNB 外: LMF の静的 PRS 設定と DU config の**手動整合**（n_id_prs, comb, period/offset, SFN タイミング基準）、および LPP フロー（タスク D）。整合ずれ・SFN0 基準・電力較正・実 UE の測定成立は [要実機確認]。
- 1b（複数 PRS resource set）を行う場合のみ、config/スケジューラを複数 resource 対応にする（構造は同じ、リストのループ）。muting は Tier 1 スコープ外のままで可。

---

## タスク B — Tier 1 の LMF 静的化に伴う NRPPa 迂回可否

### B1) NRPPa / F1AP positioning の実装状況 [確定]

`lib/nrppa/procedures/`（ディスパッチ: `lib/nrppa/nrppa_impl.cpp:203-225`）:

| Procedure | TS 38.455 | 分類 |
|---|---|---|
| `positioning_information_exchange_procedure` (.h:15-17) | §8.2.6 | **UL-SRS 中心**（応答は SRS positioning リソース構築、`positioning_information_exchange_procedure.cpp:309-318` の `prs` は SRS 空間関係の参照先） |
| `positioning_activation_procedure` (.h:15-17) | §8.2.9 | **UL-SRS 起動** |
| `measurement_procedure` (.h:18-20) | §8.5.1 | **UL-SRS 測定**（`prs_res_set_id` は測定結果 beam info のみ: `measurement_procedure.cpp:515-518`） |
| `trp_information_exchange_procedure` (.h:14-16) | §8.2.8 | TRP メタ情報 |
| `e_cid_measurement_initiation/termination_procedure` | §8.2.1 | E-CID、DL-PRS 非依存 |

依頼文の「DL-PRS は `f1ap_trp_information_exchange_procedure.cpp` のみ」は**不正確（拡張要）**: DL-PRS 型は (1) `f1ap_trp_information_exchange_procedure.cpp`（`asn1_to_dl_prs_muting_pattern` :39-40,:621-629、`prs_res_set_list` :176-252、`dl_prs_res_coordinates` 等 :401-441、`trpteg_item.dl_prs_res_set_id` :529-536 — いずれも **CU-CP 受信側の ASN.1 デコード**）、(2) `f1ap_positioning_measurement_procedure.cpp:514-515,741`、(3) `measurement_procedure.cpp:515-518`、(4) FAPI/PHY 実送信系、の 4 系統に分散。ただし **scheduler/apps に PRS は 0 件**という結論は不変。[確定]

**DU 側の決定的事実** [確定]: DU の F1AP positioning ハンドラ（`lib/f1ap/du/procedures/f1ap_du_*positioning*`、`f1ap_du_trp_information_exchange_procedure.cpp` 92 行）が呼ぶ `du_positioning_manager_impl.cpp` は:
- `request_positioning_info` → UE への **UL-SRS 設定**（`du_ue_positioning_info_procedure.cpp:21,24,40-58`）
- `request_positioning_measurement` → **UL-SRS の MAC 測定**（`du_positioning_measurement_procedure.cpp:53,105-131`: `ul_srs_rsrp`/`ul_rtoa`）
- `update_trp_info`（`:50-62`）は `trp_id/pci/cgi/arfcn` **のみ**。`trp_info_to_asn1`（`lib/f1ap/asn1_helpers.cpp:726~`）も同様で、**DL-PRS resource set・座標・muting は一切埋めない**。

→ NRPPa/F1AP TRP 手続きを起動しても DU は DL-PRS 設定を LMF に供給**できない**。LMF への PRS 供給は静的投入以外に選択肢が無い（現実装では）。

### B2) NGAP↔NRPPa 転送と必須性の判定 [確定]

コールチェーン: AMF → NGAP（`lib/ngap/ngap_impl.cpp:438-442` 分岐、`:1118-1143`/`:1146-1153` DL NRPPa transport）→ CU-CP（`lib/cu_cp/cu_cp_impl.cpp:1269-1277`; NRPPa エンティティは `cu_cp_impl.cpp:143` で**無条件生成**）→ NRPPa ディスパッチ（`nrppa_impl.cpp:203-225`）→ F1AP（`nrppa_adapters.h`、`trp_information_exchange_routine.cpp:46-51`）→ DU。UL 応答: `cu_cp_impl.cpp:1388-1421` → NGAP `:1476-1518`/`:1526-1540` → AMF/LMF。

UE-assisted DL-TDOA では、UE への測定指示・アシスタンスデータ・報告は全て LPP（NAS 透過、B3）で運ばれ、上記 NRPPa 経路の状態に **UE-facing パスは一切依存しない**。

> **一文結論 [確定]: LMF が PRS 設定を静的保持する UE-assisted DL-TDOA では、UE の DL-PRS 測定・LPP 報告は NAS 透過経路のみで完結するため、NRPPa は Tier 1 のクリティカルパスから外せる。**

（補足: NRPPa エンティティを止める設定フラグは無く常時生成される（`cu_cp_impl.cpp:143`、apps 設定はログレベル `--nrppa_level` のみ: `cu_cp_unit_config_cli11_schema.cpp:31`）が、LMF が NRPPa を送らなければ起動されないだけなので放置で害はない。[確定]／LMF 側が NRPPa を送ってきた場合の無害性は [要実機確認]）

### B3) LPP の NAS 透過確認 [確定]

- RRC 透過: UL は `lib/rrc/ue/rrc_ue_message_handlers.cpp:328-343 handle_ul_info_transfer`（`ded_nas_msg.copy()` を無検査で NGAP へ）、DL は `:354-375 handle_dl_nas_transport_message`（`nas_pdu` を無検査で SRB2/SRB1 送出）。NAS ペイロード（= LPP を内包）の解釈は皆無。
- NGAP NAS Transport 配線済: DL `ngap_impl.cpp:405`、UL `:193-237`。
- NGAP NRPPa PDU transport（**NAS transport とは別手続き**）も B2 の通り両方向で結線済み。両者ともペイロードは `byte_buffer` 透過。

→ **UE↔LMF の LPP は gNB を素通りする。gNB 側の変更ゼロで Tier 1 の LPP フローが成立する**。[確定]（実 UE の NAS 上 LPP 挙動は [要実機確認]）

---

## タスク C — 単一 O-RU での RSTD 報告機構検証（1b）の可否

### C1) 判定: **構成可能** [確定]（下位層。上位層実装が前提）

- `prs_generator_configuration`（`prs_generator_configuration.h:18-69`）は **PRS リソース単位の完全独立構造**: `slot`(:20), `cp`(:21), `n_id_prs`(:28, 0..4095), `comb_size`(:33), `comb_offset`(:38), `duration`(:43), `start_symbol`(:48), `prb_start`(:55), `freq_alloc`(:64), `power_offset_dB`(:66), `precoding`(:68)。**serving cell PCI への参照は皆無**。
- 系列生成は TS 38.211 §7.4.1.7.2 の c_init（`prs_generator_impl.cpp:42-70`、入力は `n_id_prs` と slot index のみ、PCI 不使用）→ 異なる `n_id_prs` で独立系列。[確定]
- 1 スロット複数 PDU: `MAX_PRS_PDUS_PER_SLOT=16`（`slot_pdu_capacity_constants.h:40`）、builder `add_prs_pdu()` は呼ぶ度 `emplace_back`（`dl_tti_request_builder.h:86-92`）、FAPI→PHY はループ処理（`fapi_to_phy_fastpath_translator.cpp:182,257-267`、実行 `:348-350`）、PHY 側 `prs_list` 容量 16（`downlink_processor_multi_executor_impl.h:181`）。[確定]
- PHY validator（`prs_generator_validator_impl.h:17-73`）は単一 config 単位の検査のみで、リソース間関係・本数の制約なし。[確定]
- RE 衝突: グリッド書込みは**単純代入 = 後勝ち上書き**（`prs_generator_impl.cpp:105`）。2 リソースに**異なる comb_offset か異なるシンボル/RB** を割れば共存可（例: comb-2 で offset 0/1）。同一 RE 重畳は不可。[確定]
- 同一 PHY/RU 由来のため真の TDOA ≈ 0（ケーブル長差のみ）— 機構検証(1b)には支障なし。**実 UE が同一 PCI/ARFCN 配下の 2 TRP エントリ（LPP アシスタンスデータ上は別 TRP として通知）を受理し RSTD を報告するか**は [要実機確認]（UE 実装依存。QXDM での RSTD ログ確認が判定手段）。

---

## タスク D — hug0lin Open5GS フォーク調査（別リポジトリ）

**前提修正が必要**: 「LPP-over-NAS 輸送路の substrate として使う」という計画前提が成立しない。

### D1a) フォーク特定 [確定]

- `https://github.com/hug0lin/open5gs`、LMF 作業ブランチ **`LMF2`**、HEAD `6ac05cea4ea68ac3ae084eb5834e4a9be99756d2`（2025-12-15）。フォーク独自コミットは 4 個のみ。
- ベース: upstream v2.7.0 + 588 コミット時点（2025-11-19 分岐、調査時点で upstream から 236 コミット遅れ）。
- upstream への PR: open5gs/open5gs#4208（未マージ、メンテナが分割を要求、stale）[未確認]。

### D1b) LPP ASN.1 コーデック: **存在しない** [確定]

- 追加されたのは `lib/asn1c/nrppa_ecid/`（**NRPPa r17.2.0** コーデック、1010 ファイル。原本 `lib/asn1c/support/nrppa-r17.2.0/NRPPA-*.asn`）のみ。**TS 37.355 由来の LPP コーデックは皆無**（A-GNSS すら無い）。
- `NR-DL-TDOA-ProvideAssistanceData` / `NR-DL-TDOA-RequestLocationInformation` / `NR-DL-TDOA-ProvideLocationInformation` / `NR-DL-PRS-AssistanceData` / `NR-DL-TDOA-SignalMeasurementInformation` は**全て不存在**。ヒットするのは NRPPa 側 IE（`lib/asn1c/nrppa_ecid/DL-PRS-ResourceSet-List.c` 等、未使用）と OpenAPI 列挙値 `DL_TDOA`（`lib/sbi/openapi/model/positioning_method_any_of.c:9`）のみ。
- LMF の測位法は文字列 `"CELLID"`/`"ECID"` の 2 択（`src/lmf/location-determination.c:38,83,112-122`）。

### D1c) AMF 改修 = NRPPa トランスポートのみ。**LPP NAS 転送は未改修** [確定]

- 追加は LMF↔AMF↔gNB の NRPPa 経路: LMF→AMF `N1N2MessageTransfer` の n2 class=NRPPa（`src/lmf/namf-build.c:29-160`）→ AMF が不透明オクテット列のまま NGAP `DownlinkUEAssociatedNRPPaTransport` へ（`src/amf/namf-handler.c:70-75,1986~`、`src/amf/ngap-build.c:1918-2012`）→ UL は `src/amf/ngap-handler.c:2903-3029` からコールバック URI へ HTTP POST。
- **`lib/nas` の変更ゼロ**。`OGS_NAS_PAYLOAD_CONTAINER_LPP`（=3, `lib/nas/5gs/types.h:585`）は定義のみで参照ゼロ。AMF の UL NAS TRANSPORT は PDU セッション ID 必須（`src/amf/gmm-handler.c:1215-1233`）かつ payload container switch は `N1_SM_INFORMATION` のみ、**それ以外は 5GMM STATUS で拒否**（`:1237-1238,1737-1744`）。DL NAS TRANSPORT 生成も N1 SM のみ（`src/amf/nas-path.c:779,843,904`）。→ **LPP ペイロードは現状 AMF で弾かれる**。
- 作者は Discussion open5gs#4200（2026-03-10）で「A-GNSS 向け LPP・NRPPa OTDOA/DL-TDOA/UL-TDOA を実装中、大幅リベース要、別 PR 準備中」と表明 — **未公開** [未確認]。

### D1d) 自作すべき DL-TDOA LPP 範囲（絞り込み結果）

流用可: LMF NF 骨格・NRF 登録・`Nlmf_Location determine-location` 入口（`src/lmf/lmf-sm.c:113-118`）・セル DB パターン（`src/lmf/cell-database.c`）・「不透明ペイロード転送」の設計手本としての NRPPa 経路・asn1c 生成手順の先例。

新規構築（全て）:
1. **LPP NAS トランスポート**（UE↔AMF↔LMF）: AMF UL 側で payload container type LPP の受理（PDU セッション ID 非必須分岐）+ LMF への転送、DL 側の DL NAS TRANSPORT builder LPP 対応、LPP トランザクション/routing identifier 管理。ペイロード不透明に作れば LPP メッセージ種別非依存の汎用配管にできる。
2. **LPP ASN.1 コーデック**（TS 37.355 から asn1c 生成; DL-TDOA + Common IEs のサブセットで可）。
3. **LMF 側 LPP ロジック**: Capabilities 交換、`ProvideAssistanceData`（静的 PRS 設定 → `NR-DL-PRS-AssistanceData`; DU config と手動整合）、`RequestLocationInformation` / `ProvideLocationInformation`（`nr-RSTD` / `nr-DL-PRS-RSRP` 抽出）。Tier 1 では位置算出ソルバ不要（測定値取得まで）。

---

## Tier 0 / Tier 1 の線引き（総括）

| 範囲 | 内容 |
|---|---|
| **Tier 0（送信実在、LMF 不要）で完結** | A6 の 5 点（prs_info + sched_result / resource_grid clear / cell config 配管 / prs_scheduler(occasion+fill) / MAC→FAPI 変換）。検証はスペアナ or QXDM の PRS 検出。NRPPa・LPP・LMF・RRC・SIB 一切不要 |
| **Tier 1 で追加（実 UE, LMF/LPP 必須）** | gNB 側コード追加**なし**（LPP は NAS 素通り [確定]）。追加は gNB 外: LMF の LPP スタック（タスク D の 1〜3 は全て新規）、LMF 静的 PRS 設定と DU config の手動整合、UE CarrierConfig。1b を行う場合のみ config/スケジューラの複数 resource 対応 |
| **Tier 1 スコープ外のまま** | muting、複数 TRP 幾何、位置算出、NRPPa 動的 PRS 交換、PDSCH との RE 単位共存（PHY レートマッチ拡張） |

## Guard rails への回答

1. **SIB への漏れ防止** [確定]: SIB packer（`lib/du/du_high/du_manager/converters/asn1_sys_info_packer.{h,cpp}`、ヘッダ :24-46）は `du_cfg.si` 系のみを参照し、csi/prs ヒット 0 件。**PRS 設定を `du_cfg.ran`（`ran_cell_config`）配下にのみ置き `du_cfg.si` に入れない**限り、構造的に SIB に混入しない。
2. **既存 CSI-RS/PDSCH/SSB への副作用分離**: PRS は (a) 独立した `prs_scheduler` + 独立コンテナ `result.dl.prs`（既存コンテナ・関数を改変しない）、(b) grid `fill()` で予約（SSB と同じ流儀; PDSCH は既存の `collides()` 経由で自然回避）、(c) MAC→FAPI は独立関数追加のみ。唯一の相互作用点は DL_TTI 内 PDU 順序の index 計算（A3 #5）と、PRS スロットでの PDSCH 容量減（設計上意図した効果）。PRS 無効（config 不在）時は全経路が no-op。
3. **atomic MR 4 分割の評価** [確定]: 妥当。ただし依存構造上 **MR2（sched_result 器）が MR3/MR4 双方のブロッカ**なので、推奨順は **MR2 → (MR1 config ∥ MR4 MAC-FAPI) → MR3 occasion**。MR1 は「apps→du→scheduler cell_configuration までデータを運ぶが誰も読まない」で切れば独立マージ可能。E2E 検証は MR2+3+4 が揃って初めて可能。あるいは MR1+MR2 の 1 本化も可。
4. **テスト雛形** [確定]: FAPI builder テスト `tests/unittests/fapi/p7/builders/dl_prs_pdu_test.cpp`、FAPI→PHY テスト `tests/unittests/fapi_adaptor/phy/p7/pdu_translators/dl_prs_pdu_test.cpp`、helper `build_valid_dl_prs_pdu()`（`message_builder_helpers.h:35 / .cpp:255`）は既存。新規に必要なのは MAC→FAPI PRS 変換テストと `prs_scheduler` occasion テスト（`csi_rs_scheduler` 専用 unittest は現状 0 件のため作法は新規確立）。
