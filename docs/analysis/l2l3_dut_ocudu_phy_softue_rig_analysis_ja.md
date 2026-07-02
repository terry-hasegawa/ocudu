# 対象 L2/L3（SCF FAPI/nvIPC）× OCUDU software PHY × ソフトUE 検証リグ — 実装解析レポート

- 対象リポジトリ: OCUDU（本ワークツリー、`dev` 相当。srsRAN Project 派生、namespace は `ocudu`、初回リリース 26.04）
- 調査方法: 実ソース精読（`file:line` 根拠付き）。本リポジトリ外のコンポーネント（DUT、NVIDIA Aerial/nvIPC、DeepSig connector、srsRAN_4G/srsUE、Open5GS）は本セッションからアクセス不可のため、該当記述にはすべて **要確認** を付す
- 表記: コードから確認できた事実はそのまま記述。推論は「**見解:**」、未確認は「**要確認**」と明記

---

## 0. 主要結論（TL;DR）

1. **OCUDU には split-6 の standalone DU-low アプリ（`apps/du_low`）が既に存在し、「MAC 無しで PHY を FAPI 駆動する」ための土台は完成している**。cell 設定は FAPI `CONFIG.request` から動的に取り込まれ、`START.request` で RU+PHY が生成・起動される（`apps/du_low/du_low.cpp:44`, `apps/units/flexible_o_du/split_6/o_du_low/fapi_adaptor/configuration_procedure.cpp`）。→ **standalone PHY 駆動のための本体改修は不要**。
2. ただし OSS ビルドでは FAPI トランスポートを供給する**プラグインが dummy**（`on_configuration_validation()` が `false` を返し起動前に abort。`split6_o_du_low_plugin_dummy.h:21`）。実トランスポートは `OCUDU_HAS_SPLIT6_ENTERPRISE` マクロで差し替える**商用（enterprise）プラグイン前提**の設計（`split6_o_du_low_plugin_dummy.cpp:127`）。→ **本件ブリッジ＝この `split6_o_du_low_plugin` の nvIPC 実装を自作**することに正確に帰着する。
3. OCUDU の FAPI（FAPI+）は **in-process C++ 構造体渡し**で、**バイト列パック/アンパックはリポジトリ内に一切存在しない**（全域 grep で確認）。メッセージ集合・メッセージ ID は SCF-222 に整合（`include/ocudu/fapi/common/error_indication.h:14-34` の enum が SCF の 0x00〜0x8A と一致、コメントは「SCF-222 v4.0」を全域で参照）。→ ブリッジの変換は「wire 構造体 ⇄ C++ 構造体」の**素直な field mapping**。
4. リポジトリ内に **nvIPC / cuPHY / Aerial / nFAPI のコードは皆無**（全域 grep で 0 件。`aerial` のヒットは 3GPP NGAP のドローンUE IE のみ）。nvIPC 側は Aerial SDK / DeepSig connector からの導入が必要（**要確認**）。
5. software PHY は LDPC（min-sum BP、AVX2/AVX-512/NEON）、polar、MMSE 等化、ZC 相関 PRACH 検出まで実装された**本物の L1**であり、ZMQ radio（`tcp://` REP/REQ、complex float32）で srsUE と実波形を通せる。test-mode UE（`du_high` の `test_mode.test_ue`、MAC への合成刺激）とは別物。
6. 投資判断: **greenfield ではない**（PHY/ZMQ/セッション管理/プラグイン境界/メッセージ builder が全て既存）が、**config でもない**（nvIPC slave 役のブリッジ実装は net-new）。規模感は「中」— 詳細は §6。

---

## 1. リグ全体図

```
┌──────────────────────────────────────────────────────────────┐
│  DUT: 対象 L2/L3（無改造）                        【リポジトリ外・要確認】│
│  - SCF 5G-FAPI (222.x) を nvIPC で送受（Aerial 向け実装）      │
│  - 北側: NGAP/N2/N3 → Open5GS（DUT が CU/RRC を含む前提・要確認）│
└──────────────┬───────────────────────────────────────────────┘
               │ nvIPC (SHM リング; SCF FAPI wire 構造体)      【要確認】
┌──────────────▼───────────────────────────────────────────────┐
│  ★FAPI ブリッジ（自作）= split6_o_du_low_plugin の nvIPC 実装   │
│  - cuphycontroller の nvIPC エンドポイントを代役（PHY slave 役）│
│  - SCF wire 構造体 ⇄ ocudu::fapi C++ 構造体 の双方向変換        │
│  - 実装対象 I/F:                                              │
│      split6_o_du_low_plugin      (apps/.../split6_o_du_low_plugin.h:23) │
│      mac_fapi_p5_sector_adaptor  (include/ocudu/fapi_adaptor/mac/p5/mac_fapi_p5_sector_adaptor.h:20) │
│      mac_fapi_p7_sector_adaptor  (include/ocudu/fapi_adaptor/mac/p7/mac_fapi_p7_sector_adaptor.h:23) │
└──────────────┬───────────────────────────────────────────────┘
               │ FAPI+（in-process C++。パッキング無し）
               │  南向き: fapi::p5_requests_gateway / fapi::p7_requests_gateway
               │  北向き: fapi::p5_responses_notifier / p7_slot_indication_notifier /
               │          p7_indications_notifier / error_indication_notifier
┌──────────────▼───────────────────────────────────────────────┐
│  apps/du_low（split 6 O-DU low standalone アプリ）【既存】      │
│  - configuration_procedure: P5 FSM (IDLE→CONFIGURED→RUNNING)  │
│  - session = { o_du_low + RU + P7 adaptor }を START で生成      │
│  - o_du_low: upper PHY（実DSP: LDPC/polar/MMSE/ZC）            │
│              + phy_fapi fastpath adaptor（lib/fapi_adaptor/phy）│
│  - ru_sdr: lower PHY（OFDM 変復調）+ radio driver              │
└──────────────┬───────────────────────────────────────────────┘
               │ time-domain IQ（cf_t = complex float32）
┌──────────────▼───────────────────────────────────────────────┐
│  ZMQ radio（lib/radio/zmq）【既存】                            │
│  - TX: ZMQ_REP bind / RX: ZMQ_REQ connect（tcp:// のみ）       │
│  - transmit-on-request（要求駆動サンプル転送、タイムスタンプは   │
│    サンプルカウンタで擬似化）                                   │
└──────────────┬───────────────────────────────────────────────┘
               │ tcp://（アンテナ毎に tx_port/rx_port ペア）
┌──────────────▼───────────────────────────────────────────────┐
│  srsUE（srsRAN_4G, ソフトUE = 実 UE スタック）【リポジトリ外・要確認】│
│  - NR SA。PHY/MAC/RLC/PDCP/RRC/NAS を実装、実波形を復調        │
│  - netns で TUN を分離し E2E IP 疎通                            │
└──────────────┬───────────────────────────────────────────────┘
               │ NAS（UE⇔AMF; 経路は 無線→OCUDU PHY→ブリッジ→DUT→N2）
┌──────────────▼───────────────────────────────────────────────┐
│  Open5GS 5GC（AMF/SMF/UPF ほか）【リポジトリ外・要確認】        │
└──────────────────────────────────────────────────────────────┘
```

