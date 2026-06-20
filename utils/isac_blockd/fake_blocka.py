# SPDX-FileCopyrightText: 2026 OCUDU ISAC sensing PoC
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
"""Synthetic Block A publisher for testing Block D without a gNB.

Binds a ZMQ PUB socket and emits per-slot CSI snapshots in the exact Block A wire format
(lib/isac/README.md), with periodic "motion" bursts, occasional PRB-allocation shifts, and
occasional seq gaps (drops) so the Block D per-slot handling can be exercised end to end.
"""

from __future__ import annotations

import argparse
import math
import os
import random
import struct
import time

import numpy as np

from wire import HDR_FMT, MAGIC, SC_PER_PRB

NSYMB = 14


def pack(seq, sfn, slot, system_slot, scs_khz, numerology, rank, nof_rx, dmrs_symbol,
         prb_start, prb_count, ts_rel_ns, epre, rsrp, snr, hmag):
    nof_re = prb_count * SC_PER_PRB
    hdr = struct.pack(
        HDR_FMT, MAGIC, 1, 94, seq, sfn, slot, system_slot, scs_khz, numerology, rank,
        nof_rx, dmrs_symbol, prb_start, prb_count, nof_re, 1, 1, ts_rel_ns,
        *[float(x) for x in epre], *[float(x) for x in rsrp], *[float(x) for x in snr],
    )
    body = np.zeros((nof_rx, nof_re, 2), dtype="<f4")
    body[..., 0] = hmag  # real = |H| (amplitude only; receiver takes |.|)
    return hdr + body.tobytes()


def main() -> None:
    import zmq

    ap = argparse.ArgumentParser()
    ap.add_argument("--bind", default=os.environ.get("OCUDU_ISAC_ZMQ_ENDPOINT", "tcp://*:5599"))
    ap.add_argument("--rate", type=float, default=120.0, help="snapshots per second")
    ap.add_argument("--nof-rx", type=int, default=4)
    ap.add_argument("--prb", type=int, default=8, help="allocated PRBs (nof_re = prb*12)")
    ap.add_argument("--motion", action="store_true", help="force continuous motion")
    args = ap.parse_args()

    ctx = zmq.Context()
    pub = ctx.socket(zmq.PUB)
    pub.bind(args.bind)
    print(f"[fake-blocka] PUB bound on {args.bind}  rate={args.rate}/s nof_rx={args.nof_rx} prb={args.prb}")
    time.sleep(0.3)  # let subscribers connect

    nof_rx = args.nof_rx
    scs_khz, numerology = 30, 1
    nslots = 20  # 30 kHz
    start = time.monotonic_ns()
    seq = 0
    prb_start = 10
    period = 1.0 / args.rate

    ev, ev_next = 0, 120
    t = 0
    base = np.array([0.32 + 0.16 * math.sin(b * 1.7) for b in range(nof_rx)], dtype=np.float32)
    snr_base = np.array([20.0 + 2.5 * math.sin(b * 1.3) for b in range(nof_rx)], dtype=np.float32)

    try:
        while True:
            t += 1
            nof_re = args.prb * SC_PER_PRB

            # Motion event scheduling (gaussian bump sweeping across subcarriers).
            if ev <= 0 and (t > ev_next or args.motion):
                ev, ev_next = 30, t + random.randint(110, 260)
            envel = math.sin(math.pi * (1 - ev / 30)) if ev > 0 else 0.0

            sc = np.arange(nof_re)
            hmag = np.empty((nof_rx, nof_re), dtype=np.float32)
            for b in range(nof_rx):
                lvl = base[b] + 0.16 * math.sin(t * 0.01 + b)
                col = lvl + (np.random.rand(nof_re) - 0.5) * 0.025
                if ev > 0:
                    ctr = nof_re * 0.5 + (b - 1.5) * 7 + (1 - ev / 30) * nof_re * 0.2
                    g = np.exp(-((sc - ctr) / (nof_re * 0.18)) ** 2)
                    col = col + envel * (0.8 + 0.3 * b) * g * (0.32 * math.sin(t * 0.9) + 0.16)
                hmag[b] = np.clip(col, 0.0, None)
            if ev > 0:
                ev -= 1

            snr = np.power(10.0, (snr_base + 0.4 * np.sin(t * 0.05) - envel * 1.8) / 10.0)  # linear
            epre = np.full(nof_rx, 1.0, dtype=np.float32)
            rsrp = (base * base).astype(np.float32)

            # Occasionally move the PRB allocation (tests absolute-CRB alignment).
            if t % 200 == 0:
                prb_start = random.choice([6, 10, 14])

            ts = time.monotonic_ns() - start
            slot = (t % nslots)
            sfn = (t // nslots) % 1024
            system_slot = t

            msg = pack(seq, sfn, slot, system_slot, scs_khz, numerology, 1, nof_rx,
                       2, prb_start, args.prb, ts, epre, rsrp, snr, hmag)
            pub.send(msg)

            # Occasionally drop a few seq numbers (tests gap handling).
            seq += (1 + (random.randint(3, 9) if random.random() < 0.01 else 0))
            time.sleep(period)
    except KeyboardInterrupt:
        print("\n[fake-blocka] stopped")
    finally:
        pub.close()
        ctx.term()


if __name__ == "__main__":
    main()
