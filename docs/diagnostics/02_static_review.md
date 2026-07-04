# フェーズ1: 静的レビュー — 仮説A・1〜10 の裏取り結果

前提の実測アンカー(rank1・MCS27・報告SNR≈31.2dB・RSRP≈-35dBm で UL BLER 18〜29%、MCS 据え置き)
に対し、本リポジトリのコードと、リポジトリ同梱の PUSCH BLER 統合テスト
(`tests/integrationtests/phy/upper/channel_processors/pxsch_bler_test`)による実証実験で裏を取った。

## 0. 実証実験(このリポジトリのバイナリで実測)

構成: 4 Rx ポート・273 PRB・MCS27(qam64 テーブル)・ZF 等化・FD=filter・
sinr=post_equalization(=実機 gNB の既定と同一)。200 スロット。

| 実験 | 設定 | 結果 |
|---|---|---|
| E1: クリーン基準 | single-tap, rank1, SINR 31dB | BLER 0%, 報告SINR 31.5dB, EVM 2.9% |
| E2: 所要 SINR 掃引 | single-tap, rank1, SINR {15,16,17,18,20} | **18dB: BLER 100% / 20dB: BLER 0%** → 崖 ≈ **19.2±0.5dB** |
| E3: 局所 RE 破壊 | rank1, SINR 31dB, 6 RE/シンボル に 90°回転 | **報告SINR 26.4dB・EVM 3.2% のまま BLER 21%**, LDPC max-iter 頻発 |
| E4: rank2 無相関 | TDLA/rayleigh, 2レイヤ, SINR 31dB | BLER 0.5%, 報告SINR 25.8dB(レイヤ分離ペナルティ ≈5dB) |
| E5: rank2 深フェード | TDLA/butler(直交ビーム+深い周波数選択フェード), 2レイヤ, SINR 31dB | 報告SINR **18.3dB に低下(正直に報告される)**, BLER 100%, cond≈0dB |

### 実験から確定する診断ロジック
1. **MCS27 の実際の所要 post-eq SINR は ≈19.2dB**。スケジューラの SNR→MCS テーブルは
   16.04dB(`mcs_calculator.cpp:64`)で **約3dB 楽観**(テーブルは 20MHz SISO ZMQ 校正、
   `mcs_calculator.cpp:41-54` に「暫定」と明記)。
2. 現場アンカー(BLER 18-29%)は LDPC 崖の縁 = **実効 SINR ≈ 19dB 前後**を意味する。
   報告 31.2dB との差 **≈12dB が「post-eq SINR に映らない劣化」**。
3. 「報告 SNR 高いまま BLER 高」を再現できたのは **データ RE の局所破壊(E3)** のみ。
   レイヤ分離劣化・深フェード(E4/E5)や白色雑音(E2)は報告 SINR に正直に映る
   (ZF/MMSE の post-eq 雑音分散は 1/det を含むため、ill-conditioning も自己申告される)。
   → 現場の rank1 異常は「DMRS ベースの雑音分散推定に乗らない劣化」
   (時間方向の位相ドリフト、バースト/狭帯域干渉、窓ズレ ISI、RU 内部のインパルス性障害)が第一容疑。
4. E5 より: 空間相関で rank2 が崩れる場合は報告 SINR も落ちるので、リンクアダプは
   MCS を下げて追従できるはず。rank2 で「SNR 高いまま崩れる」なら別要因(E3 型)が疑い。

---

## 仮説A【最優先】報告 SNR vs 実効 SINR のギャップ + リンクアダプ

**判定: (a)(b) 両方が成立。構造的に説明が閉じた。**

- (a) 受信機の SNR 報告系: 既定の `pusch_sinr_calc_method = "post_equalization"`
  (`du_low_config.h:34`)。post-eq SINR は「チャネル推定器が DMRS 残差から出した雑音分散」を
  等化重みで写像した量(`pusch_demodulator_impl.cpp:355-358,410-415`)。
  **DMRS シンボルに現れない劣化(データシンボルの位相回転・バースト干渉・窓ズレ)は原理的に含まれない**(実証 E3)。
  EVM ベース SINR(`sinr_evm = -20log10(EVM) - 3.7`, `pusch_processor_notifier_adaptor.h:136,148`)は
  データシンボル実測なのでこれらを含むが、**既定では計算すらされない**
  (`upper_phy_factories.cpp:785-786`: sinr法=evm か PHY debug ログ時のみ有効)。
