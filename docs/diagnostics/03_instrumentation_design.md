# フェーズ2: PUSCH 診断計装 — 設計と実装

## 設計方針

1. **単一フラグ**: `expert_phy: pusch_diagnostics_enabled: true`(du_low expert 設定)。
   既定 false。false のときの追加コストは分岐数個(ホットパス実測影響ゼロ)。
2. **既存出力の最大活用**: 追加するのは「既存に無い量」のみ。
   - 既にある(設定だけで出る): 3種 SINR・シンボル別 EVM・ポート別 RSRP・EPRE・TA・CFO
     (PHY debug ログ)、OFH 窓カウンタ(enable_ru_metrics)、`ul_olla`(scheduler log metrics)、
     受信 IQ ダンプ(`phy_rx_symbols_filename` → コンステレーション/共分散はオフライン計算)。
   - 無い(今回追加): **レイヤ別 post-eq SINR**、**2x2 Gram condition number**、
     **ポート別雑音分散**、**LLR 飽和率・平均振幅**。
3. **1 PUSCH = 1 行**: 追加量は `channel_state_information` に optional で載せ、既存の
   PUSCH info ログ行(`logging_pusch_processor_decorator`)にそのまま出る。
   debug レベル(hex ダンプ付き)に上げる必要がない。
4. rank1/rank2 は同一指標(per-layer SINR は rank1 なら 1 要素)で比較可能。

## フラグが有効化するもの

| 量 | 計算場所 | コスト(フラグ on 時) |
|---|---|---|
| post-eq SINR(選択法に関係なく) | 既存 `pusch_demodulator_impl` | 既存ロジック流用 |
| EVM / sinr_evm(選択法に関係なく) | 既存 `evm_calculator` | 復調距離のみ(軽量) |
| レイヤ別 post-eq SINR | `pusch_demodulator_impl::demodulate` | 雑音分散配列の 1 パス/シンボル |
| ポート別雑音分散 [dB] | 同上(チャネル推定器の値を転記) | ゼロ |
| condition number(2レイヤ時) | 同上(最初のデータシンボルの H から最大 64 RE サンプル) | 64 RE × 2x2 Gram(無視可能) |
| LLR 飽和率(\|LLR\|≥120)・平均振幅 | 同上(デスクランブル後の codeword を集計) | 1 パス/コードワード |

## 出力フォーマット(PHY info ログ、`log.phy_level: info` で出力)

```
PUSCH: rnti=0x4601 h_id=0 prb=[0..273) symb=[0..14) mod=64QAM rv=0 tbs=... crc=OK iter=2.0
  sinr=31.2dB diag[sinr_ce=31.4 sinr_eq=31.2 sinr_evm=19.3 evm=0.109 lyr=[31.5] cond=na
  nvar_p=[-42.1 -41.8 -40.2 -41.5] llr_sat=0.42 llr_avg=54.3] epre=-12.3dB
  rsrp=[-12.0 -12.4 -13.1 -12.2]dB t_align=0.12us cfo=+120.3Hz
```
(実際は 1 行。`diag[...]` ブロックと epre/rsrp/t_align/cfo はフラグ on のとき短形式にも出す)

## 判読ガイド(仮説との対応)

- `sinr_eq ≈ sinr_ce ≫ sinr_evm` → データシンボルにのみ乗る劣化
  (位相ドリフト=仮説9 / バースト・狭帯域干渉=仮説1系 / 窓ズレ ISI=仮説10)。
  → PHY debug に上げれば `evm=[シンボル別]` が出る: 単調勾配なら位相系、ランダムなら干渉系。
- `sinr_eq ≈ sinr_evm` なのに BLER 高 → 復号側(仮説6)だが E2 実験で反証済み → 想定外事象。
- `lyr=[a,b]` の差が大 / `cond` 大 → 空間相関 or RI 楽観(仮説4・E5)。
- `nvar_p` の 1 ポート突出 → ブランチ異常(仮説8)。`rsrp=[...]` と併読。
- `llr_sat` が中 SINR で高い → デマップ・スケーリング異常(仮説6)。
- OFH `received_packets{early,late}` / `nof_missed_uplink_symbols` > 0 → 窓/輸送(仮説10)。

## 変更ファイル一覧(全て追加的・フラグ off で挙動不変)

| ファイル | 変更 |
|---|---|
| `include/ocudu/phy/upper/pusch_diagnostics.h` | **新規**: `pusch_diagnostics` 構造体 |
| `include/ocudu/phy/upper/channel_state_information.h` | optional メンバ+set/get |
| `include/ocudu/phy/upper/channel_state_information_formatters.h` | diag ブロック出力 |
| `include/ocudu/phy/upper/channel_processors/pusch/pusch_demodulator_notifier.h` | stats に optional diag |
| `include/ocudu/phy/upper/channel_processors/pusch/factories.h` | factory に `enable_diagnostics=false` 引数 |
| `lib/phy/upper/channel_processors/pusch/demodulator_factories.cpp` | 引数伝搬 |
| `lib/phy/upper/channel_processors/pusch/pusch_demodulator_impl.{h,cpp}` | 計測本体(フラグゲート) |
| `lib/phy/upper/channel_processors/pusch/pusch_processor_notifier_adaptor.h` | stats→CSI 転記 |
| `include/ocudu/phy/upper/upper_phy_factories.h` | 設定フィールド |
| `lib/phy/upper/upper_phy_factories.cpp` | EVM/post-eq 強制有効+demod へ伝搬 |
| `apps/units/flexible_o_du/o_du_low/du_low_config.h` | `pusch_diagnostics_enabled` |
| `apps/units/flexible_o_du/o_du_low/du_low_config_cli11_schema.cpp` | CLI/YAML キー |
| `apps/units/flexible_o_du/o_du_low/du_low_config_translator.cpp` | 伝搬 |
| `apps/units/flexible_o_du/o_du_low/du_low_config_yaml_writer.cpp` | 設定ダンプ |

## 測定時の推奨設定(diag 一式)

```yaml
expert_phy:
  pusch_diagnostics_enabled: true     # 本計装(PUSCH 毎 1 行)
log:
  phy_level: info                     # diag は info で出る(debug 不要)
metrics:
  enable_json: true                   # UE/セル metrics(JSON, :8001)
  enable_ru_metrics: true             # OFH 窓カウンタ
  enable_log: true                    # ul_olla/dl_olla を含む log 版 metrics
```
さらに深掘りが必要なときのみ:
- `log.phy_level: debug` … シンボル別 EVM 配列・PDU 全フィールド(重い)
- `log.phy_rx_symbols_filename: /tmp/rx.bin` … 受信 IQ ダンプ
  (コンステレーション・共分散・PRB 別 SINR のオフライン解析用)
