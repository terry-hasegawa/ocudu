# OCUDU ISAC sensing PoC — ソース調査 findings

**目的**: PHY TAP から UL の DMRS channel estimate（H）を read-only で取り出し、外部 process へ流す前段のソース調査。
**前提**: CPU PHY 前提（CUDA L1 は対象外）。コードは静的解析のみ、**無修正**。
**調査対象**: `terry-hasegawa/ocudu`、現在の checkout（branch `claude/zen-shannon-nec3m1`、`main`/dev 相当から分岐）。
**コードベース**: upstream srsRAN の fork。namespace は `ocudu`（`srslog→ocudulog` / `srsvec→ocuduvec` 等のリネームあり）。CPU PHY = `lib/phy/upper/**`。

**凡例**: ✅ = コードで該当行を確認 / ⚠️ = 推測・upstream 類推（未確認）

---

## 重要な前提事実 ✅

PUSCH の channel estimate（H）は、この fork では旧来の monolithic な `channel_estimate` テンソルではなく、**per-rx-port の `dmrs_pusch_estimator_results` インターフェース経由**で受け渡される。

- `channel_estimate` クラス（`include/ocudu/phy/upper/channel_estimation.h`）の使用箇所を grep した結果、**PUSCH 系ファイルには一切登場せず PUCCH 系のみ**。
- → PUSCH パスの H 取得は後述の `dmrs_pusch_estimator_results::get_symbol_ch_estimate()` が正。

---

## 1. UL rank-1 固定の config 項目

**結論**: rank-1 は **ハードコードではなく**、デフォルト値の積（`nof_antennas_ul=1` と UE-cap フォールバック `default_pusch_max_rank=1`）の帰結。項目名・構造は **upstream srsRAN と同一**（リネームは namespace/log/vec のみで、yaml 項目名・config struct field は不変）。

| 観点 | 場所 | 内容 |
|---|---|---|
| yaml/CLI 項目 ✅ | `apps/units/flexible_o_du/o_du_high/du_high/du_high_config.h:237` | `unsigned max_rank = 4;`（"Maximum rank. Limits the number of layers for PUSCH"）。**default 4**。`pusch:` ブロックの `max_rank`。 |
| UL アンテナ default ✅ | `apps/units/flexible_o_du/o_du_high/du_high/du_high_config.h:1085-1087` | `nof_antennas_dl = 1; nof_antennas_ul = 1;`（struct `du_high_unit_base_cell_config`）。**default 1 の UL アンテナが rank-1 の実質要因**。 |
| transform precoding ✅ | `apps/units/flexible_o_du/o_du_high/du_high/du_high_config.h:350` | `bool enable_transform_precoding = false;`（true なら強制 1 層） |
| **yaml→RAN セル変換（UL 層数の決定式）** ✅ | `apps/units/flexible_o_du/o_du_high/du_high/du_high_config_translators.cpp:715-718` | `pusch.max_nof_layers = enable_transform_precoding ? 1 : std::min({nof_antennas_ul, pusch_constants::MAX_NOF_LAYERS, max_rank});` → default で `min(1,4,4)=1` |
| PHY 上限定数 ✅ | `include/ocudu/ran/pusch/pusch_constants.h:30` | `MAX_NOF_LAYERS = 4` |
| **UE デフォルト（ハードコード literal）** ✅ | `lib/scheduler/config/serving_cell_config_factory.cpp:331` | `make_default_ue_bwp_cfg()` 内: `.tx_cfg = tx_scheme_codebook{.max_rank = 1, .codebook_subset = non_coherent}`、`:335` SRS `nof_ports = port1`。**ただし UE attach 時に下記で上書き**。 |
| 上書き（DU resource mgr）✅ | `lib/du/du_high/du_manager/ran_resource_management/du_pusch_resource_manager.cpp:88-99`（`apply_config`→`set_ul_mimo`）, `:158-169`（`select_max_rank`） | `select_max_rank = min(pusch.max_nof_layers, UEcap or default)` |
| **rank-1 フォールバック** ✅ | `lib/du/du_high/du_manager/ran_resource_management/ue_capability_summary.h:22` | `static constexpr unsigned default_pusch_max_rank = 1;`（UE-cap 不明時の値） |
| UE-cap 由来の rank ✅ | `lib/du/du_high/du_manager/ran_resource_management/ue_capability_manager.cpp:140-151` | `mimo_cb_pusch.max_num_mimo_layers_cb_pusch` を 1/2/4 に解釈 |