- (b) リンクアダプ: UL OLLA は既定で有効(`scheduler_expert_config.h:185-189`:
  inc=0.001, target BLER=0.01, **max offset=±5.0dB**)。
  `get_effective_snr() = 瞬時 pusch_snr + olla_offset`(`ue_link_adaptation_controller.cpp:93-96`)。
  報告 31.2dB に対し飽和 -5dB でも 26.2dB ≥ MCS28 閾値 16.59dB → **MCS は下がりようがない**。
  OLLA は「テーブル誤差の微修正」設計であり、12dB 級の系統誤差は吸収不能。
  さらに `handle_ul_crc_info` は「送信 MCS ≠ OLLA 提案 MCS」のとき更新をスキップ
  (`:65-67`)— 码率 0.95 制約で MCS を下げられたスロット(`ue_cell_grid_allocator.cpp:673-705`)では
  学習も止まる。
- 観測の「mcs 27」: metrics の `ul_mcs` はスケジュール済 MCS の平均
  (`cell_metrics_handler.cpp:562,652-653`)。SNR 31→MCS28 選択後、UCI 多重や码率制約で
  27 に下げられるスロットが混ざる平均値として整合。

**帰結**: 現場アンカーは [CODE/観測] 「post-eq SINR がデータシンボル劣化を見ない」+
[CODE/設計] 「OLLA クランプ ±5dB」の複合。**MCS を下げさせる**だけなら
`pusch_sinr_calc_method: evm`(設定変更のみ)か OLLA クランプ拡大で対処可能だが、
**根本は 12dB 劣化源の特定**であり、それにはフェーズ2 計装(三者 SINR 同時ログ)が必要。

改善案(効果順):
1. [計装] sinr_ch_est / sinr_eq / sinr_evm を常時1行ログ(フェーズ2 実装)。
2. [CONFIG 実験] `expert_phy: pusch_sinr_calc_method: evm` — LA がデータシンボル
   実測 SINR を見るようになる(EVM 計算コスト小: 復調時の距離のみ)。
3. [CONFIG] `pusch: olla_max_snr_offset: 10.0`, `olla_target_bler: 0.05` 等の緩和。
4. [CODE 提案] SNR→MCS テーブルの 100MHz 再校正(+3dB シフト)or テーブルの帯域幅依存化。

## 仮説1 MMSE-IRC 不在(4Rx の余剰 DoF)

**判定: 事実。ただし「rank1 で SNR 高いまま BLER 高」の単独犯にはならない。**

- 等化器は ZF/MMSE の 2 種のみ(`channel_equalizer_generic_impl.cpp:487-504`)。
  複数レイヤ時の雑音は **全ポート最悪値のスカラー1個**:
  `:528 noise_var = *std::max_element(noise_var_estimates...)`。
  干渉共分散行列の推定・白色化(IRC)はリポジトリ全体に存在しない。
- 定常な広帯域干渉なら DMRS 残差にも乗る → noise_var ↑ → 報告 SNR ↓(アンカーと不整合)。
  **時間バースト/周波数局所の干渉は DMRS を外れれば報告 SNR に映らない**(E3 と同型)→ 整合。
- 2レイヤ時の実害: (i) IRC 不在で干渉下のレイヤ分離が劣化、
  (ii) 既定 ZF(`du_low_config.h:58`)は ill-conditioned チャネルで MMSE より雑音強調が大きい
  (E5: rank2 深フェードで post-eq SINR 18dB まで崩落。低 SINR RE では MMSE の方が数 dB 有利)。
- 改善案:
  1. [CONFIG・即効] `expert_phy: pusch_channel_equalizer_algorithm: mmse`(コード変更ゼロ)。
  2. [CODE 提案・中規模] MMSE-IRC: DMRS 残差からポート間共分散 R を推定し
     W = (H^H R^{-1} H + I)^{-1} H^H R^{-1}。挿入点は `channel_equalizer_generic_impl.cpp`
     (等化器 API は noise_var_estimates を span で受けるため、共分散への拡張は
     `dmrs_pusch_estimator` の残差保存と合わせて設計要)。フェーズ3 では設計のみ提示。

## 仮説2 チャネル推定 FD 戦略

