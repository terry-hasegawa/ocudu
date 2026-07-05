# OCUDU config→IE マッピング 3GPP 適合監査レポート(フェーズ1)

- 対象リビジョン: `91552ed`(branch: `claude/ocudu-config-ie-audit-dg2f6t`)
- 監査日: 2026-07-04
- 方針: 手書きの「設定 → RRC/NR パラメータ生成・検証」層を 3GPP TS と突き合わせ、
  範囲・既定・条件付き存在・値マッピング/スケールの取り違えを洗い出す。
  **自動生成コード(`lib/asn1/**` の pack/unpack)は対象外。**
- スコープ:
  - `lib/scheduler/config/`(serving_cell_config_factory / serving_cell_config_validator /
    scheduler_cell_config_validator / ran_cell_config_helper / sched_cell_config_helpers /
    time_domain_resource_helper / csi_helper / pucch_default_resource / pucch_resource_generator /
    pucch_guardbands / rlm_helper)
  - `lib/du/du_cell_config_validation.cpp`、`lib/du/du_high/du_manager/converters/`
    (asn1_rrc_config_helpers / asn1_csi_meas_config_helpers / asn1_sys_info_packer)
  - `lib/ran/**`(prach_helper / prach_configuration / prach_cyclic_shifts /
    prach_preamble_information / pusch_mcs / pdsch_mcs / pusch_uci_beta_offset /
    pusch・pdsch_antenna_port_mapping / tdd_ul_dl_config)
  - `lib/scheduler/support/`(dmrs_helpers / pdsch_dmrs_symbol_mask /
    pdsch・pusch_default_time_allocation / mcs_calculator)

## 1. サマリ

- **確定(コード上で立証済み)の不具合: 13 件**(うち XS: 9 件、S: 4 件。F13 は §6)+
  表記/コメントの軽微な不整合 5 件。**仕様テーブルの「値の転記ミス」は 1 件も検出されなかった**
  (MCS/CQI/β オフセット/DMRS 位置/デフォルト TD 割当/アンテナポート/PRACH NCS/
  PUCCH Table 9.2.1-1 は行単位で全一致)。問題は主に **(a) 検証関数の抜け・型・制御フロー、
  (b) 内部設定→ASN.1 変換の論理バグ、(c) 単位規約の不統一(F13)** に集中している。
- 上位 MR 候補(確定・XS・独立・後方互換):
  **[F1] PRACH format A2 の config index が正当なのに拒否される**、
  **[F2] PRACH/PUCCH 重複チェックの uint8_t オーバーフローで検証が素通り**、
  **[F3] NZP-CSI-RS ResourceSet の repetition が常に "on" でエンコードされる**。
  それぞれ `docs/audit/issue_draft_f1〜f3_*.md` に issue 下書きを用意済み。
- interop 影響が最大なのは F1(正当な設定の起動拒否)と F3(マルチビーム時の誤 RRC エンコード)。
  F2 は不正設定の見逃し(ランタイムで RA/UCI 劣化)。

## 2. Findings 一覧(確定)

凡例: 規模 XS=1〜数行 / S=十数行〜設計判断含む。確信度「確定」=コード上で立証済み。