要点:
- **DUT は無改造**。DUT から見える相手は「cuphycontroller と同じ nvIPC エンドポイント + SCF FAPI メッセージ」だけ（**要確認**: DUT の nvIPC 設定・期待する PARAM/CONFIG TLV 集合）。
- これは DeepSig `ocudu_aerial_fapi_connector`（OCUDU L2 → Aerial cuPHY、OCUDU が FAPI **master** 側）の**鏡像**（DUT L2 → OCUDU PHY、OCUDU が FAPI **slave** 側）。DeepSig repo は本セッションのアクセス制限（GitHub アクセスが本リポジトリのみに限定）により未読のため、同 connector に関する記述は全て設計参照レベル（**要確認**）。
- OCUDU 側にも同じ「鏡像」が in-tree に存在する: `apps/units/flexible_o_du/split_6/o_du_high`（DU-high 側 split6、外部 PHY へ FAPI を送る側。`split6_plugin.h:31`）。ブリッジ設計時の対向仕様の参照に使える。

---

## 2. software PHY 処理チェーン（DL/UL、file:line）

### 2.1 DL: FAPI+ 受領 → upper PHY → lower PHY → ZMQ TX

| # | 段 | 実装（file:line） | 内容 |
|---|----|----|----|
| D1 | FAPI 受領 | `lib/fapi_adaptor/phy/p7/fapi_to_phy_fastpath_translator.cpp:278` (`send_dl_tti_request`) | slot controller 取得＝resource grid 予約 + downlink_processor 設定（`.cpp:82-97`）。遅延判定 `is_message_in_time`（`.cpp:730`: 受理窓 `[current − nof_slots_request_headroom, current]`、外れは `OUT_OF_SYNC` の ERROR.indication） |
| D2 | PDU 変換 | 同 `.cpp:171` (`translate_dl_tti_pdus_to_phy_pdus`) | `dl_pdcch/pdsch/csi_rs/ssb/prs_pdu` → PHY PDU。precoding は `pm_index` → `precoding_matrix_repository`（`include/ocudu/fapi_adaptor/precoding_matrix_repository.h:14`）参照。PDU validator で fail-closed（1 PDU でも invalid なら message ごと破棄 + ERROR.indication） |
| D3 | PDSCH 保留 | 同 `.cpp:351` | PDSCH は TX_Data.request 到着まで `pdsch_pdu_repository` に保留。`send_tx_data_request`（`.cpp:620`）で `shared_transport_block` を**ゼロコピー**添付し `process_pdsch`（`.cpp:703`） |
| D4 | DL dispatch | `lib/phy/upper/downlink_processor_multi_executor_impl.cpp` | チャネル別 executor に defer: PDCCH `:54/:68`、PDSCH `:86/:102`、SSB `:126/:140`、NZP-CSI-RS `:158/:172`、PRS `:190/:204` |
| D5 | PDSCH encode | `lib/phy/upper/channel_processors/pdsch/pdsch_block_processor_impl.cpp:45` | セグメンテーション（`ldpc_segmenter_tx_impl.cpp:39`: TB-CRC・lifting）→ **LDPC encode** `:68`（AVX2/AVX-512/NEON カーネル: `ldpc_encoder_avx2.cpp:134-245` 等）→ **rate matching** `:72`（`ldpc_rate_matcher_impl.cpp:75/86/254`）→ **scrambling** `:75` → **QAM modulation** `:79`（AVX-512 実装 `modulation_mapper_avx512_impl.cpp`） |
| D6 | layer map/precoding/RE map | `lib/phy/support/resource_grid_mapper_impl.cpp:120-155` (`map_re_block`→`apply_layer_map_and_precoding`), `:165`, `:294` | DM-RS/PT-RS は `pdsch_processor_flexible_impl.cpp:292/281` |
| D7 | PDCCH/SSB | `pdcch_encoder_impl.cpp:62`（CRC24C+RNTI scramble `:30-37`、**polar encode** `:54`、rate match `:57`）、`ssb_processor_impl.cpp:11-88`（PBCH polar encode `:40`、PSS `:77`/SSS `:88`） | |
| D8 | grid 送出 | `downlink_processor_multi_executor_impl.cpp:259-265` (`send_resource_grid` → `gateway.send`) | 全 PDU 完了（atomic FSM `downlink_processor_multi_executor_state.h:68`）または `finish_processing_pdus`（`:249`）で lower PHY へ。gateway I/F: `include/ocudu/phy/upper/upper_phy_rg_gateway.h:24` |
| D9 | OFDM 変調 | `lib/phy/lower/processors/downlink/downlink_processor_baseband_impl.cpp:83`（timestamp→slot/symbol 算出、`:133` で `on_tti_boundary` を **`slot + nof_slot_tti_in_advance`** で通知）→ `pdxch_baseband_modulator.h:44/:144-168`（変調+振幅制御+`cf_t`→`ci16_t`）→ `ofdm_modulator_impl.cpp:39`（DFT `:80`、TS38.211 §5.4 位相補償 `:83-86`、**CP 付加** `:89`） | |
| D10 | 基底帯域送出 | `lib/phy/lower/lower_phy_baseband_processor.cpp:70` (`dl_process`) | RX に対し TX を最大 ~1ms 先行に throttle（`:89`）、`transmitter.transmit`（`:126`）。`tx_time_offset = ta_offset − time_alignment_calibration`（`lower_phy_factory.cpp:41-48`） |
| D11 | ZMQ TX | `lib/radio/zmq/radio_zmq_tx_channel.cpp` | ZMQ_REP bind（`radio_session_zmq_impl.cpp:52`）。1 byte のリクエスト受信（`:116`）→ circular buffer（614,400 samples 既定 `radio_session_zmq_impl.cpp:13`）からサンプル応答（`:150-187`）。未接続でも zero 送出で前進（`:278-285`） |

### 2.2 UL: ZMQ RX → lower PHY → upper PHY → indication 生成

