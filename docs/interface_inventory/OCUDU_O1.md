# OCUDU_O1 — O1 インタフェース(CM/FM/PM + VES)

## 比較表

| Axis | OCUDU finding | Evidence (repo:path:line) | Self / in-house AI-RAN |
|---|---|---|---|
| 1. Conformance spec + version | NRM モデルは 3GPP SA5 MnS YANG(ビルド時タグ `Tag_Rel19_SA112`、スクリプト既定は `Tag_Rel18_SA111`)。VES は `vesEventListenerVersion: 7.2.1` / commonEventHeader `version: 4.1`、FM は `3GPP-FaultSupervision`(TS 28.532 系 stndDefined)。PM の KPI 名は TS 28.554 準拠にリネーム。O-RAN O1 仕様(WG10)への明示的参照はコード中に**なし** | ocudu_netconf:Dockerfile:48 / ocudu_netconf:scripts/download_yang_models.sh:6,34 / ocudu_o1_adapter:templates/ves/alarm.json:19-21 / ocudu_o1_adapter:src/pm_metrics.py:5-28 | |
| 2. Protocol / transport | NETCONF over SSH(830)+ オプションで NETCONF over TLS(RFC 7589、6513、cert-to-name=common-name)。VES は HTTP(S) POST `/eventListener/v7`(Basic 認証、`verify=False`)。PM ストリームは WebSocket(gNB→adapter)+ HTTP POST JSON(adapter→streamTarget) | ocudu_netconf:entrypoint.sh:25,211-217 / ocudu_netconf:README.md:29-44 / ocudu_o1_adapter:src/ves.py:30-31,50 / ocudu_o1_adapter:src/pm_metrics.py:211-227 | |
| 3. Model coverage | サーバに実ロードされる YANG: 3GPP `_3gpp-common-*`, `_3gpp-nr-nrm-gnbdufunction/nrcelldu/gnbcucpfunction/nrcellcu/gnbcuupfunction/ep/rrmpolicy` 等 27 モジュール(DU プロファイル)+ OCUDU 独自拡張 25 本(`nrcelldu-*-extensions`, `gnbdufunction-extensions`, `log/metrics/pcap/hal/remote-control-extensions` 等、namespace `urn:ocudu-*:1.0`)。DU プロファイルは RU 用 o-ran-* モジュール群も併載 | ocudu_netconf:scripts/setup_du.sh:20-76 / ocudu_netconf:scripts/setup_cucp.sh:15-53 / ocudu_netconf:scripts/setup_ru.sh:15-58 / ocudu_netconf:custom_yangs/nrcelldu-extensions.yang:5-7 | |
| 4. Implementation maturity | CM: 実装済(通知駆動、runtime 更新は `ssb_block_power_dbm`/`RRMPolicyRatio`/`PerfMetricJob` のみ、それ以外は gNB 再起動)。FM: 実装済だが自己定義アラーム 6 種のみ(3GPP alarm IRP の網羅ではない)。PM: 独自 JSON ストリーム実装(3GPP file-based PM・VES measurement ドメインは**未実装**)。AMF 接続状態監視は TODO | ocudu_o1_adapter:src/config_manager.py:41,103-111 / ocudu_o1_adapter:src/alarm_defs.py:13-53 / ocudu_o1_adapter:src/pm_metrics.py:173(`# TODO: check for AMF connection status`) | |
| 5. Config-injection & extension points | 標準 NRM 属性で足りない gNB 設定は**すべて独自 YANG 拡張**(`ocudu_nrcelldu_ofh/ssb/prach/tdd/pdsch/pusch/pucch/csi/srs/pdcch/paging/drx/sib/mac_cell_group` + `EP_F1U` ポート拡張 + 機能単位 log/hal/pcap/metrics/testmode/remote_control 拡張)に載せ、adapter が Jinja2 テンプレート(profile 毎 gnb/cu/cucp/cuup/du.yaml)で gNB YAML に射影。初期 config XML は netconf コンテナ内蔵 or `--custom-config`(k8s では ConfigMap) | ocudu_o1_adapter:src/config_manager.py:749-950 / ocudu_o1_adapter:templates/gnb.yaml:1-8 / ocudu_netconf:entrypoint.sh:31-68,201-209 / ocudu_helm:charts/ocudu-gnb/values-o1.yaml:545-600 | |
| 6. Known vendor deviations | (a) VES ヘッダに固定値多数: `_REPORTING_ENTITY="ocududu"`, `sourceId="noIdea"`, alarm eventId が固定 DN 文字列、pnfRegistration はダミー機体情報(serialNumber "VENDORB-7DEV-…", sourceName "PRTNILACQ01M017DEV01", NETCONF 資格情報 "netconf/netconf" を additionalFields に平文埋込)。(b) TS 28.554 の綴りミス `VirtualResUtilizaiton` を**修正形で emit**。(c) `DLLat_gNB-DU` は仕様(空キュー着 SDU 限定)と異なり全 SDU 平均。(d) PM は VES ではなく独自 envelope(`nfType/metrics/jobId`) | ocudu_o1_adapter:src/ves.py:32-36,108-117 / ocudu_o1_adapter:templates/ves/pnf_registration.json:22-45 / ocudu_o1_adapter:src/pm_metrics.py:21-28,73-81,195-209 | |
| 7. Known interop constraints | VES collector は ONAP DCAE 前提(README 参照リンク)、TLS 検証無効(`_VERIFY=False`)。NETCONF SSH は hostkey_verify=False。PerfMetricJob の `streamTarget`/`administrativeState=UNLOCKED` が必須(3GPP 標準 NRM に streamTarget は無く運用側の合意前提)。NRCellDU の `administrativeState` LOCKED 既定でセル無効化。健全性は Flask REST(/config-healthy)で k8s liveness に連動 | ocudu_o1_adapter:src/ves.py:64-65 / ocudu_o1_adapter:src/o1_adapter.py:117 / ocudu_o1_adapter:src/config_manager.py:377-415,944 / ocudu_helm:charts/ocudu-gnb/templates/deployment.yaml:147-157 | |

