# フェーズ0: UL/DL BLER 調査 — コードパスマップ

対象: OCUDU (srsRAN Project 系, namespace `ocudu`) / 構成: n77 100MHz SCS30k 4T4R TDD(DDDSUUDDDD), OFH split 7.2
参照コンフィグ: `gnb_ru_ofh_tdd_n77_100mhz_4x4.yml`(pusch.max_rank=1, BFP9 static, ta4=[50,331]us)

本書は「どこで何が起きるか」をファイル:行で固定するための地図。指摘・評価はフェーズ1
(`02_static_review.md`)に分離している。

---

## 1. UL PUSCH 受信チェーン(スロット受信 → CRC → MAC 通知)

```
OFH U-plane 受信                lib/ofh/receiver/ofh_message_receiver_impl.cpp
  ├─ eAxC フィルタ (:137-147)  ul_port_id / prach_port_id 照合
  ├─ 受信窓チェック (:104) ────→ lib/ofh/receiver/ofh_rx_window_checker.cpp:64-95
  │                              (ta4_min/max→シンボル換算は ru_ofh_config_translator.cpp:26-37)
  ├─ BFP 伸長 ────────────────→ lib/ofh/compression/iq_compression_bfp_impl.cpp:89-113
  │                              (PRB 毎 exponent はペイロード内。static hdr は udCompHdr のみ省略:
  │                               ofh_uplane_message_decoder_static_compression_impl.cpp:10-19)
  └─ グリッド書込 ─────────────→ ofh_uplane_rx_symbol_data_flow_writer.cpp:58-63
                                  (値 = BFP値 / 32767, UL 側に追加スケーリング無し)

upper PHY 受信
  uplink_processor_impl.cpp:270-335   process_pusch(): HARQ バッファ reserve → pusch_executor へ
  └─ pusch_processor_impl.cpp
       :116-193  process(): DMRS チャネル推定を起動 (estimator.estimate, :192)
       :195-344  process_data(): CSI 組立(:212-213)→ ULSCH demux(:321-322)→ 復調(:342-343)
```

### 1.1 DMRS 抽出・チャネル推定(ポート毎)

| 処理 | 場所 |
|---|---|
| ポート毎推定のディスパッチ(executor 並列) | `lib/phy/upper/signal_processors/pusch/dmrs_pusch_estimator_impl.cpp:51-66` |
| DMRS 系列生成(w_t/w_f OCC 適用) | 同 `:104-169`(OCC テーブルは `dmrs_helper.cpp:21-45`) |
| Rx パイロット抽出(CDM グループ単位) | `port_channel_estimator_helpers.cpp:128-190` `extract_layer_hop_rx_pilots()` |
| LSE(受信×共役パイロット) | `port_channel_estimator_average_impl.cpp:488-492` |
| CFO 推定(DMRS 2 シンボル間の位相) | 同 `:468-525` `preprocess_pilots_and_estimate_cfo()` |
| CFO 補償+時間方向結合 | 同 `:527-599` `compensate_cfo_and_accumulate()` |
| CDM ペアの直交化(隣接 RE 平均) | 同 `:743-761` `average_pairs()` ※2レイヤ時のみ(:596-598) |
| FD スムージング(filter/mean/none) | `port_channel_estimator_helpers.cpp:192-233` `apply_fd_smoothing()` |
| 仮想パイロット(帯域端外挿) | 同 `:297-368` |
| 周波数補間(DMRS RE→全 RE) | `port_channel_estimator_average_impl.cpp:432-434` |
| 時間方向戦略(average/interpolate) | 同 `:614-676` `apply_td_domain_strategy()` |
| 雑音分散推定(再生成パイロット残差) | 同 `:763-862` `estimate_noise()` → 正規化 `:284-286` |
| SNR(推定器)= data電力/雑音分散 | 同 `:276-292`(`datarp = rsrp_avg·nof_layers/β²`) |
| TA 推定(周波数応答の群遅延) | `port_channel_estimator_helpers.cpp:235-284` → IDFT 系 `time_alignment_estimator` |

- **レイヤ×CDM の分岐**: レイヤは 2 個ずつ(CDM グループ単位)処理(`compute_hop` の `i_layer += 2`、`:346`)。
  レイヤ 0/1 は同一 RE の DMRS を TD-OCC {+1,+1}/{+1,-1} で共有し、`average_pairs()` が
  「隣接 2 RE のチャネル不変」を仮定して分離する。**rank1 では average_pairs は走らない**。
- 推定器の設定ノブ(fd/td/cfo)はコンストラクタ固定:
  `du_low_config.h:41,50,52` → `du_low_config_translator.cpp:46-51` → `upper_phy_factories.cpp:568-580,713-722`。

### 1.2 MIMO 検出 / イコライズ(1↔2レイヤ分岐の本体)

