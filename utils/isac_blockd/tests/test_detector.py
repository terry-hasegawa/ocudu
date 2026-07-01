# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI

"""Detector tests: calibration, detection, and the per-slot handlings
(PRB alignment, non-contiguity, seq gaps, epoch resets, RNTI lock, has_metrics gating)."""

import os
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from detector import Detector, DetectorConfig  # noqa: E402
from wire import SC_PER_PRB, CsiSnapshot  # noqa: E402

DT = 5_000_000  # 5 ms per snapshot -> 200/s

# Channel profile over ABSOLUTE subcarriers so PRB-shifted frames stay physically consistent.
ABS = 600
PROF = (0.4 + 0.1 * np.sin(np.arange(ABS) * 0.05)).astype(np.float32)
RNG = np.random.default_rng(1)


def frame(seq, ts, prb_start=10, nprb=8, nof_rx=4, motion=None, contiguous=True,
          has_metrics=True, rnti=0x4601):
    nre = nprb * SC_PER_PRB
    a = prb_start * SC_PER_PRB
    p = np.tile(PROF[a:a + nre], (nof_rx, 1)) + (RNG.random((nof_rx, nre)) - 0.5) * 0.01
    if motion is not None:
        center, amp = motion
        p += (amp * np.exp(-((np.arange(a, a + nre) - center) / 15.0) ** 2))[None, :]
    return CsiSnapshot(
        seq=seq, sfn=0, slot_index=seq % 20, system_slot=seq, scs_khz=30, numerology=1,
        rank=1, nof_rx_ports=nof_rx, dmrs_symbol=2, prb_start=prb_start, prb_count=nprb,
        nof_re=nre, is_contiguous=contiguous, has_metrics=has_metrics, ts_rel_ns=ts,
        rnti=rnti, epre=np.ones(nof_rx, np.float32), rsrp=np.full(nof_rx, 0.1, np.float32),
        snr=np.full(nof_rx, 100.0, np.float32), hmag=p.astype(np.float32))


def run_quiet(det, n, seq0=0, ts0=0, **kw):
    seq, ts, r = seq0, ts0, None
    for _ in range(n):
        r = det.process(frame(seq, ts, **kw))
        seq += 1
        ts += DT
    return seq, ts, r


def test_calibrate_then_detect():
    det = Detector(DetectorConfig(calib_seconds=1.0, calib_k=4.0, hold_frames=5))
    seq, ts, r = run_quiet(det, 400)
    assert r["threshold"] is not None and not r["calibrating"]
    for _ in range(20):  # quiet stays clear
        r = det.process(frame(seq, ts))
        assert not r["detected"]
        seq += 1
        ts += DT
    fired = False
    for _ in range(10):  # motion fires
        r = det.process(frame(seq, ts, motion=(10 * 12 + 48, 0.25)))
        fired |= r["detected"]
        seq += 1
        ts += DT
    assert fired and r["detections"] >= 1


def test_prb_shift_no_false_detection():
    det = Detector(DetectorConfig(calib_seconds=0.5, calib_k=4.0))
    seq, ts, _ = run_quiet(det, 200, prb_start=10)
    for _ in range(10):  # +2 PRB shift, same physical channel
        r = det.process(frame(seq, ts, prb_start=12))
        assert not r["detected"]
        seq += 1
        ts += DT


def test_non_contiguous_gated():
    det = Detector(DetectorConfig(calib_seconds=0.0))
    seq, ts, _ = run_quiet(det, 10)
    r = det.process(frame(seq, ts, contiguous=False, motion=(170, 0.5)))
    assert r["valid"] is False and r["metric"] is None
    # And the non-contiguous frame must not poison the next diff either.
    r = det.process(frame(seq + 1, ts + DT))
    assert r["metric"] is None  # prev chain was broken


def test_seq_gap_invalidates():
    det = Detector(DetectorConfig(calib_seconds=0.0))
    det.process(frame(0, 0))
    r = det.process(frame(50, DT, motion=(170, 0.3)))  # gap of 49 > max_seq_gap
    assert r["metric"] is None and r["valid"] is False


def test_epoch_reset_rearms():
    det = Detector(DetectorConfig(calib_seconds=0.5, calib_k=4.0))
    seq, ts, r = run_quiet(det, 200)
    assert r["threshold"] is not None
    # Sender restarts: ts and seq drop back near zero -> detector must re-arm, not freeze.
    r = det.process(frame(0, 0))
    assert r["calibrating"] and r["threshold"] is None
    # And it must be able to complete the new calibration on the new epoch's clock.
    _, _, r = run_quiet(det, 200, seq0=1, ts0=DT)
    assert r["threshold"] is not None


def test_rnti_lock_filters_other_ues():
    det = Detector(DetectorConfig(calib_seconds=0.0))
    assert det.process(frame(0, 0, rnti=0x4601)) is not None       # locks 0x4601
    assert det.process(frame(1, DT, rnti=0x9999)) is None          # other UE ignored
    r = det.process(frame(2, 2 * DT, rnti=0x4601))
    assert r is not None and r["ignored"] == 1 and r["rnti"] == 0x4601


def test_has_metrics_gating():
    det = Detector(DetectorConfig(calib_seconds=0.0, combine="snr", normalize="rsrp"))
    det.process(frame(0, 0, has_metrics=False))
    r = det.process(frame(1, DT, has_metrics=False))
    assert r["metric"] is not None  # fell back safely, no crash


def test_nof_rx_change_does_not_crash():
    det = Detector(DetectorConfig(calib_seconds=0.0))
    det.process(frame(0, 0, nof_rx=4))
    r = det.process(frame(1, DT, nof_rx=2))  # shape change -> invalid step, no exception
    assert r["valid"] is False


def test_render_frame_lazy():
    det = Detector(DetectorConfig(calib_seconds=0.0))
    assert det.render_frame() is None
    det.process(frame(0, 0))
    f = det.render_frame()
    assert f is not None and len(f["branches"]) == 4 and len(f["composite"]) == 64
    assert det.render_frame() is None  # same seq -> nothing new to broadcast
    det.process(frame(1, DT))
    assert det.render_frame() is not None
