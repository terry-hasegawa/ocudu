# Issue #423 実装前調査レポート

**テーマ:** `p0_nominal_without_grant` を config 可能にする
**方針:** `p0_nominal_with_grant` を流用せず、専用 config option を新設・default `-76`
**調査対象ソース:** `https://gitlab.com/ocudu/ocudu.git` — branch `dev`（commit `6abdc0f` "du: expose per-cell EUTRA coexistance parameter"）
**開発・commit 先:** GitHub fork `terry-hasegawa/ocudu` — branch `claude/ocudu-p0-nominal-without-grant-9b0710`
**日付:** 2026-07-11

> 本レポートの全 file:line は canonical な GitLab `dev` ブランチで検証済み。凡例: **確認事実** = 実コードで確認 / **推奨（判断）** = 調査に基づく評価。実装は未着手（調査のみ）。

---

## 0. エグゼクティブサマリ

- `p0_nominal_with_grant`（common 経路 → SIB1）と `p0_nominal_without_grant`（dedicated 経路 → UE 専用 RRC）は **scheduler 内部の経路が根本的に異なる**（確認事実）。
- したがって「`with_grant` の mirror」は **app 側 3 層（config struct / CLI / yaml）のみ**流用可。scheduler へのthreadingは `p0_pusch_alpha`（既存 dedicated パラメータ）を mirror すべき。
- **推奨は候補(b)**：`pusch_builder_params` に専用フィールドを新設し、factory のハードコードを差し替える。`pusch_builder_params` は SIB packing 経路に一切関与しないため、**SIB への値漏れは構造上あり得ない**（確認事実）。
- dedicated packing（`asn1_rrc_config_helpers.cpp:1946-1948`）は**既に完動**しており、config が値を届ければ自動で通る。**変更不要**。

---

## 1. `p0_nominal_with_grant` の全プランビング経路（mirror テンプレート）

| 層 | file:line | 内容 |
|---|---|---|
| app config struct | `apps/units/flexible_o_du/o_du_high/du_high/du_high_config.h:263` | `int p0_nominal_with_grant = -76;` |
| CLI11 schema | `apps/units/flexible_o_du/o_du_high/du_high/du_high_config_cli11_schema.cpp:938`〜（label `:940`） | `add_option_function<std::string>` で `--p0_nominal_with_grant`、範囲 `[-202,24]` かつ偶数チェック |
| yaml writer | `apps/units/flexible_o_du/o_du_high/du_high/du_high_config_yaml_writer.cpp:329` | `node["p0_nominal_with_grant"] = config.p0_nominal_with_grant;` |
| translator | `apps/units/flexible_o_du/o_du_high/du_high/du_high_config_translators.cpp:714-715` | `...pusch_cfg_common.value().p0_nominal_with_grant = base_cell.pusch_cfg.p0_nominal_with_grant;` ← **common 経路** |
| scheduler config struct | `include/ocudu/scheduler/config/bwp_configuration.h:90`（struct は `:82`） | `pusch_config_common::p0_nominal_with_grant`（**SIB packing 対象の struct**） |
| ran_cell_config default | `lib/scheduler/config/ran_cell_config_helper.cpp:236` | `cfg.init_ul_bwp.pusch_cfg_common.value().p0_nominal_with_grant = -76;` |

**重要な注意（確認事実）:** translator/struct/default は全て `pusch_cfg_common`（common）を経由し、SIB1 に packing される。`without_grant` にこの経路をそのまま流用すると SIB に載ってしまうため、この mirror は **app 3 層（config.h / cli11 / yaml_writer）のみ**流用可。

---

## 2. `make_default_pusch_config()` のシグネチャと最小 threading 経路

**シグネチャ（確認事実）** — `lib/scheduler/config/serving_cell_config_factory.cpp:117`
```cpp
static pusch_config make_default_pusch_config(const ran_cell_config& cell_cfg,
                                              const ue_bwp_config& ue_bwp_cfg)
```

- 関数内 `const auto& cell_pusch_cfg = cell_cfg.init_bwp.pusch;`（`factory.cpp:124`）。
- `cell_cfg.init_bwp` の型は `bwp_builder_params`（`ran_cell_config.h:39`）、`.pusch` は **`pusch_builder_params`**（`bwp_builder_params.h:54`）。
- `factory.cpp:160` で `.p0_nominal_without_grant = -76,` を**無条件ハードコード**。
- **既存の前例**：`p0_pusch_alpha` は `pusch_builder_params` のフィールド（`bwp_builder_params.h:76`）で、`factory.cpp:173-174` が `cell_pusch_cfg.p0_pusch_alpha` を読んで dedicated pwr_ctrl に載せている。→ これが最良の mirror。

### 候補比較