| id | file:line | IE/パラメータ | 実装値/挙動 | TS 参照 | 差分の内容 | 影響(interop/機能) | 提案修正 | 規模 | 確信度 |
|----|-----------|---------------|-------------|---------|-----------|--------------------|----------|------|--------|
| F1 | `lib/ran/prach/prach_helper.cpp:18-20` | prach-ConfigurationIndex 許容範囲 | FR1 FDD `{0,108},{147,167},{198,256}`・FR1 TDD `{0,87},{110,133},{145,211}`(半開)で format A2 の行を除外 | TS 38.211 Table 6.3.3.2-2(117–136=A2)/ 6.3.3.2-3(87–109=A2) | テーブル実装(`prach_configuration.cpp`、機械抽出で範囲確認済み)と PHY 検出器閾値(A2 96 エントリ)は A2 対応済みなのに validator だけが拒否 | 正当な A2 設定でセル起動拒否。`find_valid_prach_config_index` も A2 を選ばない | FDD に `{117,137}`、TDD に `{87,110}` を追加(TDD は `{0,133}` へ統合可) | XS | 確定 |
| F2 | `lib/du/du_cell_config_validation.cpp:728` | msg1-FrequencyStart / msg1-FDM と PUCCH の重複検証 | `const uint8_t prach_prb_stop = msg1_frequency_start + msg1_fdm * prach_nof_prbs;` | TS 38.331 RACH-ConfigGeneric(msg1-FrequencyStart 0..274)/ TS 38.211 Table 6.3.3.2-1(12 PRB/RO) | 274+8×12 等で uint8_t が 256 を跨いで wrap し、直後の PUCCH 重複 CHECK が空振り(同関数内の BWP 境界チェックは unsigned で正常) | PRACH と PUCCH の周波数衝突設定が受理され、RA/UCI がランタイム劣化 | `unsigned` に変更(1 行) | XS | 確定 |
| F3 | `lib/du/du_high/du_manager/converters/asn1_csi_meas_config_helpers.cpp:261` | NZP-CSI-RS-ResourceSet.repetition | `out.repeat = cfg.is_repetition_on ? on : off;`(`is_repetition_on` は `std::optional<bool>`、has_value() ガード内なので常に真)→ 常に "on" | TS 38.331 NZP-CSI-RS-ResourceSet.repetition | 設定値(`csi_helper.cpp:492` は multi-beam で off を意図)を無視して常に on をエンコード | multi-beam(nof_beams>1、DU API 経由で到達可)で UE のビーム測定前提が崩れる。既定(単一ビーム)は偶然一致 | `*cfg.is_repetition_on` に修正(1 行) | XS | 確定 |
| F4 | `lib/du/du_high/du_manager/converters/asn1_rrc_config_helpers.cpp:717-727` | ssb-perRACH-OccasionAndCB-PreamblesPerSSB | `four`/`eight`/`sixteen` 分岐に return がなく、直後の無条件 `report_fatal_error`([[noreturn]])へ落下 | TS 38.331 RACH-ConfigCommon(four INTEGER(1..16) 等は正当) | `prach_helper::nof_ssb_per_ro_and_nof_cb_preambles_per_ssb_is_valid`(検証は正しい)を通過した正当な設定で DU がクラッシュ | 現状 CLI は `--nof_ssb_per_ro` を "1" に制限しており潜在(DU 設定 API 経由で顕在) | 各分岐に return 追加 | XS | 確定 |
| F5 | `lib/ran/prach/prach_helper.cpp:47-58` | zeroCorrelationZoneConfig | `zero_correlation_zone_is_valid()` が引数 zcz を一切検証しない(config index の有効性のみ確認) | TS 38.331 RACH-ConfigGeneric(zeroCorrelationZoneConfig INTEGER(0..15))/ TS 38.211 Table 6.3.3.1-5〜7 | 検証関数の名前・呼び出し意図(`du_high_config_validator.cpp:881`)に反して no-op。内部型は `uint16_t` で上限なし | CLI に Range(0,15) があるため潜在。lib API 直接利用では zcz>15 が SIB1 エンコードまで素通り | `zcz <= 15` チェック追加(+可能なら NCS reserved 判定) | S | 確定 |
| F6 | `include/ocudu/scheduler/config/pucch_resource_builder_params.h:383` + `lib/scheduler/config/pucch_resource_generator.cpp:62,467` | PUCCH-format1 timeDomainOCC | 多重容量計算が `nof_cyc_shifts × NOF_TD_OCC(=7)` 固定。一方 RB 数見積りは `format1_symb_to_spreading_factor(nof_syms)=⌊N/2⌋` を使用。生成側は `occ = mux_idx / nof_css` で 0..6 を割当 | TS 38.211 §6.3.2.4.1 Table 6.3.2.4.1-1/-2(OCC index < N_SF; hopping 時は hop あたりの N_SF) | occ_supported=true かつ nof_syms<14(または intra-slot hopping)で N_SF 以上の timeDomainOCC を持つ資源を生成し得る(見積りとパッキングの不整合はコード上明白) | 無効な OCC index の PUCCH F1 → UE が復調不可/挙動未定義。既定(occ_supported=false, 14sym)は安全 | 多重容量を spreading factor(hopping 考慮)に基づき計算 | S | 確定(不整合)/影響条件は要確認 |
| F7 | `lib/scheduler/config/csi_helper.cpp:95-101` | CSI-ResourcePeriodicityAndOffset | `find_valid_csi_rs_period()` が `find_if` の結果 `found_rit` でなく `*rit`(無検査の次候補)を return | TS 38.331 CSI-ResourcePeriodicityAndOffset | TDD 周期で割り切れる周期を探すのに、見つけた値でなく直後の値を返す論理バグ | 現行の「TDD 周期は 10ms の約数」制約下では最大周期が常に割り切れ早期 return するため実質到達不能(潜在) | `return *found_rit;`(1 行) | XS | 確定(到達性は低) |
| F8 | `lib/scheduler/config/rlm_helper.cpp:17-19` | RLM SSB パラメータ前提 | `type != ssb or type != ssb_and_csi_rs or ssb_params.has_value()` は恒真 → assert が機能しない | (内部整合) | 前提検証が dead code。欠落時は後段の `.value()` でクラッシュ | 診断品質のみ(挙動は同じくクラッシュ) | `(type != ssb and type != ssb_and_csi_rs) or has_value()` | XS | 確定 |
| F9 | `lib/scheduler/support/dmrs_helpers.h:232-236` | PUSCH DM-RS ports(DCI 0_1 整合) | dedicated PUSCH の `dmrs_ports` を常に {port0} 固定(`nof_layers` 引数を無視、コード内 TODO 明記) | TS 38.212 Table 7.3.1.1.2-9(rank2 → ports {0,1}) | DCI 0_1 の antennaPorts フィールド(`dci_builder` は rank に応じ正しく設定)と FAPI へ渡す dmrs_ports が rank≥2 で矛盾 | UL rank≥2 設定時(`--max_rank` は設定可)に PHY のチャネル推定前提が崩れる可能性。rank1 既定では影響なし | rank に応じた ports 設定(TODO の解消) | S | 確定(不整合)/on-air 影響は要確認 |
| F10 | `lib/ran/pucch/pucch_info.cpp:134,150,217` | PUCCH F3 E_UCI | `get_pucch_format3_E_total` に DM-RS 込みの総シンボル数を渡す箇所あり(隣接コードはデータシンボル数を使用) | TS 38.212 Table 6.3.1.4-1 / §6.3.1.2.1 | E が 1088 閾値を早めに跨ぎ、境界ケースで CRC を 11bit 過大見積り(保守側) | PRB 1 個過大確保程度。誤り側には倒れない | データシンボル数に統一 | XS | 確定(影響軽微) |
| F11 | `lib/scheduler/config/csi_helper.cpp:22-31` | CSI-FrequencyOccupation.nrofRBs | `max(4⌈n/4⌉, min(24, bwp))` → 24 PRB 未満の BWP で 24 未満(例: 11PRB BWP→12)を返す | TS 38.331 CSI-FrequencyOccupation(nrofRBs INTEGER(24..277)、最小 24) | ASN.1 下限違反の値を生成し得る | CORESET#0 が最小 24PRB を要するため 24PRB 未満のセルは実質構成不能 → 事実上到達不能(潜在) | `max(24, …)` へ(encode 側でクランプ) | XS | 確定(到達性は要確認) |
| F12 | `lib/scheduler/config/pucch_guardbands.cpp:29-41` | PUCCH ガードバンド計算 | 共通リソースは PRB index、専用リソースは CRB index で同一ビットマップに記入 | (内部整合) | 初期 UL BWP が CRB0 始まりでない場合に不整合/範囲外 | 現行構成(BWP=キャリア全幅、CRB0 始まり)では潜在 | どちらかの索引系に統一 | S | 確定(到達性は低) |