| # | 段 | 実装（file:line） | 内容 |
|---|----|----|----|
| U1 | ZMQ RX | `lib/radio/zmq/radio_zmq_rx_channel.cpp` | ZMQ_REQ connect（`:49`）。1 byte 要求送信（`:91`）→ サンプル受信（`:126`）。timestamp はサンプルカウンタ（`radio_zmq_rx_stream.cpp:75/95`） |
| U2 | 基底帯域受領 | `lower_phy_baseband_processor.cpp:139` (`ul_process`) | `receiver.receive` → `last_rx_timestamp` 更新（`:155`）→ UL 復調タスク defer |
| U3 | シンボル切出し + OFDM 復調 | `lib/phy/lower/processors/uplink/uplink_processor_impl.cpp:83`（CP-aware に 1 シンボルずつ収集、CFO 補償）→ PUxCH `:230` → `ofdm_demodulator_impl.cpp:77`（CP skip `:106`、DFT、位相補償 `:112-115`） | half/full slot 通知 `:260/:266` |
| U4 | PRACH 窓 | `lib/phy/lower/processors/uplink/prach/prach_processor_worker.cpp:100-153` | 要求駆動で PRACH 窓を収集し `on_rx_prach_window`（`:129`）。復調は `ofdm_prach_demodulator_impl.cpp:14`（long/short 対応、`:136` で 1/√N スケール） |
| U5 | upper PHY 受領 | `lib/phy/upper/upper_phy_rx_symbol_handler_impl.cpp:24-42` | シンボル到着毎に `uplink_processor_impl.cpp:117` (`handle_rx_symbol`) → シンボル完了分の PDU を `pdu_repository` から取り出し dispatch（`:189-196`） |
| U6 | PRACH 検出 | `uplink_processor_impl.cpp:235/:253` → `prach_detector_generic_impl.cpp:60` | **ZC 系列生成**（`:199`）→ 共役乗算（`:235`）→ **IDFT 相関**（`:242`）→ ノイズ推定＋閾値判定（`:287-322`）→ preamble index + TA 出力（`:324-336`）→ `on_new_prach_results`（`uplink_processor_impl.cpp:256`） |
| U7 | PUSCH 復号 | `uplink_processor_impl.cpp:270`（HARQ softbuffer 確保 `:292`、payload buffer `:301`）→ `pusch_processor_impl.cpp:116`（**チャネル推定** `:192`）→ `pusch_demodulator_impl.cpp:253`（**MMSE 等化** `:343`（`equalize_mmse_mxn_simd.h:14`）、soft demap `:385`、descramble `:396-399`（AVX-512/AVX2/SSE3/NEON `:32-199`））→ `pusch_decoder_impl.cpp`（rate dematch `:329`、**LDPC decode（scaled min-sum BP）** `:344`（`ldpc_decoder_generic.cpp:12-92`）、TB CRC `:418`） | UCI 抽出（HARQ-ACK/CSI1/CSI2 `pusch_processor_impl.cpp:310-321`） |
| U8 | PUCCH/SRS | PUCCH F0/F1 検出器・F2/3/4 復調器（`uplink_processor_impl.cpp:337-438`、`pucch_detector_format1.cpp` 等）、SRS 推定 `srs_estimator_generic_impl.cpp:61`（LSE、TA、channel matrix） | |
| U9 | 結果通知 | `include/ocudu/phy/upper/upper_phy_rx_results_notifier.h:135-154` | `on_new_prach_results` / `on_new_pusch_results_{control,data}` / `on_new_pucch_results` / `on_new_srs_results` |
| U10 | indication 生成 | `lib/fapi_adaptor/phy/p7/phy_to_fapi_results_event_fastpath_translator.cpp` | PRACH→**RACH.indication**（`:53`）、PUSCH data→**CRC.indication**（`:231`）+**Rx_Data.indication**（`:275`、payload は span ゼロコピー）、PUSCH UCI→**UCI.indication**（`:170`）、PUCCH→UCI.indication（`:289-466`）、SRS→**SRS.indication**（`:491`）。RSSI/RSRP クランプは SCF-222 v4.0 §3.4.11 準拠のコメント付き |

### 2.3 「本物の L1」であることの確認（test-mode UE との対比）

- 生産コードのスタブは存在しない: test double は `tests/unittests/**` に限定（例 `tests/unittests/phy/upper/uplink_processor_test_doubles.h`）。生産系で "dummy" は無害な no-op notifier 等のみ（`upper_phy_impl.cpp:15-33`）。
- 一方 `du_high` には **test-mode UE**（`test_mode.test_ue`: RNTI 指定で MAC に合成 UE を常駐させ、固定 CQI/RI で PDSCH/PUSCH グラントを流し、`auto_ack_indication_delay` で UCI/CRC を**捏造**できる機能。`apps/units/flexible_o_du/o_du_high/du_high/du_high_config.h:1147-1184`、実装 `lib/du/du_high/test_mode/mac_test_mode_adapter.*`、例 `configs/testmode.yml`）がある。これは MAC 合成刺激であり、**本リグのソフトUE（srsUE、実波形を実際に復調する実 UE スタック）とは別物**。本リグは D1〜U10 の実信号処理を全て通す。

### 2.4 リアルタイム/スレッドと負荷感

- スレッドモデル: app 全体は単一 `main_pool`（優先度別 executor 4 本、`apps/services/worker_manager/worker_manager.cpp:334-347`; worker 数の自動導出 `:267-298` = `Σ(nof_dl_ant+nof_ul_ant) + nof_cells + 2`）。
- DU-low 並列度は「スレッド数」でなく**並列度上限**で指定: `max_pusch_and_srs_concurrency`（CPU 数から 1/2/2/4 を自動導出、`apps/units/flexible_o_du/o_du_low/du_low_config.h:100-114`）、`max_pdsch_concurrency`、`pdsch_cb_batch_length` 等（`du_low_config.h:99-152`）。上流 srsRAN の `nof_dl_threads/nof_ul_threads/nof_pusch_decoder_threads` という **YAML キーは存在しない**（内部 factory パラメータ名としてのみ残存）— 上流との明確な分岐。
- lower PHY 実行プロファイル: `single|dual|triple|blocking`（`apps/units/flexible_o_du/split_8/helpers/ru_sdr_config.h:68-96`、CPU 数で自動選択）。**`device_driver: zmq` の場合は `blocking`（sequential）に強制**され、PHY は単一ワーカーの非リアルタイムモードで回る（`ru_sdr_config_cli11_schema.cpp:297-299`、`worker_manager.cpp:395-412`、`dynamic_o_du_translators.cpp:48-60`）。
- `max_proc_delay`（DL パイプライン深さ、slot 単位）: 既定 5、範囲 1–30（`du_low_config.h:16-21`、`du_low_config_cli11_schema.cpp:183-188`）。モノリシック gnb では TDD→5 / FDD→2 を自動導出（`:403-416`）。**split6 du_low アプリでは明示必須**（`split6_o_du_low_unit_cli11_schema.cpp:76-80`）。
- 100MHz/30kHz の負荷感: リポジトリ内に SDR/ZMQ の 100MHz 実績 config は無い（100MHz 例は split-7.2 OFH の `configs/gnb_ru_ran550_tdd_n78_100mhz_4x2.yml` のみ）。`srate` は自動導出されず手動設定（既定 61.44MHz、`ru_sdr_config.h:119`; validator が `srate ≥ 帯域幅` を要求 `ru_sdr_config_validator.cpp:120-122`）。**見解:** ZMQ は blocking/sequential 強制のため、100MHz/30kHz（srate 122.88MHz）を srsUE と同居で実時間駆動するのは非現実的。機能検証は 10–20MHz から始めるのが妥当（§6-B）。

---

## 3. FAPI+ ⇔ SCF 5G-FAPI (222.x) 対応表

### 3.1 前提事実

- OCUDU の FAPI は **in-process・非シリアライズ**: 全メッセージが `span`/`static_vector`/`std::variant`/`std::optional`/`shared_transport_block` 等の native C++ 型（例 `tx_data_request.h:14-31`、`rx_data_indication.h:15-26`）。**バイト列 pack/unpack は全リポジトリに存在しない**（`pack|unpack|serialize|nfapi` 等の全域 grep で FAPI 該当 0 件。ヒットは OFH の eCPRI serdes・DCI ビット詰め `dci_packing.h`（3GPP の DCI ペイロード構築であり FAPI 枠組みではない）のみ）。
- 実装配置の分岐（上流 srsRAN 比）: `lib/fapi/` は**存在しない**。メッセージ定義+builder は header-only（`include/ocudu/fapi/{common,p5,p7}`）、コンパイルされるのは `lib/fapi_adaptor/` のみ。gateway/notifier の命名も P5/P7 分割の「fastpath」系に全面改称（旧 `slot_message_gateway`/`slot_data_message_notifier` 等は不存在）。上流の `message_bufferer` デコレータも**不存在**で、遅延許容は `nof_slots_request_headroom` + slot controller リング（サイズ headroom+1、`fapi_to_phy_fastpath_translator.h:124`/`.cpp:773`）で代替。
- 参照スペック: コメントは全域で **SCF-222 v4.0** を引用（例 `crc_indication.h:40`、`rach_indication.h:47`、`configuration_procedure.cpp:71`）。メッセージ ID enum は SCF と一致（`include/ocudu/fapi/common/error_indication.h:14-34`）。**要確認:** DUT が話す 222 のリビジョン（例 10.02/10.04、Aerial 拡張の有無）。