**候補(a): `bwp_configuration.h` の `pusch_config_common` に相乗り**
- factory は `cell_cfg.ul_cfg_common.init_ul_bwp.pusch_cfg_common.value().p0_nominal_without_grant` で到達可能（技術的には可能）。
- **副作用（確認事実）**：`pusch_config_common` は SIB packing 関数 `make_asn1_rrc_initial_up_bwp`（`asn1_rrc_config_helpers.cpp:812`、`pusch_cfg` を読むのは `:828`）が消費する構造体。`without_grant` は本来 dedicated 専用パラメータであり、common 構造体に置くのは**意味論的に誤り**。現状 `:846` 付近では packing されないため即座の bit 漏れは無いが、共通 struct に dedicated 専用値が同居し、将来の誤 packing リスク・translator も common 経路（`:714` 付近）に書く必要が生じる。→ **非推奨**。

**候補(b): `pusch_builder_params` に新フィールド → factory で読む** ✅ **推奨**
- `p0_pusch_alpha`（`bwp_builder_params.h:76` → `translators.cpp:770` → `factory.cpp:173`）と**完全に同一の既存 mirror** をなぞれる。
- `factory.cpp:160` のハードコード `-76` を `cell_pusch_cfg.p0_nominal_without_grant` に差し替えるだけで dedicated pwr_ctrl に到達。
- `pusch_builder_params` は **SIB packing 経路に一切関与しない**（common struct とは別物）ので、`asn1_rrc_config_helpers.cpp:846` 付近への漏れは**構造上あり得ない**（確認事実）。

### 推奨（判断）: **候補(b)**
SIB 副作用ゼロ、既存 `p0_pusch_alpha` の threading をそのまま複製でき、実コード上も最小。

### 候補(b) の具体的な変更内容
1. `bwp_builder_params.h:76` の隣に `int p0_nominal_without_grant = -76;` を追加。
2. `factory.cpp:160` を `.p0_nominal_without_grant = cell_pusch_cfg.p0_nominal_without_grant,` に変更（`std::optional<int16_t>` 先へ int 代入、present は現状同様常に真＝挙動不変）。
3. `translators.cpp:770` の隣に `out_cell.ran.init_bwp.pusch.p0_nominal_without_grant = base_cell.pusch_cfg.p0_nominal_without_grant;` を追加。
4. app 3 層（`du_high_config.h:263` / `cli11_schema.cpp:938` / `yaml_writer.cpp:329`）は §1 の `with_grant` を mirror。

---

## 3. 変更してはいけない末端（確認事実）

| 末端 | file:line | 理由 |
|---|---|---|
| PUSCH power controller | `lib/scheduler/support/pusch_power_controller.cpp:18-19` | `p0_nominal_with_grant` のみ消費。scheduler は `without_grant` を使わない |
| common / SIB packing | `lib/du/.../asn1_rrc_config_helpers.cpp:812`(`make_asn1_rrc_initial_up_bwp`) / `:830` / `:846` | PUSCH-ConfigCommon（SIB1）の packing。`without_grant` は絶対に載せない |
| dedicated packing | `lib/du/.../asn1_rrc_config_helpers.cpp:1933`(`make_asn1_rrc_pusch_pwr_ctrl`) / `:1946-1948` | **既存・変更不要**。`dest.p0_nominal_without_grant.has_value()` を見て packing 済み |
| scheduler common struct | `include/ocudu/scheduler/config/bwp_configuration.h:82-101`（`pusch_config_common`） | SIB struct。ここに without_grant を足さない（＝候補(a)を採らない） |
| translator common 代入群 | `apps/.../du_high_config_translators.cpp:714-715` | common 側 `pusch_cfg_common` への代入 |
| ASN.1 生成コード | `include/ocudu/asn1/rrc_nr/bwp_cfg.h`, `lib/asn1/rrc_nr/bwp_cfg.cpp` | 自動生成の pack/unpack |

---

## 4. set 箇所と config option 未実装の再確認（確認事実・GitLab `dev` で検証）

- **production コードで `p0_nominal_without_grant` を set しているのは `serving_cell_config_factory.cpp:160` の 1 箇所のみ**。他の一致は次のとおり、いずれも消費側 packing かテスト:
  - `asn1_rrc_config_helpers.cpp:1946-1948` … 読み取り（packing 側）
  - `lib/asn1/rrc_nr/bwp_cfg.cpp` … ASN.1 pack/unpack
  - `tests/unittests/du_manager/serving_cell_config_converter_test.cpp:39` … テスト入力生成
- **config option は未実装**：`apps/` 配下に `p0_nominal_without_grant` への言及ゼロ（grep 該当なし）。`du_high_config.h` / `cli11_schema.cpp` / `yaml_writer.cpp` / translator / `pusch_builder_params` のいずれにも存在しない。

---

## 5. config → packed dedicated `p0_nominal_without_grant` を検証する unit test

**既存で値検証している test は無い（確認事実）:**
- `tests/unittests/du_manager/serving_cell_config_converter_test.cpp:39` は入力 config に `.p0_nominal_without_grant = -76` を**設定はする**が、packing 結果の assertion 群（同ファイル `:548`, `:569` 付近）は `pusch_pwr_ctrl_present` / `pathloss_ref_rs` / `sri_pusch_map` / `p0_alpha_sets` のサイズしか見ておらず、**`p0_nominal_without_grant_present` / 値は検証していない**。