### upstream 類推の確認結果
ご指摘の codebook subset restriction / nof_layers 系は、この fork でも **upstream と同名で存在** ✅:
- `tx_scheme_codebook` / `tx_scheme_codebook_subset`（PUSCH の `codebookSubset` 相当, `non_coherent` 等）
- `pusch.max_nof_layers`
- 構造変更なし。

### rank を 1 に縛る最小設定
- `nof_antennas_ul: 1`（cell）または `pusch.max_rank: 1`（yaml）。
- さらに UE-cap 不明時は `default_pusch_max_rank=1` が自動で効く。
- 逆に rank>1 は `nof_antennas_ul≥2` かつ `max_rank≥2` かつ UE が capability 申告で可能（PHY 側も 4 層対応: `lib/ran/pusch/pusch_tpmi_select.cpp:665,670`）✅。

⚠️ 未確認: carrier aggregation の SCell 上書きパス（PCell 経路のみ確認）。

---

## 2. 4R 受信

**結論**: UL rx アンテナ = `nof_antennas_ul`（→ `nof_rx_antennas` → PHY `nof_rx_ports`）。**4 は設定可能**。アーキ上限はちょうど 4。4 を弾く制約は無し（`>4` は弾かれる）。

| 観点 | 場所 | 内容 |
|---|---|---|
| yaml/CLI 項目 ✅ | `apps/units/flexible_o_du/o_du_high/du_high/du_high_config.h:1087` / `.../du_high_config_cli11_schema.cpp:2324-2325` | `nof_antennas_ul`（"Number of antennas in uplink"）。**CLI に `->check(Range)` 無し → 上限チェック無し** |
| cell→PHY 配線 ✅ | `apps/units/flexible_o_du/o_du_high/du_high/du_high_config_translators.cpp:582` → `apps/units/flexible_o_du/o_du_low/du_low_config_translator.cpp:24-29,56,167` | `nof_antennas_ul` → `carrier.nof_ant` / `cell.nof_rx_antennas` → `upper_phy_factory_config.nof_rx_ports` |
| PHY chan-est dims ✅ | `lib/phy/upper/upper_phy_factories.cpp:824,883` | `...nof_rx_ports = config.nof_rx_ports;`（PUSCH chan estimator/processor の CE 次元） |
| アーキ上限 = 4 ✅ | `include/ocudu/ran/pusch/pusch_constants.h:35` | `MAX_NOF_RX_PORTS = MAX_NOF_LAYERS`（=4） |
| H バッファ容量 ✅ | `include/ocudu/phy/upper/channel_estimation.h:25` | `MAX_RX_PORTS = 16`（旧テンソル側。PUSCH パスの実上限は上記 4） |
| 0 のみ拒否 ✅ | `apps/units/flexible_o_du/o_du_high/du_high/du_high_config_validator.cpp:1310-1312` | "The number of UL antennas cannot be zero."（**max チェック無し**） |
| PHY validator ✅ | `lib/phy/upper/channel_processors/pusch/pusch_processor_validator_impl.cpp:21,50-55` | `nof_rx_ports>0` と `rx_ports.size() ≤ ce_dims.nof_rx_ports` のみ。**4 を弾く分岐は無し** |

⚠️ split_7.2 (OFH) 利用時は RU の UL/PRACH ポート list が cell アンテナ数以上であることが必要（`apps/.../ru_ofh_config_validator.cpp:104-115`）— 4R なら RU 側に 4 ポート宣言が要る、という整合性チェックのみ。出荷 yaml に `nof_antennas_ul: 4` の例は無い（最大 2）が、禁止項は無い。

---

## 3. PHY フック箇所（CPU PUSCH パイプライン）

### パイプライン（`pusch_processor_impl::process` 起点）✅

1. `lib/phy/upper/channel_processors/pusch/pusch_processor_impl.cpp:170-192` `process()`
   `dmrs_pusch_estimator::configuration ch_est_config` を構築（`slot, scaling, c_prefix, symbols_mask, rb_mask, first_symbol, nof_symbols, rx_ports`）し `estimator.estimate(notifier, grid, ch_est_config)` を呼ぶ（**非同期**、完了で notifier 発火）。

