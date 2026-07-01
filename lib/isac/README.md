# ISAC sensing PoC — Block A (PHY TAP)

Disposable prototype. Read-only tap of the **UL DMRS channel estimate (H)** from the OCUDU
**CPU PHY**, published per PUSCH PDU (≈ one per UL-granted slot) over **ZeroMQ PUB** for the
Block D detector. Existing PHY classes are untouched; everything here is additive and gated.

This whole directory can be deleted to remove the feature completely (plus the small gated
hooks listed under *Integration*).

## How it works

- A decorator wraps `dmrs_pusch_estimator` (same external shape as the existing
  `phy_metrics_pusch_channel_estimator_decorator`). In `estimate()` it captures the config
  metadata and wraps the notifier with a **notifier shim** (the net-new piece the metrics
  decorator lacks).
- A second, pass-through decorator wraps `pusch_processor` and stamps the PDU **RNTI** in a
  thread-local slot (`isac_tap_context.h`); the estimator tap reads it there (estimate() runs
  synchronously inside process() on the same thread), so every snapshot carries a UE identity
  and Block D can lock onto one UE and ignore the rest.
- The shim intercepts `on_estimation_complete(est_results)`, reads H read-only via
  `est_results.get_symbol_ch_estimate(buf, dmrs_symbol, i_port, tx_layer=0)` for each Rx branch
  at the **first DM-RS symbol** of the allocation, then **always forwards** to the real notifier
  (estimation → demodulation → decode is unchanged).
- The sink checks the queue depth **before** serializing (a full queue costs one try_lock), then
  serializes into a **recycled buffer** (no steady-state allocations) and hands it to a dedicated
  sender thread. Sequence numbers are assigned under the queue lock at enqueue time, so the
  stream is monotonic and **a gap always means a dropped snapshot**. The PHY thread never does
  ZMQ I/O and never blocks; `publish()` cannot throw.
- The process-wide sink is created on first use and intentionally never destroyed (no ZMQ
  teardown during static destruction). A failed bind is not cached: the next PHY init retries.

Tap level is `dmrs_pusch_estimator` (NOT the per-port `port_channel_estimator`, which cannot see
the notifier or the assembled results).

## Build & enable

```bash
# Build with the tap compiled in (needs ZeroMQ; ENABLE_ZEROMQ is ON by default):
cmake -DENABLE_ISAC_TAP=ON ..

# Enable at runtime by pointing the sink at an endpoint (otherwise it is a complete no-op):
export OCUDU_ISAC_ZMQ_ENDPOINT="tcp://*:5599"
./gnb ...
```

- Build **without** `-DENABLE_ISAC_TAP=ON` (default): no ISAC code is compiled and
  `upper_phy_factories.cpp` is unchanged (wiring is behind `#ifdef ENABLE_ISAC_TAP`).
- Built **with** the option but `OCUDU_ISAC_ZMQ_ENDPOINT` unset: the factories are returned
  unwrapped — no socket, no thread, behavior identical to upstream.

## Recommended `gnb` config (rank-1 × 4R)

```yaml
cell_cfg:
  nof_antennas_ul: 4        # 4R receive
  pusch:
    max_rank: 1             # force rank-1 (else 4R yields up to 4 layers)
```

## Wire format v2 (per message = per PUSCH PDU)

Little-endian (x86/ARM64). One message = fixed 100-byte header + H body.

Header (`#pragma pack(1)`, see `isac_csi_payload.h`):

| field | type | notes |
|---|---|---|
| magic | u32 | `0x43415349` ("ISAC") |
| version | u16 | 2 |
| header_bytes | u16 | 100 |
| seq | u32 | monotonic in stream order; gaps ⇒ drops |
| sfn | u32 | 0..1023 |
| slot_index | u32 | slot within radio frame |
| system_slot | u32 | absolute slot count |
| scs_khz | u16 | 15/30/60/120/240 |
| numerology | u8 | 0..4 |
| rank | u8 | tx layers (=1) |
| nof_rx_ports | u8 | 1..4 |
| dmrs_symbol | u8 | OFDM symbol the snapshot was read at |
| prb_start | u16 | lowest allocated PRB (CRB) |
| prb_count | u16 | number of allocated PRBs (packed count) |
| nof_re | u16 | subcarriers per branch = prb_count*12 |
| is_contiguous | u8 | 1 if the PRB allocation is contiguous (see note) |
| has_metrics | u8 | 1 if epre/rsrp/snr valid |
| ts_rel_ns | u64 | steady-clock ns since sink start |
| epre[4] | f32×4 | per-port EPRE (linear) |
| rsrp[4] | f32×4 | per-port RSRP (linear) |
| snr[4] | f32×4 | per-port SNR (linear) |
| rnti | u16 | UE identity of the PUSCH PDU |
| rnti_valid | u8 | 1 if rnti is valid |
| reserved | u8×3 | zero; pads to 100 B so the body is 4-byte aligned |