### 表記・コメントのみ(XS・無リスク)

| id | file:line | 内容 |
|----|-----------|------|
| C1 | `lib/ran/prach/prach_helper.cpp:170-176` | エラーメッセージの範囲表記が `[0, 837)` / `[0, 137)`(正しくは閉区間 `[0, 837]` / `[0, 137]`。チェック自体は正しい) |
| C2 | `lib/du/du_high/du_manager/converters/asn1_rrc_config_helpers.cpp:461` | PUCCH group hopping の fatal メッセージが "Invalid msg1-fdm field"(コピペ) |
| C3 | `lib/scheduler/support/pdsch/pdsch_dmrs_symbol_mask.h:39` | 参照が "TS38.211 Table 5.1.2.1.1-2"(正: TS 38.211 Table 7.4.1.1.2-3) |
| C4 | `lib/scheduler/support/pusch/pusch_default_time_allocation.h:18-19` | 参照が PDSCH 側の Table 5.1.2.1.1-2/-3(正: TS 38.214 Table 6.1.2.1.1-2/-3) |
| C5 | `include/ocudu/ran/pusch/pusch_antenna_ports_mapping.h:12` | 参照が "7.3.1.2.2-6..23"(正: TS 38.212 Table 7.3.1.1.2-6..23)。および `pusch_antenna_port_mapping.cpp:148,164` の assert 条件(`<7`)とメッセージ("lower than 9")の不一致(2 ポートの TPMI 上限は 5) |

