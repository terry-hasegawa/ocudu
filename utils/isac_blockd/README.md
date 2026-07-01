# ISAC sensing PoC — Block D (receive · detect · visualize)

Python side of the ISAC PoC. Subscribes to the Block A PHY TAP (ZMQ PUB, per-PUSCH-PDU CSI
snapshots), runs amplitude-only motion detection (design doc v0.3 ch.5), and serves a live
waterfall + metric + detection banner in the browser (ch.6). Disposable prototype.

```
Block A (gNB / C++)  --ZMQ PUB-->  server.py (SUB + detect)  --WebSocket-->  index.html (browser)
                                        \--HTTP serves index.html
```

## Install

```bash
cd utils/isac_blockd
pip install -r requirements.txt        # numpy, pyzmq, websockets
```

## Run against a live gNB (Block A)

On the gNB, Block A binds a PUB socket (e.g. `OCUDU_ISAC_ZMQ_ENDPOINT=tcp://*:5599`, built with
`-DENABLE_ISAC_TAP=ON`). Then on the detector host:

```bash
# If on the same host and using the default endpoint, no args are needed:
python3 server.py
# Otherwise point it at the gNB PUB socket:
python3 server.py --zmq tcp://<gnb-host>:5599
```

Then open the printed URL, e.g. `http://localhost:8080/index.html?ws=8765`.

Calibration: keep the sensing field **clear** for the first few seconds (the banner shows
`calibrating`); the threshold is locked as `mean + k·std` of that window. Then walk through the
field — the banner flips to `motion detected`.

## Try it without a gNB (synthetic Block A)

```bash
# terminal 1 — synthetic publisher (binds the endpoint, injects motion bursts):
python3 fake_blocka.py --bind tcp://127.0.0.1:5599 --rate 120
# terminal 2 — detector + UI:
python3 server.py --zmq tcp://127.0.0.1:5599
# browser: http://localhost:8080/index.html?ws=8765
```

## Detection tuning (design doc ch.5)

`server.py` flags:

| flag | default | meaning |
|---|---|---|
| `--combine` | `mean` | branch combine: `mean` / `l2` / `max` / `snr` (SNR-weighted, uses header SNR) |
| `--normalize` | `baseline` | per-branch normalization: `baseline` (slow EMA) / `rsrp` / `epre` |
| `--calib-seconds` | `4.0` | unmanned calibration window T |
| `--k` | `4.0` | threshold = mean + k·std |
| `--hold` | `12` | detection debounce (snapshots) |
| `--fps` | `15` | display broadcast rate (detection runs at the raw snapshot rate) |
| `--rnti` | first seen | lock to one UE RNTI (e.g. `0x4601`); other UEs' snapshots are ignored |

> Tip for demo day: if sensitivity is too low, switch `--combine snr` or `--combine max`.

## Per-slot handling (built in)

- **PRB-allocation changes**: frame-to-frame diff is aligned on **absolute CRB subcarrier index**
  (`prb_start*12`); only the overlapping subcarriers are differenced, so an allocation move is not
  mistaken for motion.
- **`has_metrics` gating**: `epre/rsrp/snr` are only used (SNR-weighted combine, rsrp/epre
  normalization) when the header flag is set.
- **`seq` gaps / large Δt**: a drop (`seq` gap) beyond `max_seq_gap`, or a Δt over `max_dt_s`,
  invalidates that step's metric (held) instead of producing a false spike.
- **Non-contiguous allocations** (`is_contiguous=0`): the packed body has no contiguous absolute
  subcarrier axis, so the diff is skipped and the prev-chain is broken (no phantom motion).
- **UE lock (RNTI)**: snapshots are locked to one UE (`--rnti` or the first RNTI seen); other
  UEs' channels are never diffed against the target's.
- **Epoch resets**: if Block A / the gNB restarts (sender clock or `seq` goes backwards), the
  detector re-arms automatically — fresh baseline and a new calibration window — instead of
  freezing on the stale threshold.
- **Robust receive**: malformed/foreign messages and per-snapshot detector errors are counted
  and skipped; they never terminate the server.

## Files

- `wire.py` — wire-format parser (authoritative layout from `lib/isac/README.md`).
- `detector.py` — detection core (metric, combine/normalize, calibration, the 3 handlings).
- `server.py` — ZMQ SUB + detector + WebSocket broadcast + static HTTP.
- `web/index.html` — browser visualization (waterfall, thumbnails, metric, banner, status).
- `fake_blocka.py` — synthetic Block A publisher for testing without a gNB.
- `tests/` — pytest suite (`python3 -m pytest tests/`): wire round-trip/malformed cases and the
  detector behaviors listed above.
