# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI

"""ISAC sensing PoC - labelled CSI recorder (D2-a data foundation).

Subscribes to the Block A stream and records labelled snapshots to .npz for offline ML work
(the D2-b upgrade path: RandomForest/CNN trained on the same features the live classifier
uses). One file per capture; run it once per label following the calibration script, e.g.:

    python3 recorder.py --label empty --seconds 20 --out session1_empty.npz
    python3 recorder.py --label zoneA --seconds 20 --out session1_zoneA.npz

Only contiguous snapshots from the locked RNTI are recorded. |H| is stored both resampled to a
fixed number of bins (ML-ready matrix) and with its allocation metadata.
"""

from __future__ import annotations

import argparse
import os
import time

import numpy as np
import zmq

from wire import parse

def connect_endpoint(ep: str) -> str:
    return ep.replace("//*:", "//127.0.0.1:").replace("//0.0.0.0:", "//127.0.0.1:")


def main() -> None:
    env_ep = os.environ.get("OCUDU_ISAC_ZMQ_ENDPOINT", "tcp://127.0.0.1:5599")
    ap = argparse.ArgumentParser(description="ISAC labelled CSI recorder (D2-a)")
    ap.add_argument("--zmq", default=connect_endpoint(env_ep))
    ap.add_argument("--label", required=True, help="capture label (e.g. empty, zoneA, walk1)")
    ap.add_argument("--seconds", type=float, default=20.0)
    ap.add_argument("--out", default=None, help="output .npz (default: <label>_<ts>.npz)")
    ap.add_argument("--bins", type=int, default=64, help="resampled subcarrier bins")
    ap.add_argument("--rnti", type=lambda v: int(v, 0), default=None)
    args = ap.parse_args()
    out = args.out or f"{args.label}_{int(time.time())}.npz"

    ctx = zmq.Context()
    sub = ctx.socket(zmq.SUB)
    sub.connect(connect_endpoint(args.zmq))
    sub.setsockopt(zmq.SUBSCRIBE, b"")
    sub.RCVTIMEO = 2000

    x = np.linspace(0.0, 1.0, args.bins)
    feats, meta = [], []
    locked = args.rnti
    t_end = time.monotonic() + args.seconds
    print(f"[recorder] label='{args.label}' for {args.seconds}s from {args.zmq} -> {out}")

    while time.monotonic() < t_end:
        try:
            snap = parse(sub.recv())
        except zmq.error.Again:
            print("[recorder] ... waiting for data")
            continue
        if snap is None or not snap.is_contiguous:
            continue
        if snap.rnti is not None:
            if locked is None:
                locked = snap.rnti
                print(f"[recorder] locked rnti {locked:#x}")
            elif snap.rnti != locked:
                continue
        prof = np.stack([np.interp(x, np.linspace(0.0, 1.0, snap.hmag[b].size), snap.hmag[b])
                         for b in range(snap.nof_rx_ports)])
        feats.append(prof.astype(np.float32))
        meta.append((snap.seq, snap.ts_rel_ns, snap.prb_start, snap.prb_count, snap.nof_re,
                     snap.slot_index, snap.sfn))

    if not feats:
        print("[recorder] no snapshots captured; nothing written")
        return

    np.savez_compressed(
        out,
        label=args.label,
        hmag=np.stack(feats),                       # (n, nof_rx, bins)
        meta=np.asarray(meta, dtype=np.int64),      # (n, 7): seq, ts_ns, prb_start, prb_count, nof_re, slot, sfn
        bins=args.bins,
        rnti=(locked if locked is not None else -1),
    )
    print(f"[recorder] wrote {len(feats)} snapshots to {out}")

    sub.close()
    ctx.term()


if __name__ == "__main__":
    main()
