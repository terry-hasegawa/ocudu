# Issue draft A1 (type: bug)

**Title:** `pucch --f4_enable_occ` is silently ignored: OCC multiplexing never enabled for PUCCH Format 4

## Summary

The du_high unit exposes `pucch: f4_enable_occ` (CLI `--f4_enable_occ`, declared in
`apps/units/flexible_o_du/o_du_high/du_high/du_high_config.h:450`), but the config translator never
copies it into the PUCCH builder parameters. In
`du_high_config_translators.cpp` (FORMAT_4 case), `pucch_f4_params.occ_supported` is left at its
default `false`, so setting `f4_enable_occ: true` has no effect. In addition, the yaml writer does
not emit the `f4_enable_occ` key, so the option is also lost on config dump round-trips.

## Motivation

- `occ_supported` gates all F4 OCC behavior:
  - `lib/scheduler/config/pucch_resource_generator.cpp:121`: `nof_occs = occ_supported ? occ_length : 1`
    — with the flag stuck at `false`, only one UE is mapped per F4 PRB instead of `occ_length` (2 or 4).
  - `include/ocudu/scheduler/config/pucch_resource_builder_params.h:394`: `mux_capacity_234()` returns 1.
- As a consequence `f4_occ_length` is effectively dead as well, since it is only meaningful with OCC enabled.
- The equivalent Format 1 knob is wired correctly
  (`f1_params.occ_supported = user_pucch_cfg.f1_enable_occ`, translators line ~866), which shows the
  F4 assignment was simply missed. Same shape as the `pusch_channel_estimator_fd_strategy` dead-knob.

## Proposed fix

Two lines:

1. `du_high_config_translators.cpp`, FORMAT_4 case:
   `f4_params.occ_supported = user_pucch_cfg.f4_enable_occ;`
2. `du_high_config_yaml_writer.cpp`: `node["f4_enable_occ"] = config.f4_enable_occ;`

## Backward compatibility

Default behavior unchanged: `f4_enable_occ` defaults to `false` and `occ_supported` defaults to
`false`, so configurations that do not set the option are unaffected. Only configurations that
already set `f4_enable_occ: true` (currently a no-op) change behavior — they start getting what
they asked for.

## Validation

- Build with `-DBUILD_TESTING=ON`; `sched_config_test` (includes `pucch_resource_generator_test`)
  and `serving_cell_config_converter_test` pass.
- A dedicated positive test would require a test scaffold for the du_high unit translators, which
  does not exist today; the change is a one-line data plumb mirroring the existing F1 handling.