## 証跡ノート

### NETCONF サーバスタック(確認済)
- netopeer2 v2.8.2 / libnetconf2 v4.2.14 / libyang v5.4.9 / sysrepo v4.5.4 をソースビルドしてコンテナ化(ocudu_netconf:Dockerfile:49-52)。起動は `netopeer2-server -v2 -d`(ocudu_netconf:entrypoint.sh:217)。
- 起動時に profile(gnb/cu/cucp/cuup/du/ru)毎の setup スクリプトで sysrepo にモジュール登録、`sysrepocfg --edit` で初期 config を running に投入(ocudu_netconf:entrypoint.sh:97-100,190-209)。停止時に running をエクスポートして永続化(ocudu_netconf:entrypoint.sh:193-199)。
- 有効化 feature: `MeasurementsUnderManagedFunction`, `FmUnderManagedElement`, `EPClassesUnderGNBDUFunction`(ocudu_netconf:scripts/setup_du.sh:48-50)。

### CM フロー(確認済)
1. adapter が `create_subscription(stream_name="NETCONF")` で netconf-config-change を購読(ocudu_o1_adapter:src/config_manager.py:962-966)。
2. 変更検知 → DeepDiff で差分分類(ocudu_o1_adapter:src/config_manager.py:66-111)。
3. runtime 更新可能(`ssb_block_power_dbm` / `RRMPolicyRatio` / `PerfMetricJob`)なら WebSocket コマンド `ssb_set` / `rrm_policy_ratio_set` を gNB に送信(ocudu_o1_adapter:src/config_manager.py:129-181, ocudu:apps/units/flexible_o_du/o_du_high/du_high/commands/du_high_remote_commands.h:21,39)。
4. それ以外は `quit` コマンド → 設定ファイル再生成 → コンテナ側 entrypoint が gNB を再起動(ocudu_o1_adapter:src/config_manager.py:113-127, ocudu_helm:charts/ocudu-gnb/resources/entrypoint.sh:568-621)。

### FM(確認済)
- アラーム定義は自己定義 6 種のみ: NETCONF_CONNECTION_LOSS(1001), REMOTE_CONTROL_CONNECTION_LOSS(1002), RU_NETCONF_CONNECTION_LOSS(1003), AMF_CONNECTION_LOSS(2001, ただし発火箇所は TODO), PTP_GM_LOSS(3001), PTP_LATENCY_TOO_HIGH(3002)(ocudu_o1_adapter:src/alarm_defs.py:13-53)。
- 通知は VES stndDefined `notifyNewAlarm`(stndDefinedNamespace `3GPP-FaultSupervision`)として送信(ocudu_o1_adapter:templates/ves/alarm.json:20-27)。ietf-alarms/o-ran-fm の NETCONF notification としての FM 送出は adapter 側には見当たらない。

### PM(確認済)
- gNB 本体の WebSocket(remote_control, 既定 8001)から JSON メトリクスを受信し flatten(ocudu:apps/services/remote_control/remote_control_appconfig.h:13-18, ocudu_o1_adapter:src/pm_metrics.py:130-148)。
- NRM の `PerfMetricJob`(administrativeState / performanceMetrics / granularityPeriod / streamTarget)を NETCONF ツリーから抽出し、job 毎にフィルタして streamTarget へ HTTP POST(ocudu_o1_adapter:src/config_manager.py:383-415, ocudu_o1_adapter:src/pm_metrics.py:175-227)。

### VES(確認済)
- 送出ドメインは **pnfRegistration / stndDefined(fault) / stateChange の 3 種のみ**。テンプレートディレクトリ ocudu_o1_adapter:templates/ves/ の実ファイルは alarm.json, pnf_registration.json, state_change.json。`measurement` ドメインは src/ves.py・templates/ves/ のいずれにも**存在しない**(探索箇所: ocudu_o1_adapter:src/ves.py 全文, ocudu_o1_adapter:templates/ves/ 一覧)。
- collector 設定は CLI(`--ves_host/--ves_port/--ves_username/--ves_password/--ves_scheme`、既定 localhost:8443 https sample1/sample1)(ocudu_o1_adapter:src/o1_adapter.py:455-485)。Helm からは `o1.ves.*` で注入(ocudu_helm:charts/ocudu-gnb/templates/deployment.yaml:268-277, ocudu_helm:charts/ocudu-gnb/values-o1.yaml:312-317)。
- PNF 登録は `--registration` フラグ時に起動時 1 回送信(ocudu_o1_adapter:src/o1_adapter.py:492-498,578-579)。

### 未確認・推定事項
- 実運用イメージの 3GPP YANG タグが Rel19_SA112 か Rel18_SA111 かは Docker ビルド時 ARG 継承に依存(推定: Rel19_SA112)。ランタイム確認要。
- specifications.o-ran.org `download?id=1035` が指す O-RAN YANG バージョンはコードから特定不能(ランタイム確認要)。
