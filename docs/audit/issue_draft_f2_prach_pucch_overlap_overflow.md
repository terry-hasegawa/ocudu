# Issue draft F2 (type: bug)

**Title:** du: fix uint8_t overflow in prach/pucch overlap validation

## Summary

In `check_prach_config()` (`lib/du/du_cell_config_validation.cpp`):

```cpp
const uint8_t prach_prb_stop =
    rach_cfg.rach_cfg_generic.msg1_frequency_start + rach_cfg.rach_cfg_generic.msg1_fdm * prach_nof_prbs;
```

`prach_prb_stop` is computed into a `uint8_t`. `msg1-FrequencyStart` ranges up to 274
(TS 38.331 RACH-ConfigGeneric: `msg1-FrequencyStart INTEGER (0..maxNrofPhysicalResourceBlocks-1)`),
`msg1-FDM` up to 8, and a short-preamble PRACH occasion occupies 12 PRBs
(TS 38.211 Table 6.3.3.2-1), so the true stop PRB can exceed 255 and wraps modulo 256.

When it wraps, the subsequent check

```cpp
CHECK_TRUE(prach_prb_stop + pucch_to_prach_guardband <= prb_interval_no_pucch.stop(), ...)
```

passes vacuously, so a configuration whose PRACH occasions genuinely overlap the PUCCH
resources (or violate the guardband) is accepted. Example that wraps while still fitting the
BWP: 100 MHz / 273-PRB BWP, short preamble (12 PRBs), `msg1_fdm = 8`,
`msg1_frequency_start = 160` → stop = 256 → wraps to 0 → check passes even if PUCCH
resources occupy the upper PRBs.

Note the sibling check in `check_ul_config_common()` computes the same quantity as
`unsigned` (`prach_prb_end`) and is not affected; only the PRACH-vs-PUCCH overlap check can
be silently bypassed. The result is an accepted cell configuration where PRACH and PUCCH
collide in frequency, degrading random access and/or UCI reception at runtime.

## Affected spec references

- TS 38.331 RACH-ConfigGeneric: `msg1-FrequencyStart` value range (0..274) — the input range
  that makes the 8-bit computation overflow.
- TS 38.211 Table 6.3.3.2-1: number of PRBs per PRACH occasion (e.g. 12 for L_RA = 139 with
  equal PUSCH/PRACH SCS).

## Proposed fix

Compute the stop PRB in a wide type:

```cpp
const unsigned prach_prb_stop = ...;
```

(One-line type change; the comparison already promotes correctly once the value is not
truncated.)

## Backward compatibility

Strictly tightening: configurations that were correctly accepted remain accepted; only
configurations that mathematically overlap PUCCH (and were passing due to wrap-around) are
now rejected with the existing, accurate error message.

## Validation

- New unit test: a du_cell_config with 273-PRB BWP, short-format PRACH, `msg1_fdm = 8`,
  `msg1_frequency_start` chosen so start+8*12 ≥ 256 and overlapping PUCCH resources → config
  must be rejected; a non-overlapping wide config must stay accepted.
- Existing du_manager / config validation suites remain green.
