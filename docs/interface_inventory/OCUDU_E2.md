# OCUDU_E2 — E2 インタフェース(CU / DU)

実装はすべて ocudu 本体リポジトリ。`lib/e2` を **CU-CP / CU-UP / DU が共有**し、ユニット毎のファクトリで**別個の E2 エージェント**(別 SCTP コネクション)を生成する構成。

## 比較表

| Axis | OCUDU finding | Evidence (repo:path:line) | Self / in-house AI-RAN |
|---|---|---|---|
| 1. Conformance spec + version | E2AP: ASN.1 ヘッダに `E2AP v03.00 (2023)`。E2SM 共通/KPM/RC: `E2SM v05.00 (2024)`(ASN.1)、実装コメントは KPM/RC とも `O-RAN.WG3.E2SM-KPM-R003-v03.00` / `O-RAN.WG3.E2SM-RC-R003-v03.00` 参照。E2SM-CCC: `E2SM CCC v06.00 (2025)` | ocudu:include/ocudu/asn1/e2ap/e2ap.h:7 / ocudu:include/ocudu/asn1/e2sm/e2sm_kpm_ies.h:7 / ocudu:include/ocudu/asn1/e2sm/e2sm_ccc.h:7 / ocudu:lib/e2/e2sm/e2sm_kpm/e2sm_kpm_asn1_packer.cpp:83 / ocudu:lib/e2/e2sm/e2sm_rc/e2sm_rc_control_service_impl.h:21-24 | |
| 2. Protocol / transport | SCTP(Near-RT RIC へ client 接続)。既定 127.0.0.1:36421。multi-homing 対応(addrs/bind_addrs 複数指定可)。リトライ: max_setup_retries=5, 再接続 1000ms | ocudu:apps/helpers/e2/e2_appconfig.h:15-32 / ocudu:apps/helpers/e2/e2_cli11_schema.cpp:18-31 / ocudu:include/ocudu/e2/e2ap_configuration.h:22-23 | |
| 3. Service-model coverage | **KPM**(RAN Func ID 2, OID 1.3.6.1.4.1.53148.1.2.2.2): Report Style 1〜5 全て広告。DU メトリクス: CQI, RSRP系, RRU.PrbAvailDl/Ul, RRU.PrbUsedDl/Ul, RRU.PrbTotDl/Ul, DRB.RlcSduDelayDl, DRB.UEThpDl/Ul, DRB.RlcPacketDropRateDl, DRB.RlcSduTransmittedVolumeDL/UL, DRB.AirIfDelayUl, DRB.RlcDelayUl, RACH.PreambleDedCell。CU-CP: RRC.ConnEstabAtt/Succ, RRC.ConnEstabFailCause.NetworkReject, UECNTX.ConnEstabAtt/Succ, RRC.ReEstabAtt…, RRC.ConnMean/Max。CU-UP: 上記共通+DRB.PdcpReordDelayUl, DRB.PacketSuccessRateUlgNBUu。**RC**(ID 3, OID …1.1.2.3): DU=Style 2 / Action 6「Slice-level PRB quota」のみ、CU-CP=Style 3 / Action 1「Handover Control」のみ、CU-UP=RC 広告するが control service **未登録**。**CCC**(OID …1.6.2.4): DU のみ、Style 2 O-RRMPolicyRatio executor のみ。Insert/Policy service は未実装 | ocudu:lib/e2/e2sm/e2sm_kpm/e2sm_kpm_asn1_packer.cpp:14,12(注: OID/ID), 90-165 / ocudu:lib/e2/e2sm/e2sm_kpm/e2sm_kpm_du_meas_provider_impl.cpp:15-88 / ocudu:lib/e2/e2sm/e2sm_kpm/e2sm_kpm_cu_meas_provider_impl.cpp:14-51,217-226 / ocudu:lib/e2/common/e2_du_factory.cpp:51-84 / ocudu:lib/e2/common/e2_cu_cp_factory.cpp:48-64 / ocudu:lib/e2/common/e2_cu_up_factory.cpp:47-55 / ocudu:lib/e2/e2sm/e2sm_rc/e2sm_rc_control_action_du_executor.cpp:73 / ocudu:lib/e2/e2sm/e2sm_rc/e2sm_rc_control_action_cu_executor.cpp:60-62 | |
| 4. Implementation maturity | E2 Setup / RIC Subscription(setup・delete)/ RIC Indication / RIC Control / E2 Removal / RIC 再接続 routine は実装済。E2 Connection Update は**常に failure 応答**(未実装と明記)。RC は多数 TODO(outcome 未充填 e2sm_rc_control_service_impl.cpp:42、型検証 TODO du_executor.cpp:94、CU HO 制御はパラメータ 7-21 未対応 cu_executor.cpp:69)。KPM も試験条件チェック等 TODO 多数(du_meas_provider_impl.cpp:195-257)。CCC は RC 由来 IF の流用に TODO(ccc_control_action_du_executor.cpp:32) | ocudu:lib/e2/procedures/e2ap_connection_update_procedure.cpp:24 / ocudu:lib/e2/common/e2_impl.cpp:60-145 / ocudu:lib/e2/e2sm/e2sm_rc/e2sm_rc_control_service_impl.cpp:42,87-94 / ocudu:lib/e2/e2sm/e2sm_rc/e2sm_rc_control_action_cu_executor.cpp:69 / ocudu:lib/e2/e2sm/e2sm_kpm/e2sm_kpm_du_meas_provider_impl.cpp:195-257 | |
| 5. Config-injection & extension points | **config ゲート(ビルドゲートなし)**: gnb/cu/du アプリの YAML `e2` セクション。`--enable_du_e2` / `--enable_cu_cp_e2` / `--enable_cu_up_e2`(既定 false)+ `--e2sm_kpm_enabled` / `--e2sm_rc_enabled` / `--e2sm_ccc_enabled`(既定 false)+ addrs/port/bind_addrs/SCTP チューニング。CMake の E2 有効化フラグは**存在しない**(`ENABLE_E2` grep 0 件)。gnb_id/PLMN/du_id は上位設定から注入 | ocudu:apps/helpers/e2/e2_appconfig.h:16-31 / ocudu:apps/units/flexible_o_du/o_du_high/e2/o_du_high_e2_config_cli11_schema.cpp:25 / ocudu:apps/units/o_cu_cp/e2/o_cu_cp_e2_config_cli11_schema.cpp:25 / ocudu:apps/units/o_cu_up/e2/o_cu_up_e2_config_cli11_schema.cpp:21 / ocudu:include/ocudu/e2/e2ap_configuration.h:17-27 | |
| 6. Known vendor deviations | RAN Function ID をハードコード(KPM=2, RC=3)。OID は O-RAN 標準 arc(53148)。RIC Connection Removal は「RIC が未対応のため」コメントアウト箇所あり(ric_connection_removal_routine.cpp:25)。KPM metric 定義に「mandatory Cause-label not supported in e2sm_kpm」等、仕様との差異注記が多数(e2sm_kpm_metric_defs.h) | ocudu:lib/e2/e2sm/e2sm_kpm/e2sm_kpm_asn1_packer.cpp:14 / ocudu:lib/e2/e2sm/e2sm_rc/e2sm_rc_asn1_packer.cpp:15 / ocudu:lib/e2/procedures/ric_connection_removal_routine.cpp:25 / ocudu:lib/e2/e2sm/e2sm_kpm/e2sm_kpm_metric_defs.h:282 ほか | |
| 7. Known interop constraints | E2 Node Component Config は F1/E1/NG のみ収集(xn/w1/s1/x2 は TODO)。Report Style 3 の条件なしアクション受理、複数 meas_cond_ueid 未対応などサブスクリプション制約(TODO 注記)。CU-CP/CU-UP/DU が**個別に** RIC へ接続するため、RIC 側は 3 ノード扱い(gNB 単一 E2 ノードではない) | ocudu:lib/e2/common/e2ap_asn1_helpers.h:172 / ocudu:lib/e2/e2sm/e2sm_kpm/e2sm_kpm_report_service_impl.cpp:239,339 / ocudu:lib/du/du_high/o_du_high_factory.cpp:156, ocudu:lib/cu_cp/o_cu_cp_factory.cpp:34, ocudu:lib/cu_up/o_cu_up_factory.cpp:32 | |

