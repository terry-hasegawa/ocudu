# Issue draft F1 (type: bug)

**Title:** ran: prach_config_index_is_valid rejects supported format A2 configuration indices

## Summary

`prach_helper::prach_config_index_is_valid()` (`lib/ran/prach/prach_helper.cpp`) gates the
PRACH configuration index against hard-coded "supported" intervals:

- FR1 FDD: `{0, 108}, {147, 167}, {198, 256}` (half-open)
- FR1 TDD: `{0, 87}, {110, 133}, {145, 211}` (half-open)

These interval lists skip the preamble format A2 rows of TS 38.211:

- Table 6.3.3.2-2 (FR1 paired): indices 117–136 are format A2,
- Table 6.3.3.2-3 (FR1 unpaired): indices 87–109 are format A2.

However, format A2 is fully supported by the rest of the code base:

- `prach_configuration_get()` (`lib/ran/prach/prach_configuration.cpp`) implements all A2 rows
  of both tables (machine-checked: FR1 paired 117–136, FR1 unpaired 87–109 are A2 entries,
  matching the TS tables; the reserved gaps around them are the combined A1/B1, A2/B2, A3/B3
  and B1 formats, which are genuinely not implemented).
- `get_prach_preamble_short_info()` provides the A2 preamble parameters (4 symbols,
  CP 576·2^-µ·κ), as per TS 38.211 Table 6.3.3.1-2.
- The PHY PRACH detector has calibrated thresholds for A2
  (`lib/phy/upper/channel_processors/prach/prach_detector_generic_thresholds.cpp`, 96 A2
  entries — the same coverage as the accepted A3/C2 formats).

As a result, configuring any valid A2 `prach-ConfigurationIndex` (e.g. 87–109 for a TDD FR1
cell) makes the scheduler cell-config validator (`validate_rach_cfg_common()`) reject the
cell with "PRACH configuration index ... not supported", and
`prach_helper::find_valid_prach_config_index()` never selects an A2 index either. Formats A1
and A3, which bracket A2 in the tables and have identical structural properties (short
preamble, multiple occasions per slot), are accepted.

This looks like an accidental omission of the A2 rows from the interval lists rather than a
deliberate restriction (no comment, no test asserting the exclusion, and the PHY explicitly
supports A2).

## Affected spec references

- TS 38.211, Table 6.3.3.2-2 (random access configurations for FR1 paired spectrum): rows
  117–136 = preamble format A2.
- TS 38.211, Table 6.3.3.2-3 (FR1 unpaired spectrum): rows 87–109 = preamble format A2.

## Proposed fix

Add the A2 intervals to the supported lists in `prach_config_index_is_valid()`:

- FR1 FDD: add `{117, 137}`
- FR1 TDD: add `{87, 110}`

(Equivalent, simpler alternative: merge the now-contiguous TDD intervals `{0, 87}` +
`{87, 110}` + `{110, 133}` into `{0, 133}`.)

Out of scope (can be follow-ups): FR1 TDD indices 256–262 (Rel-16 extension rows,
implemented in the table but capped by the validator and by
`find_valid_prach_config_index()`s 0..255 loop), and the FR2 list, which currently accepts
only B4 (112–143) while the table implements A1/A2/A3/C0/C2 as well — whether FR2 short
formats other than B4 are production-ready needs separate confirmation.

## Backward compatibility

Purely permissive change: all previously accepted indices remain accepted; previously
rejected (but spec-valid and implementation-supported) A2 indices become usable. No change
to defaults; `find_valid_prach_config_index()` keeps returning the same index for existing
TDD patterns unless an A2 index becomes the first valid candidate — to be double-checked in
review (the search starts at index 0 and formats 0/3/A1 occupy lower indices for FR1 TDD, so
the selected default index is unchanged for common configs).

## Validation

- New unit test: `prach_config_index_is_valid()` accepts a representative A2 index per
  duplex mode (e.g. 117 FDD, 87 TDD) and still rejects reserved rows (e.g. 108 FDD, 133 TDD).
- Cross-check test: for every index where `prach_configuration_get()` returns a non-invalid
  short/long format (excluding 256–262 and FR2 non-B4), `prach_config_index_is_valid()`
  accepts it — keeps table and validator in sync going forward.
- Existing suites: tests/unittests/ran/prach/* remain green.