**判定: 設定プラミングのバグを確認(dead knob)。ただし既定測定への実害なし。**

- `lib/phy/upper/upper_phy_factories.cpp:568-574`:
  ```cpp
  if (config.pusch_channel_estimator_td_strategy == "none") {   // ← 本来は fd_strategy
    ... = fd_smoothing_strategy::none;
  } else if (config.pusch_channel_estimator_td_strategy == "mean") {  // ← 同上
  ```
  FD 戦略の選択が **TD 戦略の文字列**を参照。td は CLI 検証で "average"/"interpolate" のみ
  許容(`du_low_config_cli11_schema.cpp:170-175`)のため、**fd は常に filter 固定**。
  `pusch_channel_estimator_fd_strategy: mean|none` は無言で無視される。
- 既定(fd=filter, td=average)では偶然正しい動作になるため**現行測定の BLER 原因ではない**。
  ただし切り分け実験(fd=none で filter 起因を除外する等)をブロックする。→ **パッチ#1(修正済み)**。
- filter 実装自体(`port_channel_estimator_helpers.cpp:192-233`)は RC ローパス+仮想パイロット
  外挿で、エッジ PRB の系統誤差対策も入っており妥当。
- 追加発見(同ファイル群): `port_channel_estimator_average_impl.cpp:222` の連続マスク判定が
  `nof_active == end - begin`(正しくは `end + 1 - begin`)。
  (i) 連続割当で高速パスが**常に不成立** → 全 PUSCH シンボル×ポート×レイヤで無駄コピー(性能)。
  (ii) 「穴が丁度 1 個」のマスクが来た場合に誤って連続扱い→ サイズ不一致
  (Release はアサート無効のためバッファ不整合の潜在リスク)。→ **パッチ#2(修正済み)**。

## 仮説3 DMRS ポート/OCC(2レイヤ)

**判定: 仕様準拠。主要因ではない。**

- OCC テーブル(`dmrs_helper.cpp:21-45`)は TS 38.211 Table 6.4.1.1.3-1(type1)/-2(type2)
  準拠(port 1000/1001 = 同一 CDM グループ, FD-OCC {+1,+1}/{+1,-1})。
- CDM 分離は `average_pairs()`(`port_channel_estimator_average_impl.cpp:743-761`)=
  隣接 DMRS RE ペア平均。前提は「ペア 2 RE(60kHz 間隔)でチャネル一定」。
  遅延スプレッド τ に対し位相回転 2π·60kHz·τ。近距離 OTA(τ≲300ns)なら誤差 ≲7°で軽微。
  大遅延環境では 2レイヤのみ劣化する系統誤差になる(rank1 は非適用: `:596-598` 条件)。
  → レポータでは「rank2 のみ悪化 & t_align/遅延大」で注意喚起。

## 仮説4 RI(rank)選択の楽観性

**判定: 実装は楽観バイアスあり(確認)。max_rank:2 実験時の要注意点。**

- `ue_channel_state_manager.cpp:87-137 get_nof_ul_layers()`:
  SRS から TPMI/SINR を計算する際、**雑音分散を「受信電力-30dB」と固定仮定**(`:69-71`)。
  受信 SNR が 31dB 環境では常に「30dB SINR のきれいなチャネル」に見える。
  さらに min-layer-SINR > 18dB で即採用、平均比較には +2dB の高 rank 優遇(`:107-131`)。
- E5 実証: 実効 18dB(BLER100%)まで崩れるチャネルでも、SRS ベース判定(-30dB 雑音仮定)では
  rank2 が通り得る。
- rank 品質の実測(condition number)はどこにも計算されていない。
- 改善案: [CODE 提案] SRS 雑音を実測化 or 閾値 config 化 / cond-number ゲート追加。
  短期は [CONFIG] max_rank=1 固定で回避(現行コンフィグ通り)。

## 仮説5 OLLA/リンクアダプ

仮説A(b) に統合。追加の実装事実:
- OLLA は rank 非依存の 1 オフセット(rank1/rank2 で所要 SNR が違うため、rank 切替時に
  過渡 BLER が出る設計制約)。
- `pusch_rv_sequence` 既定 {0}(`scheduler_expert_config.h:142`)→ **再送が常に rv0 =
  Chase 合成のみで IR 利得なし**。BLER 20% 環境ではスループット回復力に直結。
  [CONFIG] `pusch: rv_sequence: [0,2,3,1]` を推奨。
