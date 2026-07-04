# Issue draft A5 (type: bug / cleanup)

**Title:** `du_high_unit_metrics_config::autostart_stdout_metrics` is dead: no CLI binding, no reader

## Summary

`apps/units/flexible_o_du/o_du_high/du_high/du_high_config.h` declares
`bool autostart_stdout_metrics = false;` inside `du_high_unit_metrics_config`, but a repository-wide
search shows the declaration is its only reference: no CLI schema binds it, no yaml writer emits
it, no translator or service reads it.

## Motivation

The application-level configs (`gnb_appconfig.h` / `du_appconfig.h`) have their own
`autostart_stdout_metrics` fields, which are properly bound to CLI and consumed in `gnb.cpp` /
`du.cpp`. The identically named du_high field is a leftover that suggests a per-unit knob exists
when it does not — anyone setting it programmatically (or looking for the du_high metrics
autostart behavior) is misled.

## Proposed fix

Remove the field (one line). No other code references it.

## Backward compatibility

No behavior change: the field has no CLI/yaml surface and no reader. Removing it cannot affect
any configuration file or runtime path.

## Validation

- `ocudu_du_high_unit_helpers` and dependent targets compile in a `-DBUILD_TESTING=ON` build;
  `sched_config_test` and `serving_cell_config_converter_test` remain green.
