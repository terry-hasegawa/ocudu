# MR description A5 (target: `dev`)

**Title:** du_high: remove unused autostart metrics field

## What

Remove `du_high_unit_metrics_config::autostart_stdout_metrics`.

## Why

The field's declaration is its only reference in the tree: it has no CLI binding, is not written
by the yaml writer, and is never read. The working `autostart_stdout_metrics` knobs live in the
application-level configs (`gnb_appconfig` / `du_appconfig`), which are bound and consumed; the
du_high copy is a misleading leftover.

## Behavior change

None — the field had no configuration surface and no reader.

## Validation

- du_high unit helpers and dependent test targets compile and pass in a `-DBUILD_TESTING=ON`
  build (`sched_config_test`, `serving_cell_config_converter_test`).
- `clang-format` clean.

Closes #<issue>