- DL 側 OLLA も同構造(CQI 補正 ±4、target BLER 1%)。DL BLER 実測 4-8% は
  「CQI 楽観 + クランプ到達」か「更新スキップ」かを DL 計装(dl_olla ログ)で判別可能。

## 仮説6 LLR/デマップ

**判定: 健全(主要因ではない)。**

- LLR は float 計算 → ±`RANGE_LIMIT_FLOAT=20` で飽和 → int8 量子化
  (`demodulation_mapper_qam64.cpp:27-28,147-150`)。雑音分散スケーリングは
  RE 毎の post-eq noise var を使用(`pusch_demodulator_impl.cpp:385`)。
- E2 で single-tap の所要 SINR 19.2dB は 64QAM r=0.926 の理論値(≈18.5-19.5dB)と整合 →
  デマップ〜LDPC チェーンに系統損失なし。
- 注意点のみ: 報告 noise var が過小(未モデル劣化)だと LLR が過大確信になり
  HARQ 合成の重み付けが歪む(二次的影響)。

## 仮説7 UL IQ 圧縮(BFP9/STATIC/exponent4)

**判定: 実装は正しい。「SNR 高報告」の犯人にはなりにくい(量子化雑音は DMRS にも乗るため)。**

- static compression header は「セクション毎 udCompHdr の省略」のみで、
  **PRB 毎の BFP exponent(udCompParam)は常にペイロード内に存在し毎 PRB 解釈される**
  (`ofh_uplane_message_decoder_static_compression_impl.cpp:10-19`,
   `iq_compression_bfp_impl.cpp:89-113`)。DU 側に「exponent 固定値」の設定は存在しない。
  → RU の M-Plane 値 "exponent 4" は RU 送信側の代表値であり、DU はワイヤ上の値に従う。整合。
- 9bit BFP の SQNR ≈ 50dB(フル活用時)。**ただし RU のデジタルゲインが低く
  マンティッサ実効ビットが少ない場合は SQNR がそのまま下がる**。
  この場合も量子化雑音は白色で DMRS 残差に乗る → noise_var ↑ → 報告 SNR ↓(アンカーと不整合)。
  つまり BFP 劣化なら「報告 SNR 自体が低い」群に現れる。
- レポータ: EPRE(グリッド dBFS 相当)が極端に低い(< -60dBFS 等)場合に
  [CONFIG/RU] 「RU ゲイン/exponent 設定を確認」を出すルールを実装。

## 仮説8 Rx 4 ブランチ間アンバランス

**判定: ゲイン/位相の静的アンバランスはチャネル推定が吸収する(BLER 直結しない)。
計装で即時に白黒つく。**

- ポート毎に独立推定(`dmrs_pusch_estimator_impl.cpp:51-66`)→ 複素ゲイン差は H に吸収。
  MRC/ZF/MMSE は H を使うため自動補正。
- 実害が出るのは (i) 1 ブランチが雑音支配(故障)− 1xN は port 除外ロジックあり
  (`equalize_zf_1xn.h:123`)、**2xN は max(σ²) スカラー化のため全体の SINR「過小」報告**
  (`channel_equalizer_generic_impl.cpp:528`)→ MCS が下がる方向で、アンカーとは逆。
  (ii) 時変の位相ホッピング(校正イベント)− E3 型(SNR 高いまま BLER)を作り得る。
- 既存データで観測可能: `port_rsrp_dB[4]` が CSI に既にあり(`channel_state_information.h:291`)、
  PHY debug ログに `rsrp=[a,b,c,d]dB` で出る。フェーズ2 でポート別 noise_var も追加。

## 仮説9 OTA 残留 CFO・位相ドリフト・推定の陳腐化

**判定: 「報告 SNR に映らない劣化」の有力機構その1(E3 と同族)。計装で判別可能。**

- CFO はスロット内 DMRS 2 シンボル間位相から推定し全シンボルに一次補償
  (`port_channel_estimator_average_impl.cpp:468-525,182-186`、既定 compensate_cfo=true)。
- td=average(既定)は DMRS 間の時間変動を平均化。**CFO 推定誤差・位相雑音・
  ドップラーによる高次変動は、DMRS から遠いデータシンボルの EVM にのみ現れ、
  noise_var(パイロット位置の残差)には現れない** → まさにアンカーの型。
