# MR description A1 (target: `dev`)

**Title:** du_high: wire f4_enable_occ into pucch builder

## What

- Copy the `pucch: f4_enable_occ` option into `pucch_f4_params::occ_supported` in the du_high
  config translator (FORMAT_4 case), mirroring the existing Format 1 handling
  (`f1_params.occ_supported = user_pucch_cfg.f1_enable_occ`).
- Emit the missing `f4_enable_occ` key in the du_high yaml writer.

## Why

`--f4_enable_occ true` was silently ignored: the translator never set `occ_supported`, so
`pucch_resource_generator` generated `nof_occs = 1` (one UE per F4 PRB) and
`mux_capacity_234()` reported no multiplexing capacity. This also made `f4_occ_length`
ineffective, since it is only read when OCC is enabled.

## Behavior change

None for default configs (`f4_enable_occ` defaults to `false`). Configs that set
`f4_enable_occ: true` now actually enable OCC multiplexing (2 or 4 UEs per F4 resource,
per `f4_occ_length`).

## Validation

- `sched_config_test` and `serving_cell_config_converter_test` green in a
  `-DBUILD_TESTING=ON` build.
- `clang-format` clean.

Closes #<issue>