`lib/phy/upper/equalization/channel_equalizer_generic_impl.cpp`

- アルゴリズム選択(zf/mmse)は**プロセス起動時に固定**: `upper_phy_factories.cpp:563-566`
  (設定キー `pusch_channel_equalizer_algorithm`、**既定 "zf"** `du_low_config.h:58`)。
- 分岐表 `equalize()` (:511-653):

| layers × ports | ZF(既定) | MMSE 設定時 |
|---|---|---|
| 1 × N (N=1..4,8) | `equalize_zf_1xn.h`(= MRC, ポート別雑音分散で除外判定) | 同左(1レイヤは ZF と等価として共通化 :591-597) |
| 2 × 4 | `equalize_zf_2xn.h`(閉形式擬似逆行列) | `equalize_mmse_mxn_simd.h`〈2,4〉(G+σ²I の逆行列) |
| 3-4 × 4 | `equalize_zf_mxn` | `equalize_mmse_mxn` |

- **雑音モデル**: 複数レイヤ時は**スカラー1個**に縮約 — `:528`
  `noise_var = max(noise_var_estimates)`(全ポート最悪値)。**共分散行列は持たない = MMSE-IRC 非実装**
  (リポジトリ全体に IRC/干渉共分散のコード無し)。
- 1レイヤ(MRC)だけはポート別 σ²_p を保持: `equalize_zf_1xn.h:110-152`
  (整形合成 `Σh*y / Σ|h|²`、post-eq 雑音分散 `Σ|h|²σ²_p / (Σ|h|²)²`)。
- 2×4 ZF の post-eq 雑音分散はレイヤ別に `σ²·‖h_other‖²/(β²·det)`(`equalize_zf_2xn.h:229-230`)、
  つまり**雑音強調(ill-conditioning)は post-eq SINR に反映される**。
- 4 Rx ブランチ合成箇所 = 上記 equalize 呼出し(`pusch_demodulator_impl.cpp:343-344`)。
  ポート別雑音分散の取り出しは同 `:336-338`(`est_results.get_noise_variance(i_port)`)。

### 1.3 復調・LLR・デスクランブル

`lib/phy/upper/channel_processors/pusch/pusch_demodulator_impl.cpp`

- シンボル毎ループ `:294-431`。等化出力はレイヤ・インタリーブ(`[l0,l1] per RE`)。
- post-eq SINR: 等化出力雑音分散の平均 → `-10log10(mean)`(`:410-415, 433-438`)。
  **全レイヤ・全 RE 混合平均でありレイヤ別 SINR は算出していない**(計装ポイント)。
- ソフトデマップ: `demapper->demodulate_soft(codeword, eq_re, eq_noise_vars, mod)`(`:385`)
  → `lib/phy/upper/channel_modulation/demodulation_mapper_qam64.cpp` 等。
  LLR は float 計算後 `RANGE_LIMIT_FLOAT = 20` で飽和し int8 量子化(qam64.cpp:27-28)。
- EVM: `evm_calc` が有効な場合のみ(`:388-392`)。有効条件は
  「sinr 計算法 = evm」or「PHY ログ debug」(`upper_phy_factories.cpp:785-786`)。
- デスクランブル `revert_scrambling`(`:21-205`)。
- DC サブキャリア: チャネル推定を 0 化 → 等化器が noise_var=∞ を出し SINR 集計から除外(`:520-525`, `filter_infinite_and_accumulate:239-248`)。

### 1.4 ULSCH demux → LDPC → HARQ 合成 → CRC

- demux: `ulsch_demultiplex_impl.cpp`(データ/HARQ-ACK/CSI1/CSI2 の RE 分離)。
- デコーダ: `pusch_decoder_impl.cpp`
  - CB 分割・並列 fork `:295-381`
  - レートデマッチ+HARQ ソフト合成: `pusch_codeblock_decoder.cpp:26`(`rate_match` が
    `rx_buffer_impl.h:162-173` の永続バッファへ LLR 加算合成 = IR 合成)
  - LDPC 復号: `pusch_codeblock_decoder.cpp:34-46`
    (max_iterations は設定 `pusch_decoder_max_iterations` **既定 6**, `du_low_config.h:23`;
     early-stop 既定 true `:25`)
  - CRC: CB=24B/TB=24A/16 選択 `:24-35`、TB 検証 `:403-422`
- 結果通知: `pusch_processor_notifier_adaptor.h:129-150` が demod 統計を CSI に注入
  (post-eq SINR、EVM→`sinr_evm = -20log10(EVM) - 3.7dB`)→ `on_sch()`。

### 1.5 「UL SNR」の出自と MAC への伝搬(仮説A の配線)

