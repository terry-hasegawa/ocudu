# ISAC sensing PoC 手順書（Phase 1 → Phase 2）

デモ・動作確認のための運用手順。**Phase 1（UL 信号だけ見る = motion 検出のみ）で疎通と検出を確認してから、Phase 2（AI ゾーンキャリブレーション = 部屋ヒートマップ）に移行する**。

- 見た目の完成形（合成データのモック）: `web/mock_zone_demo.html` をブラウザで開くだけで確認可能。
- 本番画面は `web/index.html`（`server.py` が配信・給電）。

---

## 0. 前提（一度だけ）

### 0-1. gNB（Block A）ビルド
```bash
# libzmq が必要（Ubuntu: libzmq3-dev）
cmake -DENABLE_ISAC_TAP=ON .. && ninja gnb
```
- `ENABLE_ISAC_TAP=OFF`（既定）でビルドすれば従来動作と完全同一（tap はコンパイルされない）。

### 0-2. gNB config（rank-1 × 4R）
```yaml
cell_cfg:
  nof_antennas_ul: 4     # 4R 受信
  pusch:
    max_rank: 1          # rank-1 明示（これが無いと 4R で最大 4 層になる）
```
- split 7.2 (OFH) の場合は RU 側の UL ポートを 4 つ宣言（`ru_ul_port_id` が 4 要素以上）。

### 0-3. Block D（Python）
```bash
cd utils/isac_blockd
pip install -r requirements.txt
python3 -m pytest tests/   # 18 件パスすることを確認
```

### 0-4. illuminator（UE）
- test UE 1 台。`iperf3 -u -c <コア側> -b 30M -t 86400` などで **UL を常時**流す（PUSCH DMRS が snapshot 源）。
- 複数 UE がいても Block D は最初に見えた RNTI にロックする（明示するなら `--rnti 0x4601`）。

---

## Phase 1: UL 信号だけ見る（motion 検出の動作確認）

**目的**: TAP→ZMQ→検出→画面の疎通と、閾値キャリブ＋motion 検出の成立を確認する。ゾーン機能は使わない。

### 1-1.（任意）gNB なしのドライラン
```bash
# 端末1: 合成 Block A
python3 fake_blocka.py --bind tcp://127.0.0.1:5599 --rate 120
# 端末2: 検出サーバ（Phase 1 = --zones なし）
python3 server.py --zmq tcp://127.0.0.1:5599
# ブラウザ: http://localhost:8080/index.html?ws=8765
```
- waterfall が流れ、数秒後に `calibrating → clear`、時々 `motion detected` が出れば配管は正常。

### 1-2. 実機
```bash
# gNB 側（起動前に環境変数で tap を有効化）
export OCUDU_ISAC_ZMQ_ENDPOINT="tcp://*:5599"
./gnb -c gnb.yaml
# 起動ログに以下が出ること:
#   [isac] CSI tap publishing on 'tcp://*:5599' (queue_depth=16).

# 検出ホスト側
python3 server.py --zmq tcp://<gnb-host>:5599
# ブラウザで index.html を開く
```

### 1-3. 確認チェックリスト
| 項目 | 期待値 |
|---|---|
| ヘッダの `layers/Rx` | `1L / 4R`（rank-1×4R config が効いている証拠） |
| snapshot rate | UL grant レート相当（iperf 中なら数十〜数百/s） |
| waterfall | 4 branch とも生きている（死んだ branch は RU 配線を疑う） |
| キャリブ | 起動後 `calibrating` → 数秒（無人厳守）→ `clear` に遷移 |
| 検出 | sensing field を人が横切ると `motion detected`（amber）、静止で `clear` へ復帰 |
| 誤検出 | 無人で放置して banner が clear のまま（出るなら `--k 5` へ） |

### 1-4. 感度チューニング（必要時のみ）
- 感度不足 → `--combine snr` または `--combine max`
- 過敏 → `--k 5` / `--hold 16`
- gNB を再起動した場合: 検出器は自動 re-arm（再キャリブ）される。画面の `re-arm detector` ボタンでも手動再キャリブ可能。

**Phase 1 合格基準: 上記チェックリストが全部 green。ここまで確認できたら Phase 2 へ。**

---

## Phase 2: AI ゾーンキャリブレーション（部屋ヒートマップ）

**目的**: 部屋を模した四角の中の「どこにいるか」をヒートマップ表示。ラベル付きキャプチャから nearest-centroid 分類器を**その場で学習**する（＝AI キャリブレーション）。

