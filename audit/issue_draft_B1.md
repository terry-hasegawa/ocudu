# Issue draft B1 (type: bug)

**Title:** UE is always signaled `maxMIMO-Layers = 1`, contradicting the configured maximum DL rank

## Summary

`make_default_pdsch_serving_cell_config()` in
`lib/scheduler/config/serving_cell_config_factory.cpp:280` hardcodes
`cfg.max_mimo_layers = 1;` although the function already receives `pdsch_builder_params`, whose
`max_nof_layers` member carries the configured maximum number of DL layers
(documented in `include/ocudu/scheduler/config/bwp_builder_params.h:29` as "If not set, DL antenna
ports are used"). This is the only writer of `pdsch_serving_cell_config::max_mimo_layers` in the
tree, and the ASN.1 converter unconditionally encodes it
(`lib/du/du_high/du_manager/converters/asn1_rrc_config_helpers.cpp:2798-2800`), so every UE
receives `PDSCH-ServingCellConfig.maxMIMO-Layers = 1` regardless of `cell: nof_antennas_dl` /
`pdsch: max_rank`.

Same shape as #423 (`p0_nominal_without_grant = -76`): the factory has the configured value at
hand and ignores it.

## Motivation

The UE ends up with an internally inconsistent RRC configuration:

- The CSI codebook is generated from the same `max_nof_layers` and allows RI reports up to that
  rank (`lib/scheduler/config/csi_helper.cpp:683` builds the RI restriction bitmap as
  `(1 << max_nof_layers) - 1`).
- `maxMIMO-Layers` tells the UE the PDSCH will never exceed 1 layer. Per TS 38.212,
  Section 5.4.2.1, this value is the `X` used for the LBRM TBS computation, while the gNB side
  rate-matches with the fixed maximum `tbs_lbrm_default`
  (`include/ocudu/ran/sch/sch_constants.h:26`, applied per-PDU in
  `lib/fapi_adaptor/mac/p7/pdu_translators/pdsch.cpp:37`).
- UEs that honor `maxMIMO-Layers` may cap DL rank at 1 (silent MIMO throughput loss), and their
  LBRM soft-buffer assumption diverges from the gNB's for large transport blocks.

## Proposed fix

One line: `cfg.max_mimo_layers = pdsch_params.max_nof_layers.value_or(1);`

The value inherits the existing validation chain: `pdsch: max_rank` is validated against
`nof_antennas_dl`, and `csi_helper` asserts `max_nof_layers <= nof_ports`.

## Backward compatibility

- Single-antenna cells (and cells with `max_rank: 1`): unchanged — `max_nof_layers` derives to 1.
- Multi-antenna cells: behavior changes intentionally — the UE is now signaled the same maximum
  rank the CSI configuration already allows. This aligns the UE LBRM `X` with the multi-layer
  assumption the gNB uses. Over-the-air validation with COTS UEs is recommended before merging
  (this changes an RRC field all connected UEs receive).

## Validation

- New positive test `serving_cell_config_factory_test.cpp` asserts `max_mimo_layers` follows
  `max_nof_layers` (auto-derived from antenna ports, and explicitly set).
- `sched_config_test` and `serving_cell_config_converter_test` green in a `-DBUILD_TESTING=ON`
  build (the existing converter test only asserts field presence, which is preserved).