```
channel_state_information (include/ocudu/phy/upper/channel_state_information.h)
  ├─ sinr_ch_estimator_dB   … チャネル推定器(DMRS 残差ベース)
  ├─ sinr_post_eq_dB        … 等化器出力雑音分散ベース ←★既定でこれが「選択」される
  ├─ sinr_evm_dB            … 復調 EVM ベース(-20log10(EVM)-3.7)
  ├─ port_rsrp_dB[4] / epre_dB / time_alignment / cfo_Hz
  └─ get_sinr_dB() = 選択された 1 本 (:67-78)
        │  選択キー: du_low expert `pusch_sinr_calc_method`(既定 "post_equalization",
        │            du_low_config.h:34 → pusch_processor_impl.cpp:212)
        ▼
upper_phy_rx_results_notifier → FAPI CRC.indication ul_sinr_metric
        ▼
MAC/スケジューラ ue_event_manager.cpp:504-555 handle_crc_indication()
  ├─ ue_cell.cpp:123-155 handle_crc_pdu()
  │    ├─ OLLA 更新: ue_mcs_calculator->handle_ul_crc_info(...)  (:147-150)
  │    └─ SNR 取込:  channel_state->update_pusch_snr(ul_sinr_dB) (:153-155)
  │         └─ ue_channel_state_manager.cpp:60-64
  │            瞬時値 pusch_snr_db(MCS 用)+ EMA(電力制御用のみ)
  └─ TA 更新: ue.handle_ul_n_ta_update_indication (:540-543)
```

### 1.6 UL リンクアダプ(SNR+OLLA → MCS)

- OLLA 本体: `lib/scheduler/support/outer_loop_link_adaptation.h:43-53`
  (ACK: +δdown / NACK: -δup, δup=(1-target)/target·δdown, ±max_snr_offset クランプ)。
- 既定値: `scheduler_expert_config.h:185-189`
  **olla_ul_snr_inc=0.001, olla_ul_target_bler=0.01, olla_max_ul_snr_offset=5.0** → UL OLLA は既定で有効。
- コントローラ: `ue_link_adaptation_controller.cpp`
  - `get_effective_snr() = 瞬時 pusch_snr + olla_offset`(`:93-96`)
  - `calculate_ul_mcs()`(`:124-137`)→ `map_snr_to_mcs_ul()`
  - **OLLA 更新スキップ条件**: 送信 MCS ≠ OLLA 提案 MCS のとき(`:65-67`)
- SNR→MCS テーブル: `mcs_calculator.cpp:57-66`(qam64: MCS27=16.04dB, MCS28=16.59dB。
  ZMQ+AWGN/SISO/20MHz で実測校正された「暫定」テーブルと明記 `:41-54`)。
- 後段の MCS 低減(码率≤0.95 制約): `ue_cell_grid_allocator.cpp:673-705` — ここで下げられた
  スロットは上記スキップ条件により OLLA 更新が走らない。
- UL rank(nof_layers)決定: SRS ベース `ue_channel_state_manager.cpp:87-137`
  `get_nof_ul_layers()`(閾値 18dB / レイヤ間差 6dB / 高 rank 優遇バイアス +2dB。
  SRS の雑音分散は「受信電力-30dB」の固定仮定 `:69-71`)。
  max_rank の流入: `du_high_config.h:237` → `du_high_config_translators.cpp:715-718` →
  `du_pusch_resource_manager.cpp:158-168`(コードブック max_rank)。
  **max_rank=1 なら常に 1 レイヤ**(SRS 情報が無い場合も 1, `:92-94`)。

### 1.7 n-TA / UL 受信窓の適用箇所(仮説10 の配線)

- **DU は UL データパスに n_ta_offset を適用しない**。参照箇所は次の 3 つのみ:
  1. UE への通知: SIB1 `n-TimingAdvanceOffset`(`asn1_sys_info_packer.cpp:279-301`)/
     RRC `ServingCellConfigCommon`(`asn1_rrc_config_helpers.cpp:3437-3451`)。
     値はバンドから固定導出: **FR1 → n25600**(`band_helper.cpp:1667-1679`)。ユーザ設定不可。
  2. split-8(SDR)lower PHY の Tx/Rx 時間差(`lower_phy_factory.cpp:41-48`)— **OFH では未使用**。
  3. PRACH/PUSCH の TA 測定は相対測定で n_ta_offset を含まない
     (`prach_detector_generic_impl.cpp:317-326`)。
- よって OFH 構成では「gNB 側 n-TA」とは = SIB の 25600 通知 + RU が O-RAN CUS 仕様に従い
  自装置で 25600 を適用、の 2 点で完結する。RU 確定値 25600 と gNB 通知 25600 は**整合**。