2. `lib/phy/upper/signal_processors/pusch/dmrs_pusch_estimator_impl.cpp:51-66` `estimate()`
   **rx port ごとに** `ch_estimator[i_port]->compute(grid, i_port, temp_symbols, est_cfg)` を実行し、結果 `port_channel_estimator_results&` を `ch_est_result[i_port]` に格納（`:55`）。全 port 完了で `notifier.on_estimation_complete(*this)`（`:57-58`）。`est_cfg.scs = to_subcarrier_spacing(config.slot.numerology())`（`:43`）。

3. `lib/phy/upper/channel_processors/pusch/pusch_processor_impl.cpp:195-213` `process_data(...)`
   引数に **`const dmrs_pusch_estimator_results& est_results`** を受領（推定完了後・復調前）。`:342-343` で `demodulator.demodulate(..., grid, est_results, demod_config)`。

4. `lib/phy/upper/channel_processors/pusch/pusch_demodulator_impl.cpp:253-344` `demodulate()`
   symbol ループ（`:294`）で `get_ch_data_estimates(est_results, i_symbol, nof_tx_layers, re_mask, dc, rx_ports)`（`:332`, 実体 `:495-530`）が H を **`[RE × rx_port × tx_layer]`** に組み立て（resize `:509`、`est_results.get_symbol_ch_estimate(buf, i_symbol, i_port, i_layer, re_mask)` で read-only コピー `:518`）、`equalizer->equalize(...)` へ。

### H が計算・取得可能になる API（const = read-only）✅
- `dmrs_pusch_estimator_results::get_symbol_ch_estimate(span<cbf16_t> estimates, i_symbol, rx_port, tx_layer[, re_mask]) const`
  — `include/ocudu/phy/upper/signal_processors/pusch/dmrs_pusch_estimator.h:179-190`
- 実装: `port_channel_estimator_average_impl::get_symbol_ch_estimate`
  — `lib/phy/upper/signal_processors/channel_estimator/port_channel_estimator_average_impl.cpp:105-187`
  割当 RB 全 RE 分（`nof_re = rb_count*12`, `:157`）を返す。**渡したバッファにコピーするだけで内部状態は不変**。

### H の data layout ✅
- 型 `cbf16_t`（complex bf16, `include/ocudu/adt/complex.h:31`）。
- **post-FFT 周波数領域・per RE**: estimator は DMRS RE で LSE → 全 subcarrier へ補間した "frequency-domain channel coefficients" を保持
  （`lib/phy/upper/signal_processors/channel_estimator/port_channel_estimator_average_impl.cpp:382` のコメント、`:432` `freq_interpolator->interpolate(freq_response...)`）。返り値は subcarrier（周波数）で index。
- per rx branch: `port_channel_estimator` は "for one receive antenna port"（`include/ocudu/phy/upper/signal_processors/channel_estimator/port_channel_estimator.h:28-29`）。port ごとに別インスタンス（`lib/phy/upper/signal_processors/pusch/dmrs_pusch_estimator_impl.h:57-59`）。
  → **4R なら rx_port=0..3 を個別取得可能**。rank-1 なら `tx_layer=0` のみ。

### 副作用なしで read-only tap できる場所（推奨度順）

**(A) 推奨 — estimator の decorator + notifier シム** ✅
既存の非侵襲パターン `phy_metrics_pusch_channel_estimator_decorator`（`lib/phy/metrics/phy_metrics_pusch_channel_estimator_decorator.h:16-45`）が `dmrs_pusch_estimator` を wrap し `estimate(notifier, grid, config)` を素通し。これを真似て:
1. `config` から metadata を確保
2. `notifier` を自前シムで包み、`on_estimation_complete(est_results)`（`dmrs_pusch_estimator.h:32`）で `est_results.get_symbol_ch_estimate(...)` を read-only に読み出して外部へ流し、本来の notifier へ forward。

**既存クラス無改造**（factory 配線のみ）。⚠️ 配線点は `lib/phy/upper/upper_phy_factories.cpp`（estimator 構築箇所、要特定）。

**(B) `process_data` 内**
`est_results`(const) と全 `pdu` metadata が同一スコープ（`lib/phy/upper/channel_processors/pusch/pusch_processor_impl.cpp:195-213`）。1 PDU=1 回。ただし処理クラスへの差し込みが必要（やや侵襲）。