## 証跡ノート

### CU / DU の分担(確認済)
- 共有ライブラリ `lib/e2`(common / e2sm / procedures / gateways)を 3 ファクトリが利用:
  - DU: `create_e2_du_agent`(KPM du_meas_provider + RC Style2/Action6 + CCC Style2)— ocudu:lib/e2/common/e2_du_factory.cpp:23-88
  - CU-CP: `create_e2_cu_cp_agent`(KPM cu_cp_meas_provider + RC Style3/Action1)— ocudu:lib/e2/common/e2_cu_cp_factory.cpp:21-68
  - CU-UP: `create_e2_cu_up_agent`(KPM cu_up_meas_provider + RC は packer 登録のみ)— ocudu:lib/e2/common/e2_cu_up_factory.cpp:20-59
- つまり「1 つの e2ap ライブラリ+ユニット毎の独立エージェント」。CCC は DU 専用。

### サブスクリプション/インディケーション処理(確認済)
- 受信ディスパッチ: ric_sub_request / ric_sub_delete_request / ric_ctrl_request / e2conn_upd を init_msg として処理(ocudu:lib/e2/common/e2_impl.cpp:135-144)。
- 手続き実装: e2ap_subscription_setup_procedure.cpp / e2ap_subscription_delete_procedure.cpp / e2ap_indication_procedure.cpp / e2ap_ric_control_procedure.cpp / e2_setup_routine.cpp / ric_reconnection_routine.cpp(ocudu:lib/e2/procedures/ 配下)。
- KPM のアクション検証で未対応 RAN function ID / OID はエラー応答(ocudu:lib/e2/common/e2_subscription_manager_impl.cpp:65,149,183-194)。