- UL 受信窓(ta4)は OFH 受信側でのみ判定: `ofh_rx_window_checker.cpp:64-95`
  (early/on_time/late カウンタ、`ofh_receiver_metrics.h:12-24`)。
  窓閉時の取りこぼし: `ofh_closed_rx_window_handler.cpp:76,113`
  (`nof_missing_uplink_symbols` / `nof_missing_prach_contexts`)。
- クローズドループ TA(MAC): `ta_management_system.cpp:182-263`
  (CRC 成功時のみ採用、SINR 閾値 `update_measurement_ul_sinr_threshold` 既定 0.0dB、
   z-score 外れ値除去、TA CMD 生成 `:101-133`)。

---

## 2. DL 側(リンクアダプ / スケジューラ / プリコーディング)

| 機能 | 場所 |
|---|---|
| CSI 取込(CQI/RI/PMI) | `ue_cell.cpp:166` → `ue_channel_state_manager.cpp:32-58` |
| DL OLLA(CQI 補正) | `ue_link_adaptation_controller.cpp:32-52`(既定: inc=0.001, target BLER=0.01, max offset=±4.0 CQI) |
| CQI→MCS(線形補間) | 同 `:98-122` + `mcs_calculator.cpp:92-112` |
| DL レイヤ数 = 報告 RI | `ue_channel_state_manager.h:43`(`recommended_dl_layers`) |
| PMI→プリコーディング | `ue_channel_state_manager.h:52-64` `get_precoding()` |
| HARQ 再送キャンセル(CQI/RI 急落) | `ue_cell.cpp:391-428`(閾値 `scheduler_expert_config.h:191-193`) |
| newTx グラント時の MCS/PRB/レイヤ | `grant_params_selector.cpp:105,117,283-289` |
| DL OLLA 更新スキップ | UL と同じ「使用 MCS ≠ OLLA 提案」+ DTX 除外(`ue_cell.cpp:113`) |

DL の受信機は UE(ブラックボックス)。gNB 側でコードとして関与するのは
OLLA / CQI→MCS / RI・PMI 追従 / スケジューラのみ(診断レポータの帰着先もここに限定)。

---

## 3. 既存の観測手段(フェーズ2・4 が消費する土台)

### 3.1 JSON metrics(remote_control :8001, `metrics.enable_json`)
- 生成: `apps/helpers/metrics/json_generators/du_high/scheduler.cpp:38-140`
- UE 毎: `pusch_snr_db`(=CRC 時 ul_sinr の平均。★リンクアダプが見る SNR と同源)、
  `pusch_rsrp_db`, `ul_mcs`(スケジュール済 MCS 平均), `ul_nof_ok/ul_nof_nok`(CRC 集計 → BLER),
  `ul_brate`, `ul_ri`, `dl_*`(cqi, dl_ri, dl_mcs, ok/nok, brate), `ta_ns`, `pusch_ta_ns`, `bsr`, `last_phr` 等
- セル毎: PRB 使用 `pusch_prbs_used_per_tdd_slot_idx` ほか
- OFH(`metrics.enable_ru_metrics` + json): `json_generators/ru/ofh.cpp`
  `received_packets{total,early,on_time,late,earliest_msg_us,latest_msg_us}`,
  `rx_window_stats{nof_missed_uplink_symbols, nof_missed_prach_occasions}`, eCPRI seq-id 統計, TX late 系
- スケジューラ log 版(`enable_log`)には `ul_olla=` / `dl_olla=` も出る
  (`scheduler_metrics_consumers.cpp:153-401`)— **OLLA 飽和の観測に使える**

### 3.2 PHY ログ(`log.phy_level`)
- info: `PUSCH: rnti=... prb=... mod=... rv=... crc=OK|KO iter=... sinr=XX.XdB t=...`
  (選択 SINR 1 本のみ。`logging_pusch_processor_decorator.h:152-162`)
- debug: 上記+ **sinr_ch_est / sinr_eq / sinr_evm(選択元に [sel] 印)+ evm(シンボル別)
  + epre + port別 rsrp + t_align(µs) + cfo(Hz)**(`channel_state_information_formatters.h:69-137`)
  ※ debug は PDU 全フィールド+ペイロード hex も出るため常用は重い → フェーズ2 で
    「診断専用フラグ」に分離する。

### 3.3 stdout メトリクス表
`scheduler_metrics_consumers.cpp:12-145`。UL 側列: `pusch`(=pusch_snr_db), `rsrp`, `ri`, `mcs`,
`brate`, `ok`, `nok`, `(%)`=nok/(ok+nok), `bsr`, `ta`, `phr`。

### 3.4 解析スクリプトの置き場
リポジトリ内に oran_iq_analyzer.py は存在しない(環境側ツールと推定)。同じ立ち位置の
スタンドアロン解析スクリプトとして `scripts/` に追加する(→フェーズ4)。