**(C) `demodulate` の per-symbol `ch_estimates`**
既に `[RE×port×layer]` 整形済（`lib/phy/upper/channel_processors/pusch/pusch_demodulator_impl.cpp:332`）。ただし **data RE のみ**（DMRS シンボル上の DMRS 占有 RE は除外）かつ **DC は 0 埋め**（`:520-525`）、ホットパス。フルグリッド H 用途には非推奨。

---

## 4. データコントラクト整合性

| 確認項目 | 可否 | 根拠 |
|---|---|---|
| H が 4 rx branch 分取れるか | ✅ 可 | port ごとに `ch_est_result[i_port]`（`dmrs_pusch_estimator_impl.h:59`, `.cpp:55`）。`get_symbol_ch_estimate(..., rx_port, ...)` で port 指定（`dmrs_pusch_estimator.h:179`）。上限 4=`MAX_NOF_RX_PORTS`（`pusch_constants.h:35`） |
| post-FFT 周波数領域か | ✅ 可 | §3 参照。"frequency-domain channel coefficients" / `freq_interpolator->interpolate`（`port_channel_estimator_average_impl.cpp:382,432`）。受信グリッド `grid.get_view(port, l)` も per-port/per-symbol の subcarrier span（`include/ocudu/phy/support/resource_grid_reader.h:108`） |
| slot index | ✅ 可 | `pdu.slot`（`slot_point`, `include/ocudu/phy/upper/channel_processors/pusch/pusch_processor.h:106`）。`ch_est_config.slot`(`pusch_processor_impl.cpp:171`) でも保持 |
| symbol index | ✅ 可 | `get_symbol_ch_estimate` の `i_symbol` 引数。範囲は `pdu.start_symbol_index` + `pdu.nof_symbols`（`pusch_processor.h:138-140`） |
| PRB 範囲 | ✅ 可 | `pdu.freq_alloc`(rb_allocation) + `pdu.bwp_start_rb`/`bwp_size_rb`（`pusch_processor.h:110-112,136`）。`freq_alloc.get_crb_mask(...)` で CRB マスク化（`pusch_processor_impl.cpp:149`） |
| SCS | ✅ 可 | `pdu.slot.numerology()` → `to_subcarrier_spacing(...)`（`dmrs_pusch_estimator_impl.cpp:43`）。port estimator config にも `scs`（`port_channel_estimator.h:53-54`）。CP は `pdu.cp` |
| 付随メタ（任意） | ✅ 可 | per-port の `get_noise_variance / get_epre / get_snr / get_rsrp / get_time_alignment / get_cfo_Hz`（`dmrs_pusch_estimator.h:131-174`） |

### metadata と H が同一箇所で取れるか
- tap (A): `estimate()` 呼び出しの `config`（slot, rb_mask, first_symbol, nof_symbols, rx_ports, scs=slot.numerology 由来）で metadata を確保し、同じ decorator 内の notifier シムで H を取得 → **両方同一クラス内**で揃う ✅。
- tap (B) `process_data`: `est_results` + `pdu` が **完全に同一スコープ**で最も素直 ✅。

### 注意（データ整合）⚠️
- tap (C) の組み立て済 H は DC を 0 埋め（`pusch_demodulator_impl.cpp:520-525`）かつ data RE 限定。フル H が必要なら (A)/(B) で `get_symbol_ch_estimate` を割当全 RE に対し呼ぶこと。
- H は DMRS から補間された値（DMRS シンボル間は時間補間 or 平均: `port_channel_estimator_average_impl.cpp:614-676`）。生の per-RE LS 推定そのものではなく **平滑化/補間後**の H である点に留意。
- estimate は port ごとに `executor.defer(...)` で並列実行（`dmrs_pusch_estimator_impl.cpp:62`）。`on_estimation_complete` は全 port 完了後に 1 回（`:57-58`）なので、そのタイミングで読めば 4 port 揃った状態 ✅。

---

## PoC 向けまとめ（最短ルート）

- **設定**: `nof_antennas_ul: 4`（4R）＋ rank-1 を保証するなら UE が単層 or `pusch.max_rank: 1`。
- **tap**: `dmrs_pusch_estimator` の decorator（`phy_metrics_*` と同型）で `config`(metadata) ＋ wrap した notifier の `on_estimation_complete(est_results)` を捕捉 → `est_results.get_symbol_ch_estimate(buf, i_symbol, i_port∈0..3, tx_layer=0)` を全 symbol / 全割当 RE に対し read-only コピーして外部 process へ。
- **H 型**: `cbf16_t`（complex bf16, 周波数領域 per RE）。
- **layout（rank-1 + 4R）**: `[OFDM symbol] × [rx_port 0..3] × [subcarrier(周波数, 割当 RB の全 RE)]`。