**推奨新設先（判断）:** 同ファイルの packing assertion ブロック（`:548` 付近）に以下を追加:
```cpp
ASSERT_TRUE(rrc_sp_cell_cfg_ded.ul_cfg.init_ul_bwp.pusch_cfg.setup()
                .pusch_pwr_ctrl.p0_nominal_without_grant_present);
ASSERT_EQ(rrc_sp_cell_cfg_ded.ul_cfg.init_ul_bwp.pusch_cfg.setup()
              .pusch_pwr_ctrl.p0_nominal_without_grant,
          /* 期待値 */);
```
- test target: `serving_cell_config_converter_test`（CMake: `tests/unittests/du_manager/CMakeLists.txt:41-43`）。
- app config → translator の end-to-end も担保したい場合は `apps/units/.../du_high/` 配下の config translator test への追加を検討（範囲外なら上記 1 箇所で十分）。

---

## 6. `.clang-format` / ビルド target / test 実行コマンド（確認事実）

- **`.clang-format`**: リポジトリ root（`/home/user/ocudu/.clang-format`）。`external/.clang-format` は外部依存用で無関係。
- **ビルド target**:
  - dedicated packing 検証 test: `serving_cell_config_converter_test`
  - factory を含む scheduler lib / app 側 lib（`ocudu_du_high` 系）
- **ビルド & test 実行**（srsRAN 標準フロー、build dir は新規作成）:
  ```sh
  mkdir -p build && cd build
  cmake ..
  make -j$(nproc) serving_cell_config_converter_test
  ./tests/unittests/du_manager/serving_cell_config_converter_test
  # または:
  ctest -R serving_cell_config_converter_test --output-on-failure
  ```
- **clang-format 適用**:
  ```sh
  clang-format -i <変更ファイル>
  ```

---

## 7. まとめ

### 変更する箇所リスト（推奨＝候補(b)、GitLab `dev` 行番号）

1. `include/ocudu/scheduler/config/bwp_builder_params.h:76` 付近 — `pusch_builder_params` に `int p0_nominal_without_grant = -76;` 追加。
2. `lib/scheduler/config/serving_cell_config_factory.cpp:160` — ハードコード `-76` を `cell_pusch_cfg.p0_nominal_without_grant` へ。
3. `apps/units/flexible_o_du/o_du_high/du_high/du_high_config.h:263` 付近 — `int p0_nominal_without_grant = -76;` 追加（`with_grant` を mirror）。
4. `apps/units/flexible_o_du/o_du_high/du_high/du_high_config_cli11_schema.cpp:938` 付近を mirror — `--p0_nominal_without_grant` option（範囲 `[-202,24]`・偶数チェック）追加。
5. `apps/units/flexible_o_du/o_du_high/du_high/du_high_config_yaml_writer.cpp:329` 付近 — `node["p0_nominal_without_grant"]` 追加。
6. `apps/units/flexible_o_du/o_du_high/du_high/du_high_config_translators.cpp:770` 付近 — `out_cell.ran.init_bwp.pusch.p0_nominal_without_grant = base_cell.pusch_cfg.p0_nominal_without_grant;` 追加（**common 経路の `:714` ではなく dedicated 経路の `:770` 側**）。
7. （テスト）`tests/unittests/du_manager/serving_cell_config_converter_test.cpp:548` 付近 — packed `p0_nominal_without_grant_present` / 値の assertion 追加。

### 変更してはいけない箇所リスト

1. `lib/scheduler/support/pusch_power_controller.cpp:18-19`（scheduler は without_grant 不消費）。
2. `lib/du/du_high/du_manager/converters/asn1_rrc_config_helpers.cpp:812 / 830 / 846`（`make_asn1_rrc_initial_up_bwp` = common/SIB packing）。
3. `lib/du/du_high/du_manager/converters/asn1_rrc_config_helpers.cpp:1933 / 1946-1948`（`make_asn1_rrc_pusch_pwr_ctrl` = dedicated packing、既存で完動・変更不要）。
4. `include/ocudu/scheduler/config/bwp_configuration.h:82-101`（`pusch_config_common`、SIB struct — ここに without_grant を足さない＝候補(a)を採らない）。
5. `apps/units/flexible_o_du/o_du_high/du_high/du_high_config_translators.cpp:714-715`（common 側 `pusch_cfg_common` への代入群）。
6. `include/ocudu/asn1/rrc_nr/bwp_cfg.h` / `lib/asn1/rrc_nr/bwp_cfg.cpp`（自動生成 ASN.1）。

---

## 付録: 確認事実と推測の区別

- **確認事実**: 全 file:line、型、set 箇所、test の未検証、common/dedicated 経路の分離、config option 未実装 — GitLab `dev`（`6abdc0f`）の実コードで検証済み。
- **推奨（判断）**: 「候補(b) が最小」「候補(a) の意味論的誤り・将来リスク」、§5 の test 新設先、§6 のビルド/test コマンド。