## 3. 要確認(推測を含む・裏取りしてから判断)

| id | file:line | 内容 | 確認事項 |
|----|-----------|------|----------|
| Q1 | `lib/ran/prach/prach_helper.cpp:22` | FR2 の許容が B4(112–143)のみ。テーブルは A1/A2/A3/C0/C2 も実装済み | FR2 サポート方針(意図的な限定か)。F1 の MR からは除外 |
| Q2 | `lib/scheduler/config/csi_helper.cpp:492` + 646 | `repetition` は TS 38.331 上「L1-RSRP または 'none' レポートに紐づく set のみ設定可」。set0 は cri-RI-PMI-CQI レポートに紐づくのに常時設定 | presence 自体を落とすべきか(F3 修正とは独立の仕様解釈) |
| Q3 | `lib/scheduler/config/time_domain_resource_helper.cpp:136-144` | 生成する PDSCH type A の S=`min_pdsch_symbol` に上限なし(Table 5.1.2.1-1 は S≤3、S=3 は pos3 のみ)。SS が symbol0 始まりでない設定で S=4 以上を生成し得る | in-tree 既定では到達せず、共通リストは `du_cell_config_validation.cpp:190` の S≤3 チェックで捕捉。dmrs-TypeA-Position との連動チェックはどこにもない点をどう扱うか |
| Q4 | `lib/du/du_cell_config_validation.cpp:188-206` | PDSCH TD 検証に S+L≤14(TS 38.214 Table 5.1.2.1-1 の S+L 条件)がない | 追加チェックの要否(現行生成コードは violating 値を作らない) |
| Q5 | `lib/scheduler/support/dmrs_helpers.h:181` | `nof_symbols≤2 → CDM groups=1` を transform precoding 有無に関わらず適用(TS 38.214 §6.2.2 は TP 無効時のみ 1 を許容)。mapping type B 未対応のため現状到達不能 | type B 対応時に顕在化する箇所としてマークするか |
| Q6 | `lib/scheduler/support/pdcch_aggregation_level_calculator.cpp:54` ほか | `cqi_table_t::table4`(Rel-17 1024QAM)は設定構造体・ASN.1 変換までは受理されるがスケジューラは `report_fatal_error` | table4 の到達経路(現行 apps schema から設定可能か)と対応方針 |
| Q7 | `include/ocudu/ran/tdd/tdd_ul_dl_config.h:17` | 内部上限 `MAX_NOF_SLOTS_IN_TDD_PERIOD=320`(コメントは SCS 240kHz 前提)に対し ASN.1 は maxNrofSlots=80(参照 SCS≤120kHz)。`nrofDownlink/UplinkSlots>80` の encode 前検証はない | lib レベルの range 検証を足すか(apps 側で実質制限) |

## 4. 実装制限(仕様違反ではない・現状維持で可)

- `lib/du/du_cell_config_validation.cpp:543-546`: TDD 周期に「10ms(1 フレーム)の約数」を要求。
  TS 38.213 §11.1 の要求は「P(+P2)が 20ms を割り切る」であり、例えば単一パターン P=4ms は
  仕様上正当だが本実装は拒否(スケジューラ設計上の制限)。エラーメッセージも「not supported」で妥当。
