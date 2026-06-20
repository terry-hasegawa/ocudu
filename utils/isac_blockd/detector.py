# SPDX-FileCopyrightText: 2026 OCUDU ISAC sensing PoC
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
"""ISAC sensing PoC - Block D detection core (design doc v0.3, chapter 5).

Amplitude-only motion detection from per-slot CSI snapshots:
  metric_b = mean_sc | |H_t| - |H_{t-1}| |   per Rx branch, then combined across branches.

Handles the per-slot realities from the data contract:
  1. PRB allocation can change between snapshots -> diff only the overlapping subcarriers,
     aligned on absolute CRB subcarrier index.
  2. has_metrics gating -> epre/rsrp/snr are only used when valid.
  3. seq gaps (drops) and large dt -> the metric for that step is invalidated (held).

Threshold is calibrated over the first T seconds (unmanned) as mean + k*std; the per-branch
baseline used for normalization is a slow EMA that tracks environment drift.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field

import numpy as np

from wire import CsiSnapshot, SC_PER_PRB

EPS = 1e-9


# ---- Combine strategies (swappable; design 5: mean default / L2 / max / SNR-weighted) -------

def _combine_mean(m: np.ndarray, snap: CsiSnapshot) -> float:
    return float(np.mean(m))


def _combine_l2(m: np.ndarray, snap: CsiSnapshot) -> float:
    return float(np.linalg.norm(m) / math.sqrt(len(m)))


def _combine_max(m: np.ndarray, snap: CsiSnapshot) -> float:
    return float(np.max(m))


def _combine_snr(m: np.ndarray, snap: CsiSnapshot) -> float:
    # header snr is linear; weight branches by SNR so weak branches count less.
    if not snap.has_metrics:
        return float(np.mean(m))
    w = np.clip(snap.snr[: len(m)], 0.0, None)
    s = w.sum()
    if s <= EPS:
        return float(np.mean(m))
    return float(np.dot(w / s, m))


COMBINERS = {"mean": _combine_mean, "l2": _combine_l2, "max": _combine_max, "snr": _combine_snr}


@dataclass
class DetectorConfig:
    calib_seconds: float = 4.0       # T: unmanned calibration window
    calib_k: float = 4.0             # threshold = mean + k*std
    baseline_tc_s: float = 30.0      # slow-EMA time constant for per-branch baseline
    hold_frames: int = 12            # detection debounce (consecutive holds)
    combine: str = "mean"            # mean | l2 | max | snr
    normalize: str = "baseline"      # baseline | rsrp | epre
    max_seq_gap: int = 8             # > this many dropped snapshots -> skip the diff
    max_dt_s: float = 1.0            # dt above this -> skip the diff
    display_bins: int = 64           # waterfall vertical resolution


@dataclass
class _Prev:
    seq: int
    ts_ns: int
    abs_start: int
    nof_re: int
    hmag: np.ndarray


@dataclass
class Detector:
    cfg: DetectorConfig = field(default_factory=DetectorConfig)

    def __post_init__(self) -> None:
        if self.cfg.combine not in COMBINERS:
            raise ValueError(f"unknown combine '{self.cfg.combine}'")
        self._prev: _Prev | None = None
        self._baseline: np.ndarray | None = None  # per-branch EMA of mean|H|
        self._calib: list[float] = []
        self._calib_t0_ns: int | None = None
        self._threshold: float | None = None
        self._hold = 0
        self._state = False
        self._detections = 0
        self._rate = 0.0
        self._frames = 0

    # -- helpers ------------------------------------------------------------------------------

    def _divisor(self, snap: CsiSnapshot) -> np.ndarray:
        """Per-branch normalization divisor (makes branches comparable before combining)."""
        n = snap.nof_rx_ports
        if self.cfg.normalize == "rsrp" and snap.has_metrics:
            return np.sqrt(np.maximum(snap.rsrp[:n], EPS))
        if self.cfg.normalize == "epre" and snap.has_metrics:
            return np.sqrt(np.maximum(snap.epre[:n], EPS))
        base = self._baseline if self._baseline is not None else np.ones(n, dtype=np.float32)
        return np.maximum(base[:n], EPS)

    def _resample(self, vec: np.ndarray) -> np.ndarray:
        bins = self.cfg.display_bins
        if vec.size == bins:
            return vec
        xp = np.linspace(0.0, 1.0, vec.size)
        x = np.linspace(0.0, 1.0, bins)
        return np.interp(x, xp, vec).astype(np.float32)

    def _display(self, snap: CsiSnapshot) -> tuple[list[list[float]], list[float]]:
        """Per-branch + composite |H| resampled to display_bins and scaled to ~[0,1] for color."""
        base = self._baseline if self._baseline is not None else snap.hmag.mean(axis=1)
        branches: list[list[float]] = []
        acc = np.zeros(self.cfg.display_bins, dtype=np.float32)
        for b in range(snap.nof_rx_ports):
            scale = max(float(base[b]) * 2.0, EPS)
            disp = np.clip(self._resample(snap.hmag[b]) / scale, 0.0, 1.0)
            branches.append([round(float(v), 4) for v in disp])
            acc += disp
        comp = acc / snap.nof_rx_ports
        return branches, [round(float(v), 4) for v in comp]

    # -- main entry ---------------------------------------------------------------------------

    def process(self, snap: CsiSnapshot) -> dict:
        self._frames += 1
        prev = self._prev

        # dt from the snapshot relative timestamp (fall back to nominal on first frame).
        dt = (snap.ts_rel_ns - prev.ts_ns) / 1e9 if prev else 0.0
        if dt > 0:
            inst = 1.0 / dt
            self._rate = inst if self._rate == 0.0 else self._rate + 0.1 * (inst - self._rate)

        # Per-branch baseline: slow EMA of mean|H| (tracks drift, never collapses fast motion).
        mean_mag = snap.hmag.mean(axis=1)
        if self._baseline is None or self._baseline.shape != mean_mag.shape:
            self._baseline = mean_mag.copy()
        else:
            a = 1.0 - math.exp(-dt / self.cfg.baseline_tc_s) if dt > 0 else 0.0
            self._baseline += a * (mean_mag - self._baseline)

        # ---- validity gates: seq gap (#3) and dt --------------------------------------------
        valid = prev is not None
        if prev is not None:
            drops = ((snap.seq - prev.seq) & 0xFFFFFFFF) - 1
            if drops < 0 or drops > self.cfg.max_seq_gap or (dt > self.cfg.max_dt_s):
                valid = False

        # ---- PRB alignment (#1): diff only overlapping absolute subcarriers ------------------
        combined: float | None = None
        if valid and prev is not None:
            lo = max(snap.abs_sc_start, prev.abs_start)
            hi = min(snap.abs_sc_start + snap.nof_re, prev.abs_start + prev.nof_re)
            if hi - lo <= 0:
                valid = False  # no overlap -> do not treat allocation move as motion
            else:
                cur = snap.hmag[:, lo - snap.abs_sc_start: hi - snap.abs_sc_start]
                pre = prev.hmag[:, lo - prev.abs_start: hi - prev.abs_start]
                metric_b = np.abs(cur - pre).mean(axis=1)            # per branch
                metric_b = metric_b / self._divisor(snap)            # branch normalization (#2 gated)
                combined = COMBINERS[self.cfg.combine](metric_b, snap)

        # ---- calibration (time-based, T seconds) --------------------------------------------
        calibrating = self._threshold is None
        if calibrating:
            if self._calib_t0_ns is None:
                self._calib_t0_ns = snap.ts_rel_ns
            if combined is not None:
                self._calib.append(combined)
            elapsed = (snap.ts_rel_ns - self._calib_t0_ns) / 1e9
            if elapsed >= self.cfg.calib_seconds and len(self._calib) >= 5:
                arr = np.asarray(self._calib, dtype=np.float64)
                self._threshold = float(arr.mean() + self.cfg.calib_k * arr.std())
                calibrating = False

        # ---- detection with hold/debounce (only advances on valid steps) --------------------
        if combined is not None and self._threshold is not None:
            if combined > self._threshold:
                self._hold = self.cfg.hold_frames
            elif self._hold > 0:
                self._hold -= 1
            active = self._hold > 0
            if active and not self._state:
                self._detections += 1
            self._state = active

        branches, comp = self._display(snap)
        snr_db = (
            [round(10.0 * math.log10(max(float(v), EPS)), 1) for v in snap.snr[:snap.nof_rx_ports]]
            if snap.has_metrics else None
        )

        self._prev = _Prev(snap.seq, snap.ts_rel_ns, snap.abs_sc_start, snap.nof_re, snap.hmag)

        return {
            "type": "frame",
            "seq": snap.seq,
            "slot": snap.slot_index,
            "branches": branches,
            "composite": comp,
            "snr": snr_db,
            "has_metrics": snap.has_metrics,
            "metric": (round(combined, 5) if combined is not None else None),
            "threshold": (round(self._threshold, 5) if self._threshold is not None else None),
            "detected": self._state,
            "calibrating": calibrating,
            "valid": valid,
            "rate": round(self._rate, 1),
            "detections": self._detections,
            "nof_rx": snap.nof_rx_ports,
            "rank": snap.rank,
            "scs_khz": snap.scs_khz,
            "prb_start": snap.prb_start,
            "prb_count": snap.prb_count,
            "nof_re": snap.nof_re,
        }