Body: `nof_rx_ports * nof_re` complex coefficients, **branch-major**, each as two float32
`(real, imag)`. Body length = `nof_rx_ports * nof_re * 2 * 4` bytes.

Notes:
- H is the PHY's interpolated/smoothed frequency-domain estimate (not raw per-RE LS).
- **Contiguous allocations**: the subcarrier axis spans `[prb_start*12, (prb_start+prb_count)*12)`.
- **Non-contiguous allocations** (`is_contiguous=0`): the body packs only the allocated PRBs
  densely — there is no single contiguous absolute axis, so consumers MUST NOT diff such
  snapshots against others (Block D skips them).

## Block D decode (Python / numpy)

```python
import struct, numpy as np, zmq

HDR_FMT  = "<IHHIIIIHBBBBHHHBBQ12fHB3x"   # 100 bytes; 12f = epre[4],rsrp[4],snr[4]
HDR_SIZE = struct.calcsize(HDR_FMT)
assert HDR_SIZE == 100

ctx = zmq.Context()
sub = ctx.socket(zmq.SUB)
sub.connect("tcp://127.0.0.1:5599")
sub.setsockopt_string(zmq.SUBSCRIBE, "")

while True:
    buf = sub.recv()
    h = struct.unpack(HDR_FMT, buf[:HDR_SIZE])
    (magic, version, header_bytes, seq, sfn, slot_index, system_slot,
     scs_khz, numerology, rank, nof_rx_ports, dmrs_symbol,
     prb_start, prb_count, nof_re, is_contiguous, has_metrics, ts_rel_ns) = h[:18]
    epre = np.array(h[18:22]); rsrp = np.array(h[22:26]); snr = np.array(h[26:30])
    rnti, rnti_valid = h[30], h[31]
    assert magic == 0x43415349 and version == 2

    body = np.frombuffer(buf, dtype="<f4", count=nof_rx_ports*nof_re*2, offset=header_bytes)
    Hc = body.reshape(nof_rx_ports, nof_re, 2)
    Hc = Hc[..., 0] + 1j * Hc[..., 1]     # complex H, shape (nof_rx_ports, nof_re)
    mag = np.abs(Hc)                       # |H| per branch per subcarrier -> Block D metric
```

The reference consumer (robust parser, detector, UI) lives in `utils/isac_blockd/`.

## Integration (gated hooks outside this directory)

- `CMakeLists.txt` — `option(ENABLE_ISAC_TAP ... OFF)`
- `lib/CMakeLists.txt` — `if (ENABLE_ISAC_TAP) add_subdirectory(isac) endif()`
- `lib/phy/upper/CMakeLists.txt` — gated link/include/compile-definition for `ocudu_upper_phy`
- `lib/phy/upper/upper_phy_factories.cpp` — `#ifdef ENABLE_ISAC_TAP` include + one
  `maybe_wrap_pusch_estimator_factory` call (after the metrics decorator, before `pusch_config`
  use) + `maybe_wrap_pusch_processor_factory` on the data/UCI PUSCH processor factories (RNTI
  stamping), all inside `create_ul_processor_factory()`

## Known limitations (accepted for the PoC)

- One PUB stream per process: multi-cell deployments interleave cells on the same stream (RNTI
  disambiguates UEs; there is no cell id field yet).
- The snapshot is one representative DM-RS symbol per slot, tx_layer 0 only.
- If the estimation executor discards a queued task during teardown, the in-flight notifier shim
  leaks (bounded by in-flight PDUs; visible to ASAN in shutdown races).