- PDSCH TD type B の L∈{2,4,7} 制限(Rel-15 セット)、`get_prach_duration_info` の SCS 15/30/120 限定、
  FR1 TDD の prach index 256–262(Rel-16 ext、RRC の v1610 拡張フィールド未対応のため除外は正当)など。
- `mcs_calculator.cpp` の SNR→MCS テーブルはコード内 TODO で暫定と明記(qam64LowSE テーブルが
  qam64 のコピー、テーブル間で同一 MCS0 に 7.6dB の差、という内部不整合はあるが仕様表ではない)。

## 5. 確認済み・問題なし(行単位で照合)

以下は仕様表・条項と突き合わせて**一致を確認済み**(過剰指摘の抑制のため明記):

- **MCS/CQI/β**: TS 38.214 Table 5.1.3.1-1/-2/-3(PDSCH MCS、32 行×3 表)、Table 6.1.4.1-1/-2
  (transform precoding、q 係数処理含む)、TS 38.213 Table 9.3-1/9.3-2(β オフセット、
  reserved は配列サイズで排除)、CQI→MCS 対応(SE 等価対応 48 エントリ)。
- **DMRS**: TS 38.211 Table 7.4.1.1.2-3(PDSCH type A single-symbol、pos0〜pos3・l1=11/12 の
  additionalDMRS-DL-Alt 例外含む全セル)、type1/type2 の RE 数(6/4 per CDM group)と
  CDM group 上限(2/3)、fallback の CDM groups(L=2→1、その他→2、TS 38.214 §5.1.6.2)。
  PUSCH type A への同マスク流用も TS 38.211 Table 6.4.1.1.3-3 と等価(l1=11 固定)で正当。
- **デフォルト TD 割当**: TS 38.214 Table 5.1.2.1.1-2/-3(PDSCH default A、pos2/pos3 変換含む
  全 16 行)、Table 6.1.2.1.1-2/-3(PUSCH default A)、Table 6.1.2.1.1-4(j={1,1,2,3})。
- **アンテナポート**: TS 38.212 Table 7.3.1.2.2-1/-2/-3/-4(PDSCH、実装済み全行)、
  Table 7.3.1.1.2-2〜-6/-8〜-11(PUSCH、実装サブセット)。
- **PRACH**: TS 38.211 Table 6.3.3.1-5/-6/-7(NCS、reserved 含め完全一致)、
  Table 6.3.3.1-1/-2(preamble 長・CP 長・SCS)、Table 6.3.3.2-2/-3/-4 の実装行構成
  (機械抽出で行範囲確認)、root sequence index 上限(837/137、§6.3.3.1)、
  PRACH 開始スロット偶奇(§5.3.2)。
- **PUCCH**: TS 38.213 Table 9.2.1-1(全 16 行+r_PUCCH→PRB/CS 写像、§9.2.1 両分岐)、
  F0/F1/F2/F3/F4 の symbol/PRB/code-rate 範囲と最大ペイロード計算
  (§9.2.5.1/9.2.5.2、TS 38.212 §6.3.1.2.1 の CRC 閾値含む)、F3 PRB 数の 2^a·3^b·5^c 制約、
  F3 DM-RS シンボル数(TS 38.211 Table 6.4.1.3.3.2-1)、pucch-ResourceCommon の
  F0={0..2}/F1={3..15} 分岐、maxCodeRate 列挙値。
- **CSI**: CSI-ResourcePeriodicityAndOffset / CSI-ReportPeriodicityAndOffset の列挙値、
  TRS 制約(TS 38.214 §5.1.6.1.1)、TS 38.211 Table 7.4.1.5.3-1 の row 1/2/3/4 使用と
  CDM/density/fd_alloc ビット幅、CSI-IM パターン(§7.4.1.5.3)、powerControlOffsetSS 列挙。