### 3.2 P5（config/制御）

| SCF 222 メッセージ (ID) | OCUDU FAPI+ | ブリッジでの向き | 対応状況・注意 |
|---|---|---|---|
| PARAM.request (0x00) | `fapi::param_request`（**空 struct**、`p5_messages.h:13`）→ `p5_requests_gateway::send_param_request`（`p5_requests_gateway.h:23`） | DUT→PHY | OCUDU 側は状態チェックのみ（split6 `configuration_procedure.cpp:71`、RUNNING 中はエラー） |
| PARAM.response (0x01) | `fapi::param_response`（**error_code のみ、capability TLV 無し**、`p5_messages.h:16`）→ `p5_responses_notifier::on_param_response` | PHY→DUT | **ブリッジが capability TLV 群を自前で合成する必要**（§4.4）。DUT が何を要求するか **要確認** |
| CONFIG.request (0x02) | `fapi::config_request{ cell_configuration }`（`p5_messages.h:21`、`cell_config.h:30-41`） | DUT→PHY | SCF の多数 TLV → 以下のコンパクト構造体へ写像: `scs_common`/`cp`/`pci`/`duplex`/`carrier_config`（dl/ul_bandwidth, dl/ul_f_ref_arfcn, dl/ul_grid_size, num_tx/rx_ant, dmrs_typeA_pos）/`rach_config_common`/`ssb_configuration`/`tdd_ul_dl_config_common`/`vendor_cfg(std::any)`。**注意:** SCF は周波数を kHz（dlFrequency 等）で運ぶ規定（**要確認**）に対し OCUDU は **ARFCN** — ブリッジで変換必須 |
| CONFIG.response (0x03) | `fapi::config_response`（error_code のみ） | PHY→DUT | split6 では CONFIG 内容が実際に cell 構築に使われる（§4.3）。モノリシック経路の `p5_requests_handler.cpp:53-67` は CONFIG 内容を無視して OK を返すだけ（split6 とは別実装、混同注意） |
| START.request (0x04) | `fapi::start_request` → `send_start_request` | DUT→PHY | split6: CONFIGURED 状態でのみ受理 → **session（RU+PHY）生成 → RUNNING → SLOT.indication 送出開始**（`configuration_procedure.cpp:99-126`）。SCF 同様 START.response は無し（成功は SLOT.indication 開始で表現。`start_response` struct は定義のみの dead code） |
| STOP.request (0x05) | `fapi::stop_request` → `send_stop_request` | DUT→PHY | RUNNING でのみ受理 → session 破棄 → CONFIGURED（`configuration_procedure.cpp:128-150`） |
| STOP.indication (0x06) | `fapi::stop_indication` → `p5_responses_notifier::on_stop_indication` | PHY→DUT | あり |
| ERROR.indication (0x07) | `fapi::error_indication`（`common/error_indication.h:37`; `out_of_sync` 時は `expected_slot` 付き）→ `error_indication_notifier` | PHY→DUT | あり（msg_invalid_state / msg_invalid_config / out_of_sync 等 `common/error_code.h:12`） |

### 3.3 P7（slot データ面）

| SCF 222 (ID) | OCUDU FAPI+ 構造体 | gateway/notifier | PDU/内容対応 |
|---|---|---|---|
| SLOT.indication (0x82) | `fapi::slot_indication{ slot_point_extended, time_point }`（`slot_indication.h:13`） | `p7_slot_indication_notifier::on_slot_indication`（PHY→DUT） | SCF は SFN(0–1023)+Slot。OCUDU は hyper-SFN 込み `slot_point_extended` → ブリッジで截断（逆方向の参考: split6 o_du_high 側は `slot_point_extender_adaptor` で拡張している） |
| DL_TTI.request (0x80) | `fapi::dl_tti_request`（`dl_tti_request.h:22`; PDU = `variant<dl_pdcch_pdu, dl_pdsch_pdu, dl_csi_rs_pdu, dl_ssb_pdu, dl_prs_pdu>`） | `p7_requests_gateway::send_dl_tti_request`（DUT→PHY） | PDCCH（DCI 複数/PDU、`dl_pdcch_pdu.h:45`）、PDSCH（CW 1、`dl_pdsch_pdu.h:39`）、NZP-CSI-RS、SSB、PRS。**ZP-CSI-RS は no-op で読み捨て**（translator `.cpp:191`）。precoding は `tx_precoding_and_beamforming_pdu`（`tx_precoding_and_beamforming_pdu.h:12`）= **PMidx 方式のみ**（生 BF 重み PDU は無い） |
| UL_TTI.request (0x81) | `fapi::ul_tti_request`（`ul_tti_request.h:22`; PDU = `variant<ul_prach_pdu, ul_pusch_pdu, ul_pucch_pdu, ul_srs_pdu>`） | `send_ul_tti_request` | PRACH（`ul_prach_pdu.h:13`）、PUSCH（data+UCI 部、`ul_pusch_pdu.h:43`; UCI part1→part2 対応表 `uci_part1_to_part2_correspondence.h:12` 埋込み）、PUCCH F0–F4（`ul_pucch_pdu.h:20-74`）、SRS（`ul_srs_pdu.h:18`） |
| UL_DCI.request (0x83) | `fapi::ul_dci_request`（`ul_dci_request.h:18`; `ul_dci_pdu` は `dl_pdcch_pdu` を内包） | `send_ul_dci_request` | あり（SCF と同じ独立メッセージ） |
| TX_Data.request (0x84) | `fapi::tx_data_request`（`tx_data_request.h:28`; PDU毎に `pdu_index`/`cw_index`/`shared_transport_block`） | `send_tx_data_request` | DL_TTI で保留された PDSCH に TB を添付（translator `.cpp:620-703`）。**TB は shared_transport_block（参照カウント付き）** — nvIPC バッファ寿命との整合が設計点（§4.5） |
| RX_Data.indication (0x85) | `fapi::rx_data_indication`（`rx_data_indication.h:23`; TB は `span<const uint8_t>`） | `p7_indications_notifier::on_rx_data_indication`（PHY→DUT） | **notifier コール中のみ有効な非所有 span** → ブリッジは即時に nvIPC バッファへコピー（§4.5） |
| CRC.indication (0x86) | `fapi::crc_indication`（`crc_indication.h:28`; tb_crc_status, ul_sinr_metric, TA, rssi, rsrp） | `on_crc_indication` | あり |
| UCI.indication (0x87) | `fapi::uci_indication`（`uci_indication.h:17`; `variant<monostate, uci_pusch_pdu, uci_pucch_pdu_format_0_1, uci_pucch_pdu_format_2_3_4>`） | `on_uci_indication` | HARQ/CSI-Part1/CSI-Part2（`uci_pdu_definitions.h:13-27`） |
| SRS.indication (0x88) | `fapi::srs_indication`（`srs_indication.h:32`; channel matrix / positioning report は optional） | `on_srs_indication` | あり（TA/チャネル行列。SCF の報告形式への詰め替えは **要確認**（DUT の期待 report type）） |
| RACH.indication (0x89) | `fapi::rach_indication`（`rach_indication.h:35`; occasion 毎 preamble 配列） | `on_rach_indication` | あり（負 TA の preamble は PHY 側で drop、results translator `.cpp:53`） |
| DL_TTI.response (0x8A) | **メッセージ構造体なし**（enum に ID のみ存在 `error_indication.h:24`） | — | **未対応**。DUT が DL_TTI.response を要求する設定なら不可（**要確認**） |
| （FAPIv3+ の各種 vendor/測定系: DL precoding matrix(weights), TB_CRC, measurement 等） | なし | — | 未対応（§6-A） |