### 2-1. 起動（Phase 1 との差分は `--zones` のみ）
```bash
python3 server.py --zmq tcp://<gnb-host>:5599 --zones A,B,C,D --zone-grid 2x2
```
- 画面に **zone heatmap パネル**（部屋の 2×2 グリッド＋capture ボタン）が現れる。
- ゾーンの物理配置を決める（例: 部屋を田の字に A=左上 / B=右上 / C=左下 / D=右下）。UI のセル並びは行優先。

### 2-2. キャリブレーション台本（1 ラベル 15 秒 × 5）
1. **全員退出** → `capture empty` をクリック → 15 秒待つ（`collecting:empty` → 完了）
2. 被験者が **ゾーン A に立つ**（静止）→ `capture A` → 15 秒
3. 同様に B → C → D
4. `zstate` が `ready [A,B,C,D,empty]` になったら **`save`**（`zones.json` に保存。次回は `load` で復元可）

注意:
- キャプチャ中は**その状態を維持**（empty 中に入らない、A 収集中に動かない）。
- **部屋・家具・UE の位置を変えたら再キャリブ**（fingerprint は環境固有）。`clear` で破棄して撮り直し。
- アンテナ数が変わった場合は自動で fingerprint 無効化される。

### 2-3. デモ動作
- 被験者がゾーンに立つ → 該当セルが amber に点灯（確率で濃淡）、`presence: YES`。
- 歩行中は motion banner（Phase 1 機能）が発火し、ヒートマップは移動に追従して滲む。
- 全員退出 → 全セル消灯、`presence: no`。
- 説明トーク例: 「起動時にその場で採取したラベル付き CSI から **AI（最近傍分類器）をキャリブレーション**しています。学習データを増やせば同じ枠組みで RandomForest/CNN に差し替え可能です」

### 2-4. D2-b: 学習済みモデルへのアップグレード（任意）

centroid 方式で精度が足りない場合、同じ台本で録画 → RandomForest を訓練 → 差し替え。
**ラベル名は runtime のゾーン名と完全一致させる**（`empty` / `A` / `B` / `C` / `D`）。

```bash
# (1) 録画（キャリブ台本と同じ状況で。各 15-20 秒）
python3 recorder.py --label empty --seconds 20 --out s1_empty.npz
python3 recorder.py --label A     --seconds 20 --out s1_A.npz
python3 recorder.py --label B     --seconds 20 --out s1_B.npz   # C, D も同様

# (2) 訓練（RandomForest。--model logreg も可）
python3 train_zones.py --data 's1_*.npz' --out zones_model.joblib

# (3) モデルで起動（fingerprint の代わりに学習済みモデルを使用）
python3 server.py --zmq tcp://<gnb-host>:5599 --zones A,B,C,D --zone-model zones_model.joblib
# 画面の zone パネルに「· D2-b model」と表示され、キャリブ操作なしで即 ready になる
```

- 表示される CV accuracy は**同一セッション内評価なので楽観的**。正直な数字が要る場合は
  もう 1 セッション録画して `--eval 's2_*.npz'` でホールドアウト評価する。
- `.npz` 内容: `hmag (n, nof_rx, bins)` / `meta (seq, ts, prb...)` / `label` / `rnti`。
- アンテナ数を変えたらモデルは自動無効化（要・再録画/再訓練）。

---

## トラブルシューティング

| 症状 | 確認・対処 |
|---|---|
| 画面にデータが来ない | gNB 起動ログに `[isac] CSI tap publishing` があるか（無ければ env 未設定 or `ENABLE_ISAC_TAP` 無しビルド）。`--zmq` の host:port。UL トラフィックが流れているか（grant が無いと snapshot ゼロ） |
| `[isac] failed to bind` | ポート使用中。前 gNB の残骸を kill。次回 init で自動リトライされる |
| rate が異常に低い | iperf の帯域を上げる（UL grant 頻度＝frame レート） |
| banner が calibrating のまま | キャリブ窓中に UL が途切れている（iperf 確認）。gNB 再起動直後なら自動 re-arm 後に無人 4 秒を確保 |
| 誤検出が多い | 無人キャリブの徹底 / `--k 5` / エアコン等の可動物を止める |
| ヒートマップが特定ゾーンに張り付く | そのゾーンの fingerprint が他と近すぎる。ゾーン間隔を広げる or `clear` して撮り直し |
| 別 UE に反応している | `--rnti 0x....` で対象 UE を明示 |
| malformed message カウント増加 | 同一 port に別 publisher が居ないか。Block A/D のバージョン不一致（wire v2 同士か） |

## 関連ファイル
- 本番画面: `web/index.html` ／ モック: `web/mock_zone_demo.html`
- wire 仕様: `lib/isac/README.md`（v2, 100B ヘッダ）
- Block D 詳細: `README.md`（このディレクトリ）
