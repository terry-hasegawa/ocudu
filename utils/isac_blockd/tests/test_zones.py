# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI

"""Zone classifier tests: calibration capture, classification, persistence, invalidation."""

import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from test_detector import DT, frame  # noqa: E402  (reuses the synthetic snapshot factory)
from zones import ZoneClassifier, ZoneConfig  # noqa: E402


def make_zc(**kw):
    # Short feature EMA so the synthetic 5 ms/frame timeline converges within a few frames.
    kw.setdefault("feature_tc_s", 0.01)
    return ZoneClassifier(ZoneConfig(labels=["A", "B"], grid=(1, 2), **kw))


def capture(zc, label, seq0, ts0, motion, n=30):
    """Captures n frames for a label; motion shifts the spectral profile per 'zone'."""
    zc.start_collect(label, seconds=0.05)
    seq, ts = seq0, ts0
    for _ in range(n):
        zc.feed(frame(seq, ts, motion=motion))
        seq += 1
        ts += DT
    time.sleep(0.06)  # pass the capture deadline, then one more feed finishes the collection
    zc.feed(frame(seq, ts, motion=motion))
    return seq + 1, ts + DT


def test_calibrate_classify_roundtrip(tmp_path):
    zc = make_zc()
    seq, ts = 0, 0
    # Distinct spectral signatures: empty = flat, A = bump left, B = bump right.
    seq, ts = capture(zc, "empty", seq, ts, motion=None)
    assert not zc.ready
    seq, ts = capture(zc, "A", seq, ts, motion=(10 * 12 + 20, 0.4))
    assert zc.ready  # empty + one zone
    seq, ts = capture(zc, "B", seq, ts, motion=(10 * 12 + 75, 0.4))

    # Live inference: stand in "A".
    for _ in range(20):
        zc.feed(frame(seq, ts, motion=(10 * 12 + 20, 0.4)))
        seq += 1
        ts += DT
    st = zc.state()
    assert st["state"] == "ready" and st["probs"] is not None
    pa, pb = st["probs"]
    assert pa > pb and st["presence"] is True

    # Move to "empty" -> presence drops.
    for _ in range(40):
        zc.feed(frame(seq, ts))
        seq += 1
        ts += DT
    zc._probs = None  # skip display smoothing for a deterministic check
    st = zc.state()
    assert st["presence"] is False

    # Persistence round trip.
    path = str(tmp_path / "zones.json")
    assert zc.save(path)
    zc2 = make_zc()
    assert zc2.load(path) and zc2.ready
    assert set(zc2.centroids) == {"empty", "A", "B"}

    # Wrong feature_bins -> refuse to load.
    zc3 = make_zc(feature_bins=16)
    assert not zc3.load(path)


def test_dimension_change_invalidates():
    zc = make_zc()
    seq, ts = capture(zc, "empty", 0, 0, motion=None)
    seq, ts = capture(zc, "A", seq, ts, motion=(10 * 12 + 20, 0.4))
    assert zc.ready
    zc.feed(frame(seq, ts, nof_rx=2))  # antenna count change -> stale fingerprints cleared
    assert not zc.ready and zc.centroids == {}


def test_unknown_label_rejected():
    zc = make_zc()
    assert not zc.start_collect("Z")
    assert zc.start_collect("empty")