### 3.4 タイミング規約（FAPI+ 側の確定事実）

- SLOT.indication は lower PHY の TTI boundary から `slot + nof_slot_tti_in_advance`（= `max_processing_delay_slots`）**先行**して発行される（`downlink_processor_baseband_impl.cpp:133` → `phy_to_fapi_time_event_fastpath_translator.cpp:33-38`）。つまり「SLOT.indication(N) を受けたら slot N の request を返す」という Aerial と同型の advance モデル（Aerial 側の advance 値は **要確認**）。
- request の受理窓は `[current − nof_slots_request_headroom, current]`（`fapi_to_phy_fastpath_translator.cpp:730-738`）。`nof_slots_request_headroom` は未指定時 `max_proc_delay` に自動設定（`du_low_config_cli11_schema.cpp:420`、CLI キーは `--max_request_headroom_slots`）。→ 既定で **max_proc_delay slot 分（TDD 5 slot = 2.5ms @30kHz）の遅延グレース**があり、DUT の応答ジッタに対して寛容。
- 各 slot の DL 完了通知として `p7_last_request_notifier::on_last_message(slot)`（`p7_last_request_notifier.h:15-24`）を**呼ぶ側は L2（＝ブリッジ）**。呼ばれると slot controller が解放され DL grid が確定する（`fapi_to_phy_fastpath_translator.cpp:713-716`）。呼ばれない場合も次 slot の DL_TTI で前 slot controller は解放される（`.cpp:296`）が、確定が 1 slot 遅れる。→ §4.6 の設計点。
- 同一 slot に同種メッセージ複数は未定義動作（`fapi_to_phy_fastpath_translator.h:87` に明記、`// :TODO: check the messages order` が `.cpp:473` に残存）。

---

## 4. ブリッジ設計（nvIPC FAPI slave = cuphycontroller 代役）

### 4.1 結論: 差し込み点は `split6_o_du_low_plugin`、コア改修ゼロ

- OCUDU 側の受け皿は完成済み: `apps/du_low`（`du_low.cpp:44` "runs the low part (PHY) of the split 6"）が P5 駆動の cell ライフサイクル（`configuration_procedure`: IDLE→CONFIGURED→RUNNING の FSM、`fapi_adaptor/configuration_procedure.h:19-21`）と session 生成（`split6_flexible_o_du_low_session_factory.cpp:57-101`）を持つ。session = { plugin 製 P7 adaptor + `o_du_low` + `radio_unit` + adapters }（`split6_flexible_o_du_low_session.h:23-65`）。
- インターフェース自体に「**This interface is used to integrate a third-party L2**」と明記（`mac_fapi_p5_sector_adaptor.h:19`、`mac_fapi_p7_sector_adaptor.h:22`）。
- OSS ビルドは dummy プラグインのみで**起動不能**（`split6_o_du_low_plugin_dummy.h:21` の validation false → `du_low.cpp:169-171` で fatal）。差し替えはコンパイル時: `#ifndef OCUDU_HAS_SPLIT6_ENTERPRISE` ガード（`split6_o_du_low_plugin_dummy.cpp:127-132`）+ static lib `ocudu_split6_o_du_low_plugin` の置換（`apps/units/flexible_o_du/split_6/o_du_low/CMakeLists.txt:19`）。動的ロード機構は無い（`dlopen` は radio driver 用のみ、`lib/radio/plugin_radio_factory.cpp:105-107`）。
- **したがって「standalone PHY 駆動のための改修」は不要**。必要なのは (a) `create_split6_o_du_low_plugin()` を提供する自作 plugin ライブラリ（`-DOCUDU_HAS_SPLIT6_ENTERPRISE` でビルドし dummy を置換）、(b) nvIPC ランタイムの導入、のみ。

### 4.2 実装すべきインターフェース（確定シグネチャ）

```
split6_o_du_low_plugin                      apps/units/flexible_o_du/split_6/o_du_low/split6_o_du_low_plugin.h:23
 ├─ on_parsing_configuration_registration(CLI::App&)      … nvIPC 設定(YAML)の追加
 ├─ on_configuration_validation() / on_loggers_registration()
 ├─ create_fapi_p5_sector_adaptor(fapi::p5_requests_gateway&, task_executor&, task_executor&)
 │     → mac_fapi_p5_sector_adaptor         include/ocudu/fapi_adaptor/mac/p5/mac_fapi_p5_sector_adaptor.h:20
 │        ├─ get_p5_responses_notifier()    … PHY→DUT: PARAM/CONFIG response, STOP.indication を nvIPC へ pack
 │        ├─ get_error_indication_notifier()… ERROR.indication → nvIPC
 │        └─ get_operation_controller()     … start(): nvIPC primary 初期化・RX ループ開始 / stop()
 ├─ create_fapi_p7_sector_adaptor_factory(task_executor&, task_executor&)
 │     → mac_fapi_p7_sector_adaptor_factory apps/.../fapi_adaptor/mac_fapi_p7_sector_adaptor_factory.h:23
 │        └─ create(const fapi::cell_configuration&, fapi::p7_requests_gateway&,
 │                  fapi::p7_last_request_notifier&, ru_controller&)   ← session 生成時に呼ばれる
 │           → mac_fapi_p7_sector_adaptor   include/ocudu/fapi_adaptor/mac/p7/mac_fapi_p7_sector_adaptor.h:23
 │              ├─ get_p7_slot_indication_notifier() … SLOT.indication → SCF 0x82 pack → nvIPC send
 │              ├─ get_p7_indications_notifier()     … RX_Data/CRC/UCI/SRS/RACH → pack → nvIPC
 │              └─ get_error_indication_notifier()
 └─ fill_worker_manager_config(worker_manager_config&)     … nvIPC RX スレッド等の登録
```

南向き（DUT→PHY）はブリッジの nvIPC RX ループが SCF wire メッセージを unpack し、
`p5_requests_gateway.send_{param,config,start,stop}_request`（`p5_requests_gateway.h:23-38`）と
`p7_requests_gateway.send_{dl_tti,ul_tti,ul_dci,tx_data}_request`（`p7_requests_gateway.h:23-38`）を呼ぶだけである。
FAPI+ 構造体の組み立てには in-tree の header-only builder（`include/ocudu/fapi/p7/builders/` 24 種: `dl_tti_request_builder.h`, `ul_pusch_pdu_builder.h` ほか）がそのまま使える。

### 4.3 P5 の流れ（cell 設定は FAPI から動的に取り込まれる）

- `configuration_procedure::send_config_request` が `cell_cfg = msg.cell_cfg` を保存（`configuration_procedure.cpp:152-158`）、`send_start_request` → `p5_operational_change_request_notifier::on_start_request(cell_cfg)` → `split6_flexible_o_du_low_session_manager`（`session_manager.h:18`）→ session factory が **FAPI の cell_configuration から** RU と o_du_low を構築する:
  - SDR RU の srate は FAPI の DL 帯域幅から導出（`session_factory.cpp:258-269,299`）。scs/duplex/cp/アンテナ数/grid size/ARFCN/PRACH/TDD パターンも全て FAPI 値から（`session_factory.cpp:114-183, 201-269`）。
  - YAML から来るのは expert 系（`max_proc_delay`【必須】, `nof_slots_request_headroom`, `allow_request_on_empty_uplink_slot`）、RU 選択（`ru_sdr`/`ru_ofh`）、metrics/ログのみ（`split6_o_du_low_unit_config.h:15-25`）。