---

## 主要ファイル一覧（絶対パス）

### config（Area 1 & 2）
- `apps/units/flexible_o_du/o_du_high/du_high/du_high_config.h`（237, 350, 1085-1087）
- `apps/units/flexible_o_du/o_du_high/du_high/du_high_config_cli11_schema.cpp`（2324-2325）
- `apps/units/flexible_o_du/o_du_high/du_high/du_high_config_translators.cpp`（582, 715-718）
- `apps/units/flexible_o_du/o_du_high/du_high/du_high_config_validator.cpp`（1310-1312）
- `apps/units/flexible_o_du/o_du_low/du_low_config_translator.cpp`（24-29, 56, 167）
- `lib/scheduler/config/serving_cell_config_factory.cpp`（331, 335）
- `lib/du/du_high/du_manager/ran_resource_management/du_pusch_resource_manager.cpp`（88-99, 158-169）
- `lib/du/du_high/du_manager/ran_resource_management/ue_capability_summary.h`（22, 24）
- `lib/du/du_high/du_manager/ran_resource_management/ue_capability_manager.cpp`（140-151）
- `include/ocudu/ran/pusch/pusch_constants.h`（30, 35）
- `lib/phy/upper/upper_phy_factories.cpp`（824, 883）
- `lib/ran/pusch/pusch_tpmi_select.cpp`（665, 670）

### PHY フック / データコントラクト（Area 3 & 4）
- `include/ocudu/phy/upper/signal_processors/pusch/dmrs_pusch_estimator.h`（32, 131-174, 179-190）
- `lib/phy/upper/signal_processors/pusch/dmrs_pusch_estimator_impl.{h,cpp}`（h:57-59 / cpp:43, 51-66, 171-196）
- `include/ocudu/phy/upper/signal_processors/channel_estimator/port_channel_estimator.h`（28-29, 53-54, 95-140）
- `lib/phy/upper/signal_processors/channel_estimator/port_channel_estimator_average_impl.cpp`（105-187, 382, 432, 614-676）
- `lib/phy/upper/channel_processors/pusch/pusch_processor_impl.cpp`（149, 170-192, 195-213, 342-343）
- `lib/phy/upper/channel_processors/pusch/pusch_demodulator_impl.cpp`（253-344, 332, 468, 485, 495-530, 509, 518, 520-525）
- `include/ocudu/phy/upper/channel_processors/pusch/pusch_processor.h`（102-162: pdu_t）
- `include/ocudu/phy/support/resource_grid_reader.h`（108）
- `include/ocudu/adt/complex.h`（31: cbf16_t）
- `lib/phy/metrics/phy_metrics_pusch_channel_estimator_decorator.h`（16-45: decorator パターン）
- `lib/phy/upper/channel_processors/pusch/pusch_processor_validator_impl.cpp`（21, 50-55）
- `include/ocudu/phy/upper/channel_estimation.h`（25, 389-394: 旧テンソル、PUSCH 未使用）

---

## 5. estimator factory 配線点（decorator 差し込み箇所）— Block A 実装の起点【v0.2 追記】

設計書 v0.2 7 章の残課題 ⚠「estimator の factory 配線点の特定」への回答。`dmrs_pusch_estimator` は `upper_phy_factories.cpp` 内で**直接 `new` されず factory チェーンとして組まれる**。decorator は factory レベルで挿入する（既存 metrics と同型）。**static 解析のみ・無修正。**

### 配線箇所はすべて `create_ul_processor_factory()` 内 ✅
- 関数定義: `lib/phy/upper/upper_phy_factories.cpp:550-551`
  `static std::shared_ptr<uplink_processor_factory> create_ul_processor_factory(const upper_phy_factory_configuration& config, ...)`