- 判別材料は既にある: CSI の `symbol_evm[14]`(シンボル別 EVM)。
  「DMRS からの距離に比例して EVM が伸びる」なら位相追従不足が確定。
  [CONFIG 実験] `pusch_channel_estimator_td_strategy: interpolate`(こちらのプラミングは正常)
  と DMRS additional position 増(`dmrs_add_pos`)で改善するかを確認できる。

## 仮説10 TDD n-TA / FFT 窓 / ta4

**判定: 宿題クローズ — DU ソース修正は不要(OFH 構成では DU は n-TA を適用しない)。**

- `n_ta_offset` の参照は 3 箇所のみで、UL データパスには無い:
  1. SIB1/RRC で UE へ通知(`asn1_sys_info_packer.cpp:279-301`,
     `asn1_rrc_config_helpers.cpp:3437-3451`)。値はバンド固定導出 **FR1→25600**
     (`band_helper.cpp:1667-1679`)。n77 は FR1 → **RU の n-ta-offset=25600 と整合**。
  2. split-8 lower PHY の Tx/Rx タイムオフセット(`lower_phy_factory.cpp:41-48`)— OFH 不使用。
  3. PRACH/PUSCH TA 測定は相対値(`prach_detector_generic_impl.cpp:317-326`)。
- OFH の受信タイミングは ta4 窓の統計判定のみ(`ofh_rx_window_checker.cpp:64-95`)。
  窓は設定翻訳時にシンボル換算: `sym_start=floor(ta4_min/T_sym)=1`,
  `sym_end=ceil(ta4_max/T_sym)=10`(35.68µs シンボル)。
  **窓外パケットは counted+破棄 → 該当シンボルはゼロのまま復調 → BLER 直撃**。
  カウンタは JSON(`json_generators/ru/ofh.cpp`: `received_packets{early,on_time,late}`,
  `rx_window_stats{nof_missed_uplink_symbols,...}`)と定期ログに既出
  (`ru_metrics_consumers.cpp:164-191`)— **metrics.enable_ru_metrics を有効にするだけ**。
- FFT 窓そのもの(CP 内の切り出し位置)は RU 実装内(split 7.2 で DU からは不可視)。
  DU 側でできるのは t_align(群遅延)監視まで。t_align が CP(~2.3µs)に対して大きい
  UE がいれば TA ループ(`ta_management_system.cpp`)の設定
  (`update_measurement_ul_sinr_threshold` 既定 0dB が低すぎる注記 `:187-191`)を確認。

## その他の発見(軽微)

| 発見 | 場所 | 影響 |
|---|---|---|
| metrics 収集の `if (!enabled()) { }` 空文 | `ofh_message_receiver_metrics_collector.h:48-52` | 無効時も収集(無害・無駄) |
| `ul_snr_qam64_lowse_mcs_table` が qam64 と同一 | `mcs_calculator.cpp:81-90` | lowSe テーブル選択が無意味 |
| SRS 雑音 -30dB 固定仮定 | `ue_channel_state_manager.cpp:69-71` | 仮説4 |
| TA 採用 SINR 閾値既定 0dB(コメントは 10dB 推奨) | `ta_management_system.cpp:187-191` | 低 SINR 時の TA 荒れ |
| 非連続割当の推定 TODO | `port_channel_estimator_average_impl.cpp:167-168` | 現行構成では未到達 |

## 結論(切り分けの優先順)

1. **計装で三者 SINR(ch_est / post-eq / EVM)+シンボル別 EVM を同時観測**(フェーズ2)。
   - sinr_evm ≪ sinr_eq → データシンボル劣化(仮説9 の位相系 or E3 型干渉/RU 障害)。
     シンボル別 EVM の形で位相系(単調勾配)か干渉系(ランダム/バースト)かを分離。
   - sinr_evm ≈ sinr_eq ≈ 31dB なのに BLER 高 → デコーダ側(可能性低。E2 で反証済み)。
2. OFH 窓カウンタ(既存)で窓ズレ説を白黒(仮説10)。
3. その後 max_rank:2 に上げ、rank2 の上乗せ分を E4/E5 の物差し(post-eq SINR の低下量と
   cond number)で評価(仮説1・4・8)。