- **含意:** DUT の CONFIG.request が実質の cell 設定源。ブリッジは SCF TLV → `fapi::cell_configuration` の写像を正しく行えばよく、OCUDU 側 YAML と DUT 設定の二重管理は最小化される（RU/ZMQ 設定と expert 系のみ YAML）。

### 4.4 PARAM.response の自前合成（ブリッジ責務）

OCUDU の `param_response` は error_code のみ（`p5_messages.h:16`）で capability TLV を持たないため、**DUT へ返す PARAM.response の TLV 群（対応 PDU、帯域、numerology、モード等）はブリッジが静的表として持つ**必要がある。内容は §6-A のカバレッジ表を機械化したものにする。DUT がどの TLV を必須とするかは **要確認**。

### 4.5 バッファ所有権（nvIPC ⇄ FAPI+）

| 方向 | OCUDU 側の型 | ブリッジの扱い |
|---|---|---|
| TX_Data.request（DUT→PHY） | `shared_transport_block`（参照カウント。PDSCH 処理完了まで生存が必要、`tx_data_request.h:24`） | 案A: nvIPC data buffer から**1 回コピー**して自前プールで保持（単純・推奨初期実装）。案B: nvIPC バッファを参照する custom deleter 付き shared_transport_block でゼロコピー（nvIPC の buffer release API と整合させる。**要確認**: nvIPC の rx buffer 解放規約） |
| RX_Data.indication（PHY→DUT） | `span<const uint8_t>`（notifier コール中のみ有効、`rx_data_indication.h:19`） | notifier 内で nvIPC の tx data buffer を allocate → コピー → send（コピー 1 回、必須） |
| その他 indication | 値埋め込み構造体 | 単純 pack |

### 4.6 スロットペーシングと `on_last_message`（最重要設計点）

- ブリッジは SLOT.indication(N) を nvIPC へ送出後、DUT からの slot N 向け request 群（DL_TTI → UL_TTI → UL_DCI → TX_Data の SCF 順序）を nvIPC RX で受け、gateway へ流す。
- OCUDU は「slot N のメッセージが出揃った」ことを `on_last_message(N)` で知る（in-tree MAC は `on_cell_results_completion` を契機に呼ぶ: `mac_to_fapi_fastpath_translator.h:62-71`）。**SCF 222 には対応する明示 end-marker が無い**ため、ブリッジのポリシーが必要:
  - 案A（推奨）: DUT の per-slot メッセージパターン（例: 常に DL_TTI+UL_TTI を送る、DL データ有時のみ TX_Data、等）を設定で宣言し、パターン完了時に `on_last_message` を呼ぶ。Aerial 系 L2 の送出パターンは **要確認**。
  - 案B: slot N+1 の最初のメッセージ受信（または nvIPC queue drain + 小タイムアウト）で N を close。
  - フォールバック: 呼ばなくても次 slot の DL_TTI が前 slot controller を解放する（`fapi_to_phy_fastpath_translator.cpp:296`）ため機能は保たれるが、DL 確定が遅れて late リスクが増える。
- 遅延耐性: 受理窓は headroom slot 分（既定 = max_proc_delay）。窓を外すと `OUT_OF_SYNC` の ERROR.indication（`expected_slot` 付き、`error_indication.h:37-42`）が DUT へ返る — DUT がこれをどう扱うか **要確認**。

### 4.7 nvIPC 側（cuphycontroller 代役）— 全て要確認（リポジトリ外）

- 役割: Aerial 標準構成では PHY 側（cuphycontroller）が nvIPC の **primary**（SHM リング/メモリプール生成側）、L2 が secondary として attach する構成が一般的（**要確認**）。本ブリッジは primary を担い、DUT の nvipc 設定（prefix 名、ring 長、msg/data buffer サイズ、CPU/GPU メモリ種別、通知方式 epoll/poll）と一致させる必要がある（**要確認**: DUT の nvipc yaml）。
- CPU-only 運用: nvIPC は SHM+CPU メモリのみで動作可能（GPU/CUDA 不要）という理解（**要確認**）。
- 入手性: nvIPC ライブラリと SCF FAPI wire 構造体ヘッダは Aerial SDK（cuBB）付属。DeepSig connector が再配布可能な形で同梱しているかは **要確認**（ライセンス確認必須）。

### 4.8 DeepSig `ocudu_aerial_fapi_connector` からの流用/相違（鏡像分析）

本セッションは GitHub アクセスが本リポジトリに限定されており DeepSig repo は未読。以下はタスク記載情報（`libaerial_bridge.a`: OCUDU C++ ↔ SCF C structs, nvIPC SHM）と一般知識に基づく整理で、**全項目 要確認**。

| 項目 | DeepSig（OCUDU=L2/master、cuPHY が PHY） | 本件（OCUDU=PHY/slave、DUT が L2） | 流用度 |
|---|---|---|---|
| nvIPC トランスポート層（初期化、リング送受、通知） | secondary（L2）側 attach | **primary（PHY）側 create** — 初期化パスが逆 | コードの骨格は流用可、役割設定は書き換え（低工数） |
| SCF wire 構造体定義・enum・スケーリング定数 | 保有 | 同一物が必要 | **高**（そのまま流用候補） |
| request 系変換（DL_TTI/UL_TTI/UL_DCI/TX_Data） | FAPI+ → SCF に **pack**（送信側） | SCF → FAPI+ に **unpack**（受信側） | field 対応表（知識）は 100% 共有、コードは鏡写しに書き直し |
| indication 系変換（SLOT/RACH/CRC/RX_Data/UCI/SRS/ERROR） | SCF → FAPI+ に unpack（受信側） | FAPI+ → SCF に pack（送信側） | 同上 |
| SLOT.indication の生成 | cuPHY が生成（受けるだけ） | **本件は OCUDU PHY のティックから自前生成** | net-new（ただし OCUDU 側 notifier に既製、§3.4） |
| P5 応答（PARAM/CONFIG.response）の生成 | cuPHY が生成 | **ブリッジが合成**（§4.4） | net-new |
| バッファ所有権 | L2 が TB を渡す側 | PHY が TB を受ける/返す側 | 方向逆転、net-new |

**見解:** DeepSig connector は「OCUDU FAPI+ ⇔ SCF C structs の対応関係が実運用で成立する」ことの実証であり、変換辞書（フィールド対応・単位系・スケーリング）と nvIPC 統合ノウハウの参照価値が高い。一方でコードの主要部（pack/unpack の向き、エンドポイント役割、indication 生成）は逆向きのため、「fork してすぐ動く」類ではなく「対称形を書き起こす」作業になる。

---

## 5. E2E 起動手順（0 → DUT + ソフトUE 疎通）

### 5.1 ビルド

