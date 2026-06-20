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
- The shim intercepts `on_estimation_complete(est_results)`, reads H read-only via
  `est_results.get_symbol_ch_estimate(buf, dmrs_symbol, i_port, tx_layer=0)` for each of the
  4 Rx branches at the **first DM-RS symbol** of the allocation, then **always forwards** to the
  real notifier (estimation → demodulation → decode is unchanged).
- The bytes are serialized on the PHY thread (a bounded copy) and handed to a **dedicated sender
  thread** via a bounded queue. The PHY thread never does ZMQ I/O and never blocks; if the queue
  is full/contended or there is no subscriber, the snapshot is **dropped** (PUB + `ZMQ_DONTWAIT`).

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
- Built **with** the option but `OCUDU_ISAC_ZMQ_ENDPOINT` unset: the factory is returned
  unwrapped — no socket, no thread, behavior identical to upstream.

## Recommended `gnb` config (rank-1 × 4R)

```yaml
cell_cfg:
  nof_antennas_ul: 4        # 4R receive
  pusch:
    max_rank: 1             # force rank-1 (else 4R yields up to 4 layers)
```

## Wire format (per message = per PUSCH PDU)

Little-endian (x86/ARM64). One message = fixed 94-byte header + H body.

Header (`#pragma pack(1)`, see `isac_csi_payload.h`):

| field | type | notes |
|---|---|---|
| magic | u32 | `0x43415349` ("ISAC") |
| version | u16 | 1 |
| header_bytes | u16 | 94 |
| seq | u32 | monotonic; gaps ⇒ drops |
| sfn | u32 | 0..1023 |
| slot_index | u32 | slot within radio frame |
| system_slot | u32 | absolute slot count |
| scs_khz | u16 | 15/30/60/120/240 |
| numerology | u8 | 0..4 |
| rank | u8 | tx layers (=1) |
| nof_rx_ports | u8 | =4 |
| dmrs_symbol | u8 | OFDM symbol the snapshot was read at |
| prb_start | u16 | lowest allocated PRB (CRB) |
| prb_count | u16 | number of allocated PRBs |
| nof_re | u16 | subcarriers per branch = prb_count*12 |
| is_contiguous | u8 | 1 if PRB allocation contiguous |
| has_metrics | u8 | 1 if epre/rsrp/snr valid |
| ts_rel_ns | u64 | steady-clock ns since sink start |
| epre[4] | f32×4 | per-port EPRE (linear) |
| rsrp[4] | f32×4 | per-port RSRP (linear) |
| snr[4] | f32×4 | per-port SNR (linear) |

Body: `nof_rx_ports * nof_re` complex coefficients, **branch-major**, each as two float32
`(real, imag)`. Body length = `nof_rx_ports * nof_re * 2 * 4` bytes.

Notes:
- H is the PHY's interpolated/smoothed frequency-domain estimate (not raw per-RE LS).
- The subcarrier axis spans the allocated RBs `[prb_start*12, (prb_start+prb_count)*12)`.

## Block D decode (Python / numpy)

```python
import struct, numpy as np, zmq

HDR_FMT  = "<IHHIIIIHBBBBHHHBBQ12f"   # 94 bytes; 12f = epre[4],rsrp[4],snr[4]
HDR_SIZE = struct.calcsize(HDR_FMT)
assert HDR_SIZE == 94

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
    assert magic == 0x43415349

    body = np.frombuffer(buf[header_bytes:], dtype="<f4")
    H = body.reshape(nof_rx_ports, nof_re, 2)
    Hc = H[..., 0] + 1j * H[..., 1]      # complex H, shape (nof_rx_ports, nof_re)
    mag = np.abs(Hc)                      # |H| per branch per subcarrier  -> Block D metric
```

## Integration (gated hooks outside this directory)

- `CMakeLists.txt` — `option(ENABLE_ISAC_TAP ... OFF)`
- `lib/CMakeLists.txt` — `if (ENABLE_ISAC_TAP) add_subdirectory(isac) endif()`
- `lib/phy/upper/CMakeLists.txt` — gated link/include/compile-definition for `ocudu_upper_phy`
- `lib/phy/upper/upper_phy_factories.cpp` — `#ifdef ENABLE_ISAC_TAP` include + one `maybe_wrap_*`
  call in `create_ul_processor_factory()` (after the metrics decorator, before `pusch_config` use)
