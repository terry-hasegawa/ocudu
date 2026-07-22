# OCUDU_HELM — Helm(プラットフォーム/デプロイ)

対象: ocudu_helm リポジトリ(charts/ 13 チャート)。RAN 本体チャートは ocudu-gnb(CU/DU 一体)、ocudu-cu、ocudu-cu-cp、ocudu-cu-up、ocudu-du。

## 比較表

| Axis | OCUDU finding | Evidence (repo:path:line) | Self / in-house AI-RAN |
|---|---|---|---|
| 1. Conformance spec + version | 標準準拠対象ではなく独自 values スキーマ。チャート版数: ocudu-gnb 3.7.x / ocudu-cu 1.2.x / ocudu-du 1.2.x ほか。K8s >= 1.24, Helm 3.x 前提 | ocudu_helm:README.md:11-27,31-35 | |
| 2. Protocol / transport(K8s 露出) | Service 定義: N2(SCTP 38412)/ N3(UDP 2152)LoadBalancer、metrics(8001)、O1(NodePort 30830 既定、TLS 時 6513)、DU は F1-C/F1-U/O1 個別 Service。hostNetwork true/false 両対応(false が既定・NetworkPolicy 可) | ocudu_helm:charts/ocudu-gnb/values.yaml:285-349,150-155 / ocudu_helm:charts/ocudu-gnb/values-o1.yaml:325-338 / ocudu_helm:charts/ocudu-du/templates/service-f1c.yaml, service-f1u.yaml, service-o1.yaml | |
| 3. Model coverage(values スキーマ) | values.yaml 直下に replicaCount / image / configmap / serviceAccount / rbac / podDisruptionBudget / securityContext / probes / network / resources / nodeSelector / metricsService / service / persistence / o1 / sriovConfig / networkPolicy / config。gNB 設定本体は `config."gnb-config.yml"` に **YAML 全文をインライン埋込**(ConfigMap 化)。O1 時は `o1Config."o1-config.xml"` に NRM XML 全文をインライン埋込 | ocudu_helm:charts/ocudu-gnb/values.yaml:565-651 / ocudu_helm:charts/ocudu-gnb/values-o1.yaml:302-371,372-681 | |
| 4. Implementation maturity | gnb/cu/cu-cp/cu-up/du の分割チャート一式・O1 サイドカー・SR-IOV/DPDK・hugepages・PTP(linuxptp)・監視(grafana/influxdb3)・SMO(onap-smo-lite)まで揃う。Multus の NetworkAttachmentDefinition はチャートでは**生成しない**(docs の手動手順のみ、Pod への annotation は podAnnotations 経由でユーザ付与) | ocudu_helm:charts/(一覧) / ocudu_helm:charts/ocudu-gnb/docs/sriov-setup.md:14,67,101 / ocudu_helm:charts/ocudu-gnb/values.yaml:92 | |
| 5. Config-injection & extension points | (a) 非 O1: ConfigMap の gnb-config.yml を entrypoint.sh が加工して起動 — POD_IP を cu_up.ngu/cu_cp.amf の bind_addr に前置注入、USE_EXT_CORE 時 LB_IP を ext_addr 注入、`hal.eal_args` の `@(...)` を cgroup 実 CPU に書換(CPU ピニング)、SR-IOV 割当 VF の PCI BDF を `network_interface` に、PF 経由取得 MAC を `du_mac_addr` に書換。(b) O1: config は netconf-server(o1-config.xml)→ o1-adapter が gnb-config.yml を emptyDir に生成 → gnb コンテナは生成待ちループ後に起動。liveness は adapter の /config-healthy に切替(config 不整合で Pod 再起動)。hugepages は resources に hugepages-1Gi/2Mi を書くだけで volume 自動生成 | ocudu_helm:charts/ocudu-gnb/resources/entrypoint.sh:196-253,306-341,400-458,568-621 / ocudu_helm:charts/ocudu-gnb/templates/deployment.yaml:42-105,147-157,229-252 / ocudu_helm:charts/ocudu-gnb/values.yaml:157-206 | |
| 6. Known vendor deviations / workarounds | (a) VF の MAC 取得は sysfs → PF の ip-link → dmesg の 3 段フォールバック(vfio-pci バインド時対策)。(b) O1 postStart フックが /restarted を叩いて健全状態へリセット。(c) NETCONF TLS 証明書は Secret 未指定時に自己署名を emptyDir 共有(dev/test 用と明記)。(d) DU チャートは SR-IOV netdevice 非対応で DPDK(vfio-pci)前提と明記 | ocudu_helm:charts/ocudu-gnb/resources/entrypoint.sh:347-398 / ocudu_helm:charts/ocudu-gnb/templates/deployment.yaml:229-252,58-73 / ocudu_helm:charts/ocudu-du/values.yaml:327-337 | |
| 7. Known interop constraints | replicaCount=1 固定(stateful)、strategy Recreate。SR-IOV 前提条件: SR-IOV device plugin + containerd `device_ownership_from_security_context=true` + イメージ側 setcap(cap_sys_nice,cap_ipc_lock,cap_perfmon)。O1 は SMO 側に ONAP 系(SDN-R + VES collector)を想定(onap-smo-lite、VES ホスト既定値が onap-smo-ves-collector.onap.svc...)。イメージは GitLab レジストリ固定参照 | ocudu_helm:charts/ocudu-gnb/values.yaml:4-7,98-129 / ocudu_helm:charts/ocudu-gnb/values-o1.yaml:307,312-317 / ocudu_helm:charts/onap-smo-lite/values.yaml:29-60 | |