```bash
# 1) OCUDU（ZMQ radio は既定 ON: CMakeLists.txt:66 ENABLE_ZEROMQ）
#    依存: libzmq, fftw3f, mbedtls/openssl, sctp ほか
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j --target gnb odu_low   # odu_low = apps/du_low（split_6 unit は EXCLUDE_FROM_ALL だが
                                              #  apps/du_low が明示リンクするため一緒にビルドされる:
                                              #  apps/du_low/CMakeLists.txt:33）
# 2) ブリッジ plugin:
#    - 自作 split6_o_du_low_plugin 実装（nvIPC 終端）を static lib 化し
#      ocudu_split6_o_du_low_plugin（apps/units/flexible_o_du/split_6/o_du_low/CMakeLists.txt:19 の
#      dummy 版）を置換、全体を -DOCUDU_HAS_SPLIT6_ENTERPRISE 付きでビルド
#    - nvIPC ランタイム（Aerial SDK / DeepSig 由来。要確認）をリンク
# 3) srsUE: srsRAN_4G を ENABLE_ZEROMQ=ON でビルド（リポジトリ外・要確認）
# 4) Open5GS: パッケージ or ソース（要確認）
```

### 5.2 設定の要点

- `du_low` アプリ YAML（in-tree に ZMQ 例は無いため新規作成）:
  - `ru_sdr:` `device_driver: zmq`, `srate: <MHz>`, `device_args: tx_port=tcp://127.0.0.1:2000,rx_port=tcp://127.0.0.1:2001`（アンテナ毎にカンマ区切りで追加。`ru_sdr_config_translator.cpp:106-144`。`tcp://` のみ受理: `radio_config_zmq_validator.cpp:43`。tx/rx ストリーム数一致必須 `:129`）
  - `expert_phy:` `max_proc_delay: <N>`（**必須**: `split6_o_du_low_unit_cli11_schema.cpp:76-80`）
  - ZMQ 選択で lower PHY は blocking/sequential に自動設定、TX/RX gain 0dB（`ru_sdr_config_cli11_schema.cpp:289-309`）
- srsUE 側（**要確認**、上流 srsRAN_4G）: `device_name: zmq`, `device_args: tx_port=tcp://*:2001,rx_port=tcp://localhost:2000,base_srate=<Hz>`（gNB の `srate` と**完全一致**が前提 — OCUDU 側はリサンプリングせず要求分をそのまま流す transmit-on-request 型のため。ソケット対称性: OCUDU TX=REP bind ⇔ UE RX=REQ connect / UE TX=REP ⇔ OCUDU RX=REQ connect（OCUDU 側: `radio_session_zmq_impl.cpp:52,73`））。タスク文中の「4ms prefill」は srsRAN_4G UE 側実装の挙動であり本リポジトリからは確認不能（**要確認**。OCUDU 側の実測事実: TX circular buffer 既定 614,400 samples、未接続時 zero 送出で前進 `radio_zmq_tx_channel.cpp:278-285`）
- **cell パラメータの一致**: 帯域/SCS/ARFCN/PRACH/TDD は DUT の CONFIG.request が源（§4.3）なので、**DUT 設定 ⇔ srsUE 設定**を一致させる（OCUDU 側 YAML に cell 設定は無い）。srsUE の対応範囲（帯域幅・band・PRACH format）は **要確認**
- Open5GS: PLMN/TAC を DUT に合わせ、srsUE の USIM（IMSI/K/OPc）を subscriber 登録（**要確認**）

### 5.3 起動シーケンス

```
(1) Open5GS 起動（AMF が N2 待受）                          【要確認】
(2) du_low + ブリッジ起動
     - plugin の operation_controller.start() で nvIPC primary 初期化（SHM 生成）
     - P5 FSM は IDLE で待機（SLOT.indication はまだ流れない）
(3) DUT 起動 → nvIPC attach → PARAM.request/response →
    CONFIG.request（→ fapi::cell_configuration 保存、CONFIGURED）→
    START.request（→ session 生成 = RU(ZMQ)+upper/lower PHY 起動、RUNNING）
     - 以後 500µs 毎（30kHz 時）に SLOT.indication が DUT へ流れ、
       DUT が DL_TTI/UL_TTI/UL_DCI/TX_Data を返し始める（SSB が ZMQ に出る）
(4) srsUE 起動（別 netns 推奨）→ ZMQ 接続 → SSB 検出 → PRACH 送信
     → RACH.indication が DUT へ → RAR/Msg3... → RRC Setup → NAS Registration
     → PDU Session Establishment（DUT↔Open5GS、要確認）
(5) 疎通確認: ip netns exec ue1 ping <UPF/DN> / iperf3
```

停止は逆順（DUT の STOP.request で session 破棄 → CONFIGURED に戻る。`configuration_procedure.cpp:128-150`）。

### 5.4 必要リソース

- **GPU 不要**（software PHY は SIMD 最適化 CPU 実装のみで完結。ZMQ 経路にハード依存なし）。サーバー 1 台で DUT + ブリッジ + du_low + srsUE + Open5GS を同居可能（netns で UE を分離）。
- CPU: 20MHz/1x1 なら経験的に 8〜16 物理コア級で余裕（**見解**。in-tree の自動導出式: worker 数 = `Σ(dl_ant+ul_ant) + nof_cells + 2`、`worker_manager.cpp:283-292`）。ZMQ 時は lower PHY blocking のため実時間保証はなく、UE/gNB が互いに引っ張り合う準実時間で動く。
- AVX2 以上の x86（AVX-512 あれば有利）。

---

## 6. 投資判断サマリ

### 6-A. カバレッジ（DUT のどこまで叩けるか）

**叩ける（= OCUDU software PHY + split6 が現に実装**）:

| 面 | 対応内容（根拠） |
|---|---|
| DL | SSB/PBCH、PDCCH（polar）、PDSCH（LDPC、~256QAM、CW1、PMidx precoding、DM-RS/PT-RS）、NZP-CSI-RS、PRS（`downlink_processor_multi_executor_impl.cpp:54-204`） |
| UL | PRACH（long/short、ZC 相関検出）、PUSCH（MMSE、LDPC、UCI on PUSCH、CSI part1/part2）、PUCCH F0–F4、SRS（channel matrix / positioning report）（§2.2） |
| indication | SLOT / RACH / CRC / RX_Data / UCI / SRS / ERROR（§3.3） |
| 手続き | PARAM/CONFIG/START/STOP の P5 FSM、slot 毎 P7 サイクル、HARQ softbuffer（`rx_buffer_pool_impl.cpp`）、遅延グレース（headroom） |

→ **DUT の MAC スケジューラ、HARQ 制御、RA 手続き、UCI 処理、TB 組立、RRC/上位、これら L2/L3 ロジックの実波形での機能検証は成立する**（ソフトUE が実際に復調・応答するため、test-mode のような自己完結刺激では見えない DUT バグ — タイミング整合、UCI 解釈、再送制御 — を検出できる）。

**未対応/制約（DUT のこの機能は叩けない）**:

- split6 アプリの実装制約: **1 cell**（`split6_constants.h:25`）、**PRACH 1 port**（`:19`）、**PUSCH 1 layer**（`:22` — UL MIMO は gnb 側にはあるが split6 経路は現状 single layer）、**TX 4 antenna まで**（`:28`）、**FR1 のみ**（`:31`）。
- FAPI 面: DL_TTI.response 無し、PARAM.response の capability TLV 無し（ブリッジ合成）、**BF 重み（BFW）搬送 PDU 無し**（precoding は PMidx→repository のみ）、ZP-CSI-RS は no-op、MU-MIMO 系/FAPIv4 拡張 PDU 無し、同一 slot 同種メッセージ複数は未定義（§3.4）。
- PHY 能力: lower PHY は SCS 15/30/120kHz のみ（`lower_phy_factory.cpp:70-76`）。
- **要確認**: DUT が必須とする Aerial 拡張（vendor メッセージ、GPU メモリ渡し、特定 TLV）があれば個別対応が要る。

