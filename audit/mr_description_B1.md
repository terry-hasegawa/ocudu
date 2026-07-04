# MR description B1 (target: `dev`)

**Title:** sched: derive max mimo layers from builder params

## What

Set `pdsch_serving_cell_config::max_mimo_layers` from `pdsch_builder_params::max_nof_layers`
(fallback 1 when unset) instead of hardcoding 1 in `make_default_pdsch_serving_cell_config()`.
Add a positive unit test.

## Why

The factory receives the configured maximum number of DL layers but ignored it, so every UE was
signaled `PDSCH-ServingCellConfig.maxMIMO-Layers = 1` — the only writer of this field in the tree.
That contradicts the CSI-MeasConfig generated from the same parameter (RI restriction allows ranks
up to `max_nof_layers`, `csi_helper.cpp:683`) and skews the UE-side LBRM TBS computation
(TS 38.212, Section 5.4.2.1) away from the gNB's fixed `tbs_lbrm_default` assumption. UEs that
honor the field cap DL transmissions at 1 layer.

Same pattern as #423: a factory hardcode shadowing an existing config value.

## Behavior change

- Cells with one DL antenna port or `pdsch: max_rank: 1`: no change.
- Multi-antenna cells: `maxMIMO-Layers` now matches the configured maximum rank (intended fix).
  OTA validation with COTS UEs recommended, since this changes an RRC field every UE receives.

## Validation

- New `serving_cell_config_factory_test` (2 cases: derived from antenna ports; explicit
  `max_nof_layers`) green.
- `sched_config_test`, `serving_cell_config_converter_test` green in a `-DBUILD_TESTING=ON` build.
- `clang-format` clean.

Closes #<issue>
