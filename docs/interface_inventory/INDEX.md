# OCUDU 外部インタフェース・インベントリ — INDEX

srsRAN 系 O-RAN CU/DU スタック「OCUDU」の対外・O-RAN 標準系インタフェースを静的解析で棚卸しした結果。
各表の「Self / in-house AI-RAN」列は比較用に**空欄**のまま(後日記入)。

調査対象リポジトリ(いずれも default branch、HEAD は REPO_ROLE_MAP 参照):
ocudu / ocudu_netconf / ocudu_o1_adapter / ocudu_helm

## ドキュメント一覧

| ファイル | 内容 |
|---|---|
| [REPO_ROLE_MAP.md](REPO_ROLE_MAP.md) | 4 リポジトリの役割マップ(O1 サーバの所在、VES 送出元、ライブラリ/サーバの区別) |
| [OCUDU_O1.md](OCUDU_O1.md) | O1: NETCONF サーバスタック、YANG モジュール実装セット、CM/FM/PM 実装状況、VES ドメイン |
| [OCUDU_MPLANE.md](OCUDU_MPLANE.md) | M-Plane: call-home、supervision、使用 o-ran YANG、config-push 構造、可変/固定の切り分け |
| [OCUDU_E2.md](OCUDU_E2.md) | E2: E2AP/E2SM 版数、KPM/RC/CCC カバレッジ(CU/DU 別)、購読/制御処理、設定ゲート |
| [OCUDU_HELM.md](OCUDU_HELM.md) | helm: values 構造、config 注入経路、SR-IOV/hugepages/CPU ピニング、CU-DU 分割トポロジ |

## 全体像(コード確認済みの要点)

- O1 終端は本体外: NETCONF サーバ = ocudu_netconf(netopeer2)、NRM の意味処理・VES 送信 = ocudu_o1_adapter(Python サイドカー)。本体との結合は WebSocket JSON(remote_control、quit/ssb_set/rrm_policy_ratio_set/metrics_subscribe)。
- gNB 設定のうち 3GPP NRM で表現できない部分は独自 YANG 拡張(urn:ocudu-*:1.0)に集約 → ベンダ拡張差分の比較はこの拡張群が最重要ポイント。
- M-Plane は ncclient ベースの軽量クライアント(direct / RFC 8071 call-home)。U-Plane 設定・キャリア活性化・supervision keep-alive まで。software/file-management 等の運用系 RPC は未実装。
- E2 は CU-CP / CU-UP / DU が個別エージェントとして RIC に接続。KPM(Style 1-5)、RC(DU: Slice-level PRB quota、CU: Handover Control 各 1 アクション)、CCC(DU: O-RRMPolicyRatio)のみ。すべて config ゲート(既定無効)。
- helm は O1 有効時 3 コンテナ/Pod のサイドカー構成。gNB config は values 内生テキスト + entrypoint スクリプトによる実行時書換(POD_IP / VF BDF / MAC / cgroup CPU)。

## 未確定事項(要ランタイム確認)一覧

1. **3GPP YANG タグの実効値**: Dockerfile ARG は `Tag_Rel19_SA112`、download スクリプト既定は `Tag_Rel18_SA111`。実イメージ内のモジュール revision 要確認(ocudu_netconf:Dockerfile:48, ocudu_netconf:scripts/download_yang_models.sh:6)。
2. **O-RAN YANG モジュール版数**: specifications.o-ran.org `download?id=1035` の中身(WG4 M-Plane リリース版)はコードから特定不能(ocudu_netconf:scripts/download_yang_models.sh:15)。
3. **M-Plane の実機検証範囲**: README 記載の「TDD 100MHz / PRACH format B4 サブセットのみ検証」以上の相互接続実績は不明(ocudu_o1_adapter:README.md:87)。
4. **E2 の RIC 相互運用**: 対向 RIC(OSC RIC 等)との動作実績・E2AP バージョンネゴ挙動はコードから判定不能。
5. **helm values.yaml 単体での O1 有効化**: `o1.healthcheckPort` 等が values-o1.yaml にのみ存在するため、values.yaml 単体で `enable_ocudu_o1=true` とした場合のテンプレート成立性は `helm template` での確認要(ocudu_helm:charts/ocudu-gnb/values-o1.yaml:302-306)。
6. **VES measurement ドメイン**: 現状不在(PM は独自 JSON ストリーム)。SMO 側 (onap-smo-lite) の VES collector が受けるのは fault/pnfRegistration/stateChange のみという理解で運用整合しているかは実環境で確認要。
7. **E2AP ASN.1 ヘッダ表記**: 「3GPP TS ASN1 E2AP v03.00」はコード生成テンプレート由来表記と推定(実体は O-RAN WG3 E2AP)。
