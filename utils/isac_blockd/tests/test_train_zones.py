# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI

"""D2-b tests: recorder-format captures -> train_zones -> live model backend round trip."""

import os
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from test_detector import DT, frame  # noqa: E402
from train_zones import build_model, load_captures  # noqa: E402
from zones import ZoneClassifier, ZoneConfig  # noqa: E402

import joblib  # noqa: E402

RNG = np.random.default_rng(7)
REC_BINS = 64


def write_capture(path, label, motion, n=60):
    """Writes a recorder.py-format .npz using the synthetic snapshot factory."""
    hmag, meta = [], []
    x = np.linspace(0.0, 1.0, REC_BINS)
    for i in range(n):
        s = frame(i, i * DT, motion=motion)
        prof = np.stack([np.interp(x, np.linspace(0.0, 1.0, s.hmag[b].size), s.hmag[b])
                         for b in range(s.nof_rx_ports)])
        hmag.append(prof.astype(np.float32))
        meta.append((s.seq, s.ts_rel_ns, s.prb_start, s.prb_count, s.nof_re, s.slot_index, s.sfn))
    np.savez_compressed(path, label=label, hmag=np.stack(hmag),
                        meta=np.asarray(meta, dtype=np.int64), bins=REC_BINS, rnti=0x4601)


def make_dataset(tmp_path):
    write_capture(tmp_path / "s1_empty.npz", "empty", motion=None)
    write_capture(tmp_path / "s1_A.npz", "A", motion=(10 * 12 + 20, 0.4))
    write_capture(tmp_path / "s1_B.npz", "B", motion=(10 * 12 + 75, 0.4))
    return str(tmp_path / "s1_*.npz")


def test_train_and_live_inference(tmp_path):
    pattern = make_dataset(tmp_path)
    x, y = load_captures([pattern], bins=32, smooth_s=0.05)
    assert x.shape[1] == 4 * 32 and set(y) == {"empty", "A", "B"}

    model = build_model("rf")
    model.fit(x, y)
    bundle = str(tmp_path / "zones_model.joblib")
    joblib.dump({"model": model, "classes": list(model.classes_), "feature_bins": 32, "kind": "rf"}, bundle)

    zc = ZoneClassifier(ZoneConfig(labels=["A", "B"], grid=(1, 2), feature_tc_s=0.01))
    assert zc.load_model(bundle)
    assert zc.ready  # model backend is ready without on-site calibration

    # Stand in "A": model probabilities must favour A and report presence.
    seq, ts = 0, 0
    for _ in range(30):
        zc.feed(frame(seq, ts, motion=(10 * 12 + 20, 0.4)))
        seq += 1
        ts += DT
    st = zc.state()
    assert st["backend"] == "model" and st["state"] == "ready"
    pa, pb = st["probs"]
    assert pa > pb and st["presence"] is True

    # Empty room -> presence drops.
    for _ in range(60):
        zc.feed(frame(seq, ts))
        seq += 1
        ts += DT
    zc._probs = None  # bypass display smoothing for a deterministic check
    st = zc.state()
    assert st["presence"] is False


def test_model_validation(tmp_path):
    pattern = make_dataset(tmp_path)
    x, y = load_captures([pattern], bins=16, smooth_s=0.05)
    model = build_model("logreg")
    model.fit(x, y)
    bundle = str(tmp_path / "m16.joblib")
    joblib.dump({"model": model, "classes": list(model.classes_), "feature_bins": 16, "kind": "logreg"}, bundle)

    # bins mismatch -> rejected
    zc = ZoneClassifier(ZoneConfig(labels=["A", "B"], grid=(1, 2)))
    assert not zc.load_model(bundle)

    # label mismatch -> rejected (model has A/B, config only has C)
    zc2 = ZoneClassifier(ZoneConfig(labels=["C"], grid=(1, 1), feature_bins=16))
    assert not zc2.load_model(bundle)

    # garbage file -> rejected, no exception
    bad = tmp_path / "bad.joblib"
    bad.write_bytes(b"not a joblib")
    assert not zc.load_model(str(bad))