### 6-B. 限界

1. **タイミング**: Aerial 実機は PTP/GPS 同期の厳格な slot cadence（SLOT.indication の N−advance 応答規律）を持つ（**要確認**）のに対し、本リグは ZMQ の**要求駆動・準実時間**（timestamp はサンプルカウンタ擬似、`read_current_time()` は 0 固定 `radio_session_zmq_impl.cpp:183`）。OCUDU 側の受理窓（headroom、既定 = max_proc_delay）が緩衝するが、**DUT 側が壁時計基準の watchdog を持つ場合は破綻し得る**（**要確認**）。SLOT.indication の cadence は UE がサンプルを引く速度に律速される点も Aerial と異なる。
2. **スケール**: 100MHz/30kHz（srate 122.88MHz）の software PHY + ZMQ + srsUE 同居実時間は非現実的（**見解**; in-tree にも SDR 100MHz 例は無い）。現実解は 10–20MHz へ帯域縮小（DUT の設定可否 **要確認**）。スループット絶対値・多UE容量・レイテンシ実測は本リグの目的外。
3. **性質**: 本方式は**機能検証**である。GPU 実時間性能・cuPHY 等価性・beamforming/massive MIMO・スケール検証は対象外（それらは Aerial 実環境でのみ検証可能）。

### 6-C. 工数（流用 vs net-new、低/中/高）

| 作業項目 | 区分 | 工数レベル | 根拠 |
|---|---|---|---|
| standalone PHY 駆動（MAC 無し起動） | **流用（完成済み）** | **ゼロ** | `apps/du_low` + split6 session 機構が in-tree（§4.1）。プラグイン以外の本体改修不要 |
| software PHY / ZMQ / srsUE / 5GC 土台 | 流用 | 低（構築・設定のみ） | 全て既存 OSS。ZMQ 例 YAML が無いので config 起こしは必要 |
| plugin 骨格（CMake 置換、CLI/logger、executor 配線） | net-new | 低 | dummy 実装（`split6_o_du_low_plugin_dummy.cpp` 全 132 行）が正確なテンプレート |
| nvIPC 統合（primary 役、SHM/リング、RX ループ） | net-new（DeepSig/Aerial 参照） | 中 | ライブラリは既製（**要確認**）。役割逆転（secondary→primary）は設定+初期化パスの書換えで、DUT の nvipc 設定一致のすり合わせに実機デバッグを要す |
| SCF wire 構造体・enum 定義 | 流用（DeepSig/Aerial SDK） | 低 | 定義流用（ライセンス **要確認**） |
| P7 request unpack（DL_TTI/UL_TTI/UL_DCI/TX_Data、全 PDU 型 → builder 呼出し） | net-new（変換辞書は DeepSig の逆向き知識を流用） | **中〜高** | 最大ボリューム。PDU 種 ×フィールド対応 ×単位変換。builder が header-only で揃っており（§4.2）片側は既製 |
| indication pack（SLOT/RACH/CRC/RX_Data/UCI/SRS/ERROR） | net-new | 中 | 構造体は素直（§3.3）。UCI の bit 詰め・スケーリング（SINR 等の単位系）に注意 |
| P5（PARAM.response 合成、CONFIG TLV→cell_configuration、kHz⇔ARFCN） | net-new | 中 | 有限マッピング（`cell_config.h` はコンパクト）。DUT の必須 TLV 集合が不確定要素（**要確認**） |
| slot ペーシング / on_last_message / late 耐性チューニング | net-new | 中〜高 | §4.6 の設計点。DUT 実機との統合デバッグが主コスト。フォールバック挙動があるため段階的に詰められる |
| E2E 統合（DUT↔ブリッジ↔PHY↔srsUE↔5GC） | 統合 | 中 | RACH→attach→PDU session の突合せ。srsUE/Open5GS はコミュニティ実績が厚い定番構成 |

**総括（見解）:**
- 位置づけはタスクの仮説どおり「**前例（DeepSig 鏡像）と標準 SCF FAPI/nvIPC の存在により greenfield より明確に軽い。しかし config では済まない**」。さらに本調査で判明した追い風は、(i) OCUDU が split6 で**この統合形態を第一級サポート**しており（"third-party L2" 明記、P5 駆動 cell 生成まで完備）、ブリッジの置き場所・境界・builder が全部用意されていること、(ii) FAPI+ のメッセージ ID/意味論が SCF-222 v4.0 に整合しており変換が素直なこと。
- 主リスクは OCUDU 側ではなく **DUT/nvIPC 側の不確定要素**（nvipc 設定、必須 TLV、vendor 拡張、タイミング watchdog — いずれも 要確認）と、変換辞書の網羅テスト。
- 規模感（見解）: FAPI/5G PHY 経験者 1 名換算で、最小疎通（SSB→RACH→attach、20MHz/1x1）までブリッジ実装 2〜3 人月 + DUT 統合デバッグ 1〜2 人月のオーダー。DeepSig コード（変換辞書・nvIPC 層）が想定どおり流用できれば下振れ、DUT の Aerial 拡張依存が強ければ上振れ。

---

## 付録 A. 上流 srsRAN Project との確認済み分岐（本調査範囲）

| 項目 | 上流 srsRAN（参考） | OCUDU（確認事実） |
|---|---|---|
| namespace / ライブラリ名 | `srsran`, srslog, srsvec | `ocudu`, ocudulog, ocuduvec |
| FAPI 実装 | `lib/fapi`（validators/builders/decorators, message_bufferer） | `lib/fapi` 無し。header-only（`include/ocudu/fapi/{common,p5,p7}`）+ `lib/fapi_adaptor` のみ。message_bufferer 無し（headroom リングで代替） |
| FAPI I/F 名 | slot_message_gateway / slot_{time,data,error}_message_notifier / last_message_notifier | P5/P7 分割 fastpath 系: `p5_requests_gateway`/`p7_requests_gateway`/`p7_indications_notifier`/`p7_slot_indication_notifier`/`p7_last_request_notifier` |
| PHY スレッド設定 | `nof_dl_threads`/`nof_ul_threads` 等の YAML キー | 廃止。並列度上限方式（`max_pusch_and_srs_concurrency` 等）+ 単一 main_pool |
| split6 DU-low | （商用 plugin 前提の split6 あり） | session ベースに再設計: P5 CONFIG/START が cell を動的生成する `apps/du_low` + `OCUDU_HAS_SPLIT6_ENTERPRISE` プラグイン境界 |
| ZMQ 例 config | あり（tutorial） | in-tree 無し（driver は完備） |

## 付録 B. 本調査の未確認事項一覧（要確認）

1. DUT: 話す SCF 222 リビジョン、必須 PARAM/CONFIG TLV、per-slot メッセージ送出パターン、DL_TTI.response 要否、タイミング watchdog、帯域縮小可否、nvipc 設定（prefix/ring/buffer サイズ/通知方式）、北側（NGAP/CU）構成。
2. nvIPC: primary/secondary の生成規約、CPU-only モード、buffer 解放 API、再配布ライセンス。
3. DeepSig connector: 実コード構成、`libaerial_bridge.a` の変換網羅度、SCF ヘッダ同梱の有無、ライセンス。
4. srsUE: ZMQ 実装詳細（プリフィル挙動）、NR SA の帯域/型式対応範囲、必要 config。
5. Open5GS: バージョン・設定詳細。
