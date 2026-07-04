# Issue draft F3 (type: bug)

**Title:** du: nzp-csi-rs resource set repetition always encoded as on

## Summary

In `make_asn1_nzp_csi_rs_resource_set()`
(`lib/du/du_high/du_manager/converters/asn1_csi_meas_config_helpers.cpp`):

```cpp
if (cfg.is_repetition_on.has_value()) {
  out.repeat_present = true;
  out.repeat = cfg.is_repetition_on ? nzp_csi_rs_res_set_s::repeat_opts::on
                                    : nzp_csi_rs_res_set_s::repeat_opts::off;
}
```

`cfg.is_repetition_on` is a `std::optional<bool>`. The ternary condition evaluates the
*optional* (i.e. `has_value()`), not the contained boolean — and inside the
`has_value()` guard it is always true. Consequently the ASN.1 `repetition` field of
`NZP-CSI-RS-ResourceSet` (TS 38.331) is always encoded as `on`, even when the internal
configuration requests `off`.

The scheduler-side builder sets the intent explicitly
(`lib/scheduler/config/csi_helper.cpp`):

```cpp
// Single beam: repetition on (omnidirectional). Multi-beam: repetition off (distinct spatial directions).
sets[0].is_repetition_on = (nof_beams == 1);
```

With the default single-beam configuration the encoded value happens to be correct, so the
bug is currently masked. For a multi-beam (beam-sweep) CSI configuration
(`nof_beams > 1`, supported by `csi_helper` and reachable through the DU cell config API),
the UE is incorrectly told that all NZP-CSI-RS resources of the set are transmitted with the
same downlink spatial-domain filter — the opposite of what a beam sweep requires — which
corrupts L1-RSRP-based beam measurement/selection at the UE.

## Affected spec references

- TS 38.331, `NZP-CSI-RS-ResourceSet` → `repetition` field description: "Indicates whether
  repetition is on/off. [...] the UE may not assume that the NZP-CSI-RS resources within the
  resource set are transmitted with the same downlink spatial domain transmission filter"
  (off case). The encoded value must reflect the configured one.

## Proposed fix

```cpp
out.repeat = *cfg.is_repetition_on ? nzp_csi_rs_res_set_s::repeat_opts::on
                                   : nzp_csi_rs_res_set_s::repeat_opts::off;
```

(One-character-class fix; same pattern as the neighbouring optional-valued fields.)

Follow-up worth tracking separately (not part of this fix): per the same TS 38.331 field
description, `repetition` "can only be configured for CSI-RS resource sets which are
associated with CSI-ReportConfig with report of L1 RSRP or 'no report'"; resource set 0 is
associated with a cri-RI-PMI-CQI report in the default configuration, so always *including*
the field is itself questionable — needs confirmation before changing presence logic.

## Backward compatibility

Default single-beam configurations encode `on` before and after the fix (no OTA change).
Only multi-beam configurations change, from an incorrect `on` to the intended `off`.

## Validation

- Unit test in tests/unittests/du_manager/ (converter tests): build a
  `nzp_csi_rs_resource_set` with `is_repetition_on = false` and assert the packed ASN.1
  field equals `off`; with `true` → `on`; with `nullopt` → absent.
- Existing du_manager converter suites remain green.