## 証跡ノート

### O1 サイドカー・トポロジ(確認済)
- `o1.enable_ocudu_o1=true` で 1 Pod = 3 コンテナ:
  1. gnb 本体(生成された /etc/config/gnb-config.yml を待って起動、ocudu_helm:charts/ocudu-gnb/templates/deployment.yaml:206, resources/entrypoint.sh:568-586)
  2. ocudu-o1-adapter(`--netconf_host` / `--ves_*` / `--ws_*` / `--template gnb.yaml` を values から注入、deployment.yaml:254-298)
  3. netconf-server(`entrypoint.sh --config gnb --custom-config /etc/netconf/o1-config.xml`、deployment.yaml:307-347)
- 共有 volume: o1-gnb-config(emptyDir、生成 YAML 受け渡し)、netconf-running-config-volume(running 永続化)、o1-config-volume(初期 NRM XML ConfigMap)、TLS 時 netconf-tls-certs(deployment.yaml:47-73)。

### CU-DU 分割トポロジ(確認済)
- 分割デプロイは ocudu-cu(N2/N3/F1)、ocudu-cu-cp、ocudu-cu-up、ocudu-du(F1 + SR-IOV/DPDK)を個別リリースとして構成。DU の F1 接続先は values の `config` 内 `f1ap.addrs/bind_addrs`・`f1u.socket[].bind_addr` に直書き(ocudu_helm:charts/ocudu-du/values.yaml:357-363)。
- DU チャートにも values-o1.yaml があり、DU 単体 O1(profile du、EP_F1C/EP_F1U を NRM 経由で注入)に対応(ocudu_helm:charts/ocudu-du/values-o1.yaml、o1_adapter 側対応: ocudu_o1_adapter:src/config_manager.py:242-267)。

### ネットワークアタッチメント・モデル(確認済)
- 既定は CNI(hostNetwork=false)+ SR-IOV device plugin の extended resource(`intel.com/intel_sriov_netdevice` 等)を limits/requests に 1 個自動計上(ocudu_helm:charts/ocudu-gnb/templates/deployment.yaml:122-145, values.yaml:427-443)。
- Multus NAD の作成・annotation 付与はチャート外(docs/sriov-setup.md の手順とサンプル annotation のみ。ocudu_helm:charts/ocudu-gnb/docs/sriov-setup.md:38-101)。
- VF 実デバイスは環境変数 `PCIDEVICE_<RESOURCE>` から BDF を取得して config に注入(resources/entrypoint.sh:108-123,554-565)。

### リソース/hugepages/CPU ピニング(確認済)
- hugepages-1Gi / hugepages-2Mi を resources に書くと volume(medium: HugePages-*)と mount を自動生成(templates/deployment.yaml:96-105,221-228)。
- CPU ピニングは Guaranteed QoS(requests=limits)前提で、eal_args の CPU リストを cgroup cpuset から実測して書換(resources/entrypoint.sh:259-341)。

### パラメータ化 vs ハードコード(確認済)
- パラメータ化: イメージ、リソース、SR-IOV リソース名、O1 有効化、VES 接続先、NETCONF TLS、Service 型/ポート、NetworkPolicy、persistence、probe。
- ハードコード気味: gnb-config.yml / o1-config.xml は values 内の**生テキスト**(スキーマ検証なし、キー単位のオーバーレイ不可)。O1 ポート系(healthcheck 5000、TLS 6513)や entrypoint の待機タイムアウト(CONFIG_CREATE_TIMEOUT 既定 30s)はテンプレート/スクリプト内固定(resources/entrypoint.sh:634)。

### 未確認・推定事項
- values.yaml(非 O1 系)には `o1.healthcheckPort` 等のキーが無く、`o1.enable_ocudu_o1=true` を values.yaml 単体で立てるとテンプレート参照が不足する可能性(values-o1.yaml の併用が前提とみられる。推定・helm template での確認要)。