| 役割 | 場所 | 内容 |
|---|---|---|
| ① ベース factory 構築 ✅ | `upper_phy_factories.cpp:714-722` | `create_dmrs_pusch_estimator_factory_sw(prg_factory, low_papr_sequence_gen_factory, pusch_ch_estimator_factory, *...pusch_ch_estimator_executor.executor, config.nof_rx_ports, fd_strategy, td_strategy, compensate_cfo)` |
| **★② decorator 挿入点（＝既存 metrics の wrap・参考）** ✅ | `upper_phy_factories.cpp:725-730` | `if (metric_notifier) { pusch_channel_estimator_factory = create_pusch_channel_estimator_metric_decorator_factory(std::move(pusch_channel_estimator_factory), metric_notifier->get_pusch_channel_estimator_notifier()); ... }` ← **ISAC tap decorator factory はここで同様に `std::move` して包む**（挿入は 723 の直後〜811 の前、自然には 730 の後） |
| ③ 消費点 ✅ | `upper_phy_factories.cpp:811` → `825`, `838` | `pusch_config.estimator_factory = pusch_channel_estimator_factory;`（:811）→ `create_pusch_processor_factory_sw(pusch_config)`（:825 data PUSCH / :838 UCI PUSCH） |

**重要** ✅: data PUSCH（`:825`）と UCI PUSCH（`:838`）は**同一の `pusch_config` を共有**するため、②で 1 回包めば両 processor に効く。wrap は `:811` より前で行うこと。

### decorator が「どこで wrap してるか」のメカニズム（参考実装）✅
- wrap 実体: `lib/phy/metrics/phy_metrics_factories.cpp:251-260`
  `create_pusch_channel_estimator_metric_decorator_factory(...)` →
  `metric_decorator_factory<dmrs_pusch_estimator, dmrs_pusch_estimator_factory, pusch_channel_estimator_metric_notifier, phy_metrics_pusch_channel_estimator_decorator>` を返す。
- factory-decorator テンプレート: `phy_metrics_factories.cpp:41-56`、核心 `:51`
  `std::unique_ptr<PhyBlock> create() override { return std::make_unique<Decorator>(base_factory->create(), notifier); }`
  → **生成のたびに base factory の `create()`（＝実インスタンス）を呼び、それを `Decorator` で包む**。ISAC では `Decorator` を自前 tap decorator に差し替えるだけ。

### 実インスタンスが構築される場所 ✅
- `lib/phy/upper/signal_processors/pusch/factories.cpp:37-46`、`dmrs_pusch_estimator_factory_sw::create()`
  - `:39-43`: `nof_rx_ports` 個の `port_channel_estimator` を生成（**4R なら 4 本**）
  - `:44-45`: `std::make_unique<dmrs_pusch_estimator_impl>(prg_factory->create(), low_papr_gen_factory->create(), std::move(estimators), executor)`

### 実装上の注意（7 章 tap レシピとの差分）⚠
- 既存 `phy_metrics_pusch_channel_estimator_decorator`（`.h:27-40`）は **CPU 時間を測るだけで `notifier` は素通し**。ISAC tap は設計書どおり、`estimate()` 内で **`notifier` を自前シムで包んで `on_estimation_complete(est_results)` を横取り**する一手間が追加で必要（metrics 版にこのステップは無い）。
- **混同注意（別レベルの decorator 点）**: `upper_phy_factories.cpp:643-644` の `create_port_channel_estimator_metric_decorator_factory` は **per-port（lower-level）estimator** 用。notifier / `est_results` の組み立てを見られないため **tap には不適**。狙うのは `dmrs_pusch_estimator` レベル（②）。
- **ISAC sink（ZMQ）参照の注入**: 既存は `metric_notifier->get_pusch_channel_estimator_notifier()` を渡す。ISAC では同様に自前 sink 参照を `create_ul_processor_factory` まで配線（`upper_phy_factory_configuration`/dependencies 経由）するか、ローカル生成する — Block A 実装の細目。

### 追加の主要ファイル（Area 5）
- `lib/phy/upper/upper_phy_factories.cpp`（550-551: `create_ul_processor_factory` / 714-722: ①base / 725-730: ★②decorator / 811,825,838: ③消費 / 643-644: per-port 別レベル）
- `lib/phy/metrics/phy_metrics_factories.cpp`（41-56: `metric_decorator_factory` テンプレ, 51: create() / 251-260: pusch chan-est decorator factory）
- `lib/phy/upper/signal_processors/pusch/factories.cpp`（12-57: `dmrs_pusch_estimator_factory_sw`, 37-46: create(), 44-45: impl 構築, 61-: `create_dmrs_pusch_estimator_factory_sw`）