### RIC エンドポイント設定(確認済)
- `e2` サブコマンド(YAML では `e2:` セクション)で addrs(旧名 addr 後方互換)/ port(20000-40000 range check)/ bind_addrs / sctp / e2sm_* を設定(ocudu:apps/helpers/e2/e2_cli11_schema.cpp:12-44)。
- 出力 YAML キー: `enable_du_e2` / `enable_cu_cp_e2` / `enable_cu_up_e2`(ocudu:apps/units/*/e2/*_yaml_writer.cpp:20)。

### E2SM-CCC 詳細(確認済)
- ASN.1 は JSON ベース E2SM-CCC v06.00(ocudu:include/ocudu/asn1/e2sm/e2sm_ccc.h:7)。DU で `e2sm_ccc_control_service_style_2` + `e2sm_ccc_control_o_rrm_policy_ratio_executor`(O-RRMPolicyRatio 制御)のみ登録(ocudu:lib/e2/common/e2_du_factory.cpp:69-84)。Report service は未対応(ocudu:lib/e2/e2sm/e2sm_ccc/e2sm_ccc_impl.cpp:32)。

### 未確認・推定事項
- E2AP ASN.1 ヘッダの生成元コメントは「3GPP TS ASN1 E2AP v03.00」表記だが、E2AP は O-RAN WG3 仕様(O-RAN.WG3.E2AP-v03.00 相当)。表記はコード生成テンプレート由来とみられる(推定)。
- RIC 実機(OSC RIC 等)との相互運用実績はコードからは判定不能(ランタイム確認要)。