- **RLM**: TS 38.213 §5 Table 5-1 の N_RLM(L_max 4/8/64 → 2/4/8)、リソース候補条件。
- **電力制御/RACH 既定値**: p0-nominal(-90)/p0-NominalWithGrant(-76)/
  p0-NominalWithoutGrant(-76)/P0-PUSCH-AlphaSet p0=0 / msg3-DeltaPreamble=6 /
  preambleReceivedTargetPower=-100 は範囲・偶数条件を満たす(範囲+偶数チェックは
  apps CLI schema 層に実装済み=#423 系対応)。msg3_delta_power は TS 38.213 Table 8.2-2 の
  {-6..8, 偶数} を lib 層で検証済み。ra-ResponseWindow=10·2^µ slots(=10ms、§8.2 上限内)。
- **TDD**: dl-UL-TransmissionPeriodicity の slots→ms→列挙変換(v1530 ms3/ms4、非拡張
  フィールド無視の規定含む)、パターン境界計算(special slot の DL/UL symbol 位置、
  §11.1 のパターン構造)、maxNrofSymbols-1=13 の構造体注記。
- **その他**: SLIV(TS 38.214 §5.1.2.1 / TS 38.213 §12、N/2 境界)、k0/k1/k2 の範囲
  (≤15/1..15/≤11 ⊂ ASN.1 範囲)、PDSCH TD 検証の S/L 条件(Table 5.1.2.1-1、type B は
  Rel-15 セット)、CORESET duration=3 と dmrs-TypeA-Position の連動(TS 38.211 §7.3.2.2)、
  k0/k2 の条件付き presence(absent=0 規定)、SearchSpace duration の presence(≠1 のみ)。

## 6. SIB1/SI packer(asn1_sys_info_packer)監査

観点別サブ監査がセッション上限で中断したため、本節は主担当による重点確認
(値スケール・enum 変換・presence)の結果。SIB2 以降の SIB 内容 packing(約 600 行)は
未監査(§8 参照)。

### 追加 finding

| id | file:line | IE/パラメータ | 実装値/挙動 | TS 参照 | 差分の内容 | 影響 | 提案修正 | 規模 | 確信度 |
|----|-----------|---------------|-------------|---------|-----------|------|----------|------|--------|
| F13 | `include/ocudu/ran/sib/system_info_config.h:17-19` + `apps/.../du_high_config.h:1103-1104` + `du_high_config_cli11_schema.cpp:2427-2431` + `asn1_sys_info_packer.cpp:349` | SIB1 cellSelectionInfo.q-RxLevMin | 3 層のコメント/ヘルプが「in dBm」と表記しつつ、値域は `(-70..-22)`(=IE のフィールド域)で、packer は無変換で ASN.1 に代入 | TS 38.331 Q-RxLevMin(INTEGER(-70..-22)、実際の値 = フィールド×2 dBm) | dBm なら値域は [-140,-44]・要 /2 変換のはず。同一リポジトリ内の SIB2/セル再選択系の同名オプションは dBm 入力([-140,-44] 偶数チェック)+ `/2` 変換で正しく実装されており、単位規約が二重化 | 設定者が「-70 dBm」と信じて設定 → 実際は Qrxlevmin=-140 dBm として報知(サイレントに 2 倍緩い選択条件)。IE 値としては常に正当なので protocol violation ではなく運用罠 | 最小修正: 表記を「IE 値(実際 = 2×値 dBm)」に統一(XS・挙動不変)。単位を dBm に揃える案は既存設定の意味が変わるため要判断 | XS(doc)/S(単位変更) | 確定(不整合) |

### 確認済み・問題なし(SIB packer)

- **pagingCycle / nAndPagingFrameOffset**(`asn1_sys_info_packer.cpp:87-137`): rf32..rf256 の
  列挙対応、oneT は offset なし・halfT/quarterT/oneEighthT/oneSixteenthT で
  paging_frame_offset を設定する構造は TS 38.331 PCCH-Config の CHOICE 構造どおり。
- **firstPDCCH-MonitoringOccasionOfPO**(139-199 行): SCS 組合せ別の 8 variant を列挙対応。
- **n-TimingAdvanceOffset**(279-301 行): n0/n25600/n39936 を明示設定、`n13792` は
  フィールド absent(TS 38.331 の規定「absent 時 UE は FR1=25600 / FR2=13792 を適用」に
  より FR2 の 13792 は absent で表現)— 正しい条件付き presence。
- **tdd-UL-DL-ConfigurationCommon**(304-307 行): TDD 設定がある場合のみ present。
- **si-SchedulingInfo**(366-387 行): si-WindowLength(slots→s5..s1280)/
  si-Periodicity(radio frames→rf8..rf512)を `number_to_enum` で変換し失敗は assert。
  Rel-17 SchedulingInfo2(si-WindowPosition 含む)も並行生成。
- **cellAccessRelatedInfo**: PLMN/TAC/NCI(36bit)packing、cellReservedForOperatorUse。
- **connEstFailControl**: n1/s30/offset=1 は全て正当な列挙値・範囲。
- **q-QualMin**(350-351 行): Q-QualMin は IE 値=実際値 [dB](スケールなし)で、
  内部値域 (-43..-12)・表記「in dB」と整合 — F13 と異なり問題なし。

### 未検証(要別途確認)

- **ssb-PositionsInBurst の inOneGroup/groupPresence ビット順**(251-274 行):
  groupPresence は `set(nof_groups - i - 1, …)`(反転 index)、inOneGroup は
  `from_number(extract(...) << (8-L_max))` と、同じ「先頭ビット=index 0」を
  異なる書き方で表現しており、`bounded_bitset::extract`/ASN.1 `from_number` の
  ビット順規約まで掘らないと等価性を断定できない。実 UE との相互接続実績がある
  上流由来コードのため優先度は低いが、本監査では**未検証**として残す。
- SIB2〜SIB19 の内容 packing、`asn1_sys_info_packer_helpers.cpp`、NTN(sib19)変換。

## 7. 上位 issue/MR 候補(人間の選択待ち)

| 優先 | id | 1行要約 | 下書き |
|------|----|---------|--------|
| 1 | F1 | prach_config_index_is_valid が実装済み format A2 の index を拒否 | `docs/audit/issue_draft_f1_prach_a2_config_index.md` |
| 2 | F2 | PRACH/PUCCH 重複検証の uint8_t オーバーフローで検証素通り | `docs/audit/issue_draft_f2_prach_pucch_overlap_overflow.md` |
| 3 | F3 | NZP-CSI-RS ResourceSet repetition が常に "on" でエンコード | `docs/audit/issue_draft_f3_csi_repetition_encoding.md` |

次点(XS・確定): F4(ssb-perRACH 4/8/16 で fatal)、F7(csi period イテレータ)、F8(RLM assert)、
F13(q-RxLevMin 単位表記の統一、doc 修正案)、C1〜C5(表記)。これらも指示があれば同形式で
下書き化する。

- 1 finding = 1 MR、`upstream/dev` から `fix/<slug>`、最小 diff、`git commit -s`、
  target `dev`(フェーズ2: 人間の選択後に着手)。

## 8. 監査カバレッジと限界

- 本レポートの「確定」は全てコード読解で立証済み(該当行を明記)。仕様表の照合は
  主担当+観点別サブ監査(MCS/DMRS/TD 割当/PUCCH/CSI/SIB packer)で行単位に実施し、
  主担当が上位 finding の該当コードを再検証した。
- 3GPP 原文は本環境からネットワーク取得不可のため、条項・表番号は監査者の知識に依拠
  (F1 は実装内部の相互整合=テーブル実装/PHY 閾値/validator の矛盾として裏取りしており、
  表の行番号記憶に依存しない)。
- SIB/SI packer の観点別サブ監査はセッション上限で中断したため、§6 は主担当の重点確認のみ
  (value scale / enum 変換 / presence は確認済み、SIB2 以降の内容 packing は未監査)。
- 未監査/部分監査: `lib/ran/band_helper.cpp`(帯域表)、`pucch_info.cpp` の全数式導出
  (スポットのみ)、`asn1_csi_meas_config_helpers.cpp` の codebook/CQI 変換全分岐、
  NTN(sib19)変換、`asn1_ntn_config_helpers.cpp`、`asn1_sys_info_packer.cpp` の
  SIB2〜SIB19 内容 packing、ssb-PositionsInBurst のビット順(§6)。必要なら次フェーズで対象化。
