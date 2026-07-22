# OCUDU_MPLANE — M-Plane(O-RU 向け NETCONF クライアント)

対象実装はすべて ocudu_o1_adapter リポジトリ(`src/ru_controller.py` = スタンドアロン CLI、`src/ru_forwarder.py` = O1 アダプタ内蔵転送)。ocudu 本体(C++)には M-Plane NETCONF 実装は存在しない(lib/include/apps に netconf 系コードなし。REPO_ROLE_MAP 参照)。

## 比較表

| Axis | OCUDU finding | Evidence (repo:path:line) | Self / in-house AI-RAN |
|---|---|---|---|
| 1. Conformance spec + version | O-RAN WG4 M-Plane 相当の YANG namespace は全て `:1.0`(urn:o-ran:uplane-conf:1.0 等)。call-home は RFC 8071 と明記。supervision は o-ran-supervision watchdog-reset RPC。仕様書番号・バージョンへの明示参照はコードに**なし**(モジュール改定版はサーバ側ダウンロード物に依存) | ocudu_o1_adapter:src/ru_controller.py:66-74,317,286 / ocudu_o1_adapter:templates/mplane/oran_supervision_watchdog_reset.xml:6-8 | |
| 2. Protocol / transport | NETCONF over SSH(ncclient)。direct connect(既定、host:830)または `--callhome`(RFC 8071、listen 4334)。hostkey_verify=False 固定。TLS は M-Plane 側には**なし**(TLS 実装は O1 サーバ向けのみ) | ocudu_o1_adapter:src/ru_controller.py:310-320,389-447 | |
| 3. Model coverage(使用 o-ran YANG) | クライアントが読み書きするモジュール: o-ran-uplane-conf, o-ran-processing-element, o-ran-interfaces, ietf-interfaces, ietf-hardware, o-ran-sync, o-ran-supervision, (転送フィルタに) o-ran-performance-management。DU 側 NETCONF サーバ(ru/du プロファイル)には加えて o-ran-module-cap / delay-management / mplane-int / fm / dhcp / certificates / software-management / file-management / usermgmt / troubleshooting / operations / compression-factors / wg4-features を登録 | ocudu_o1_adapter:src/ru_controller.py:66-74,195-227 / ocudu_o1_adapter:src/ru_forwarder.py:23-31 / ocudu_netconf:scripts/setup_ru.sh:29-48 | |
| 4. Implementation maturity | edit-config(merge)による U-Plane 設定・キャリア活性化・supervision keep-alive・get-config 読み出しは実装済。**部分実装**: software-management / file-management / fm 購読 / delay-management 測定等の RPC ハンドリングは**未実装**(モジュール登録のみ)。perf_measurement 設定はコメントアウト。frame_structure 値に TODO。「full config は TDD 100MHz, PRACH B4 等サブセットのみ検証」と README 明記 | ocudu_o1_adapter:src/ru_controller.py:116(`# self.set_oran_perf_measurement()`),481(`# TODO: verify frame structure values`) / ocudu_o1_adapter:README.md:87 | |
| 5. Config-injection & extension points | RU 個体差パラメータは CLI 引数(--ru_mac_addr/--vlan/--du_mac_addr/--iq_bitwidth/--compression_type/--rf_bandwidth_hz/--dl_arfcn/--dl_freq/--tx_gain 等)→ Jinja2 XML テンプレート(templates/mplane/*.xml、12 種)に注入。**ハードコード**: TDD パターンは固定テンプレート `oran_uplane_tdd_7d1s2u_slot_6_4_4.xml`、PRB 数は帯域→固定表(100→273 等)、low-level tx/rx links はパラメータ無しテンプレート。O1 経由では `ocudu_nrcelldu_ofh_extensions`(独自 YANG)が OFH セル設定の注入点 | ocudu_o1_adapter:src/ru_controller.py:462-518,472-478 / ocudu_o1_adapter:templates/mplane/(ファイル一覧) / ocudu_helm:charts/ocudu-gnb/values-o1.yaml:546-577 | |
| 6. Known vendor deviations / workarounds | (a) ncclient call_home の 10s accept timeout と O-RU の 60s 再試行タイマの競合をリトライループで回避、EADDRINUSE(ncclient issue #578)も明示 workaround。(b) supervision はタイマ駆動でなく **notification 駆動のみ**(初回リセットも送らない)という設計判断をコメントで明記。(c) RU→DU 同期は許可 namespace 7 種にフィルタして子要素単位で流し込み、失敗要素はスキップ続行 | ocudu_o1_adapter:src/ru_controller.py:403-417,275-283 / ocudu_o1_adapter:src/ru_forwarder.py:23-31,116-135 | |
| 7. Known interop constraints | supervision 既定 interval=60s / guard=10s。デフォルト認証は root 固定パスワード類(--password 既定値あり)。call-home ポート 4334(標準の SSH call-home ポート)。O-RU が supervision-notification を自発送信しない実装だと watchdog が回らない(notification 駆動前提)。dry-run モードあり。転送は edit-config merge のみで replace 非対応(operation="merge" 固定は RuConfig.operation) | ocudu_o1_adapter:src/ru_controller.py:362-365,312,319,62 / ocudu_o1_adapter:src/ru_forwarder.py:126-132 | |

## 証跡ノート

### call-home フロー(確認済)
- `--callhome` 指定時、`manager.call_home(host=bind, port=4334)` で O-RU からの inbound SSH を待ち受け。TimeoutError は「O-RU の再試行間隔の方が長い」ため無限リトライ、EADDRINUSE は 1 秒待ちリトライ(ocudu_o1_adapter:src/ru_controller.py:389-417)。
- direct 接続は `manager.connect(hostkey_verify=False, look_for_keys=False)`(ocudu_o1_adapter:src/ru_controller.py:421-429)。

### supervision / watchdog(確認済)
- `create_subscription()` → `take_notification(timeout=interval+guard)` で `{urn:o-ran:supervision:1.0}supervision-notification` を待ち、受信ごとに `supervision-watchdog-reset` RPC(interval, guard-timer-overhead)を dispatch。タイムアウトは警告ログのみで継続(ocudu_o1_adapter:src/ru_controller.py:275-303, ocudu_o1_adapter:templates/mplane/oran_supervision_watchdog_reset.xml:6-9)。

### config-push アーキテクチャ(確認済)
- スタンドアロン: `set_full_config` = ietf-interfaces → processing-elements → tx/rx endpoints → tx/rx array-carriers → low-level links → carrier-active → TDD 固定パターン、の順に edit-config merge(ocudu_o1_adapter:src/ru_controller.py:106-118)。
- O1 統合時(`--ru_forward`): 起動時に RU running を取得し許可 namespace のみ DU 側 NETCONF サーバへ merge(RU→DU 一方向ブートストラップ、ocudu_o1_adapter:src/ru_forwarder.py:100-158)。以後は DU 側の設定変更を queue 経由で RU へ転送、queue は最新 1 件のみ保持(古い更新は破棄)(ocudu_o1_adapter:src/config_manager.py:358-375, ocudu_o1_adapter:src/ru_forwarder.py:199-240)。
- 転送許可 namespace(SYNC_ALLOWED_NAMESPACES): netconf base, iana-if-type, ietf-interfaces, urn:o-ran:interfaces:1.0, urn:o-ran:performance-management:1.0, urn:o-ran:processing-element:1.0, urn:o-ran:uplane-conf:1.0(ocudu_o1_adapter:src/ru_forwarder.py:23-31)。

### RPC ハンドリング(確認済)
- RPCError から path / bad-element を抽出してログ後 exit(1)(スタンドアロン時)(ocudu_o1_adapter:src/ru_controller.py:76-104)。ru_forwarder 側は warning ログでスキップ継続(ocudu_o1_adapter:src/ru_forwarder.py:220-227)。

### 未確認・推定事項
- 実 O-RU との相互接続実績はコードからは README の「TDD 100MHz / PRACH format B4 で検証」以上は不明(ランタイム確認要)。
- o-ran YANG モジュール版数(specifications.o-ran.org アーカイブ id=1035 の中身)はビルド成果物依存(ランタイム確認要)。
