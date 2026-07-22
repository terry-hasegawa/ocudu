# REPO_ROLE_MAP — OCUDU リポジトリ役割マップ

調査日: 2026-07-22(静的解析のみ。ビルド・実行なし)

対象リポジトリと調査時点のHEAD:

| リポジトリ | HEAD commit | 役割(結論) |
|---|---|---|
| ocudu (https://gitlab.com/ocudu/ocudu) | 91552ed | gNB/CU/DU 本体 (srsRAN 系 C++)。**E2 エージェント**(CU-CP/CU-UP/DU)と、O1 連携用の **WebSocket リモート制御サーバ**を内蔵。NETCONF/VES のコードは持たない |
| ocudu_netconf | a764471 | **O1 の NETCONF サーバ**(SMO 向け)。netopeer2/sysrepo をコンテナ化した独立サーバであり、ライブラリではない |
| ocudu_o1_adapter | 130aadd | **O1 アダプタ**(Python)。NETCONF **クライアント**として ocudu_netconf サーバの datastore を購読し、gNB 設定 YAML を生成・投入。**VES 送信元**。**M-Plane クライアント**(O-RU 向け ru_controller / RU フォワーダ)も同居 |
| ocudu_helm | 0f70d17 | **Helm チャート集**(gnb / cu / cu-cp / cu-up / du + インフラ)。O1 サイドカー構成・SR-IOV・hugepages などプラットフォーム注入点を定義 |

## 1. ocudu(本体)

- E2: 共有ライブラリ `lib/e2` に E2AP/E2SM 実装。CU-CP・CU-UP・DU それぞれ専用ファクトリでエージェントを生成。
  - 証跡: ocudu:lib/e2/common/e2_du_factory.cpp:23, ocudu:lib/e2/common/e2_cu_cp_factory.cpp:21, ocudu:lib/e2/common/e2_cu_up_factory.cpp:20
  - 生成箇所: ocudu:lib/du/du_high/o_du_high_factory.cpp:156, ocudu:lib/cu_cp/o_cu_cp_factory.cpp:34, ocudu:lib/cu_up/o_cu_up_factory.cpp:32
  - E2AP バージョン: ocudu:include/ocudu/asn1/e2ap/e2ap.h:7(`E2AP v03.00 (2023)`)
- O1 関連: NETCONF/sysrepo/VES のコードは**存在しない**。`grep -ril 'netconf|sysrepo|libyang' lib include apps` は 0 件、VES 系キーワードも実質 0 件(lib/asn1/rrc_nr/serving_cell.cpp の偶然一致のみ)。
  代わりに O1 アダプタが接続する **WebSocket リモート制御サーバ**(JSON コマンド `quit` / `metrics_subscribe` / `ssb_set` / `rrm_policy_ratio_set`)を提供。
  - 証跡: ocudu:apps/services/remote_control/remote_server.cpp:57,100, ocudu:apps/units/flexible_o_du/o_du_high/du_high/commands/du_high_remote_commands.h:21,39, ocudu:apps/services/remote_control/remote_control_appconfig.h:13-18(default port 8001)

## 2. ocudu_netconf(O1 NETCONF サーバ / SMO-facing、サーバ側)

- 独立コンテナで netopeer2-server を起動する構成。**共有ライブラリではない**(o1_adapter は本リポジトリのコードを import せず、ネットワーク越しに NETCONF で接続する)。
  - 証跡: ocudu_netconf:entrypoint.sh:217(`netopeer2-server -v2 -d`)、ocudu_netconf:README.md:9-25(`--config gnb|cu|cucp|cuup|du|ru` でプロファイル選択)
- スタック: libyang v5.4.9 / sysrepo v4.5.4 / libnetconf2 v4.2.14 / netopeer2 v2.8.2(CESNET 系)。
  - 証跡: ocudu_netconf:Dockerfile:49-52
- YANG 取得元: 3GPP SA5 MnS(forge.3gpp.org、ビルド時タグ `Tag_Rel19_SA112`)、O-RAN specs(specifications.o-ran.org download?id=1035)、O-RAN SC oam/modeling、IEEE 802.1X、ietf-system。
  - 証跡: ocudu_netconf:Dockerfile:47-48, ocudu_netconf:scripts/download_yang_models.sh:15,20,25-31,34
  - 注: スクリプト単体のデフォルトは `Tag_Rel18_SA111`(ocudu_netconf:scripts/download_yang_models.sh:6)だが、イメージビルドでは Dockerfile の ARG `Tag_Rel19_SA112` が優先される(推定・ビルド時 env 継承による。実イメージでの要確認事項)。
- サーバは SSH(830)に加え、`--enable-tls` で NETCONF over TLS(RFC 7589、6513)対応。cert-to-name(common-name)マッピング。
  - 証跡: ocudu_netconf:entrypoint.sh:25-26,211-213, ocudu_netconf:README.md:29-44, ocudu_netconf:scripts/setup_tls.sh

## 3. ocudu_o1_adapter(O1 アダプタ + M-Plane クライアント)

- 役割宣言: 「CU/DU と SMO の間のアダプタ。CM/FM をサポート、PM は WebSocket 経由の JSON メトリクスサービス」。
  - 証跡: ocudu_o1_adapter:README.md:3-14
- NETCONF **クライアント**(ncclient)として ocudu_netconf サーバに接続(SSH または TLS)。datastore 変更通知を購読し、Jinja2 テンプレートから gNB 設定 YAML を再生成 → WebSocket でランタイム更新 or 再起動要求。
  - 証跡: ocudu_o1_adapter:src/o1_adapter.py:99-121, ocudu_o1_adapter:src/config_manager.py:962-986, ocudu_o1_adapter:src/config_manager.py:41(`_RUNTIME_UPDATABLE_PARAMS`)
- **VES はここから送信**(HTTP POST → `/eventListener/v7`)。ドメイン: pnfRegistration / stndDefined(FaultSupervision)/ stateChange。
  - 証跡: ocudu_o1_adapter:src/ves.py:50,59-131,133-161, ocudu_o1_adapter:templates/ves/(alarm.json, pnf_registration.json, state_change.json の3種のみ)
- **M-Plane クライアント**: `ru_controller.py`(スタンドアロン)が O-RU へ NETCONF 接続(direct / call-home RFC 8071)し o-ran-uplane-conf 等を設定。`--ru_forward` で DU 側 NETCONF 設定を O-RU へ選択転送。
  - 証跡: ocudu_o1_adapter:src/ru_controller.py:6-11,316-320,389-417, ocudu_o1_adapter:src/ru_forwarder.py:23-31,100-158

## 4. ocudu_helm(プラットフォーム)

- チャート: ocudu-gnb / ocudu-cu / ocudu-cu-cp / ocudu-cu-up / ocudu-du + linuxptp / grafana / influxdb3 / onap-smo-lite / rt-tests / ru_emulator / tuned / open5gs。
  - 証跡: ocudu_helm:README.md:11-27, ocudu_helm:charts/ 配下ディレクトリ一覧
- O1 有効時は 1 Pod 内に gnb + ocudu-o1-adapter + netconf-server の 3 コンテナ(サイドカー)構成。
  - 証跡: ocudu_helm:charts/ocudu-gnb/templates/deployment.yaml:253-366
- SMO 側(SDN-R + VES collector + Kafka)は onap-smo-lite チャートで提供。
  - 証跡: ocudu_helm:charts/onap-smo-lite/templates/ves-collector-deployment.yaml, ocudu_helm:charts/onap-smo-lite/values.yaml:29-60

## 事前仮定との突合

- 「O1 = SMO 向け NETCONF サーバ」→ **一致**。サーバ実体は ocudu_netconf(netopeer2)。ただし NRM の意味処理(設定反映・FM/PM/VES)は o1_adapter が担う分離構造。
- 「M-Plane = O-RU 向け NETCONF クライアント(call-home)」→ **一致**。実体は ocudu_o1_adapter 内 ru_controller / ru_forwarder。call-home は `--callhome` オプション(デフォルトは direct connect)。
- 「ocudu_netconf はライブラリか独立サーバか」→ **独立サーバ**(コンテナ)。o1_adapter とはプロセス・リポジトリともに分離し、NETCONF プロトコルでのみ結合。
