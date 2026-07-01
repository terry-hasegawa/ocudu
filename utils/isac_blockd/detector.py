# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI

"""ISAC sensing PoC - Block D detection core (design doc v0.3, chapter 5).

Amplitude-only motion detection from per-slot CSI snapshots:
  metric_b = mean_sc | |H_t| - |H_{t-1}| |   per Rx branch, then combined across branches.

Handles the per-slot realities from the data contract:
  1. PRB allocation can change between snapshots -> diff only the overlapping subcarriers,
     aligned on absolute CRB subcarrier index; non-contiguous allocations are excluded
     (is_contiguous gate) because their packed body has no contiguous absolute axis.
  2. has_metrics gating -> epre/rsrp/snr are only used when valid.
  3. seq gaps (drops) and large dt -> the metric for that step is invalidated (held).
  4. UE identity: snapshots are locked to one RNTI (configured or first seen); other UEs'
     snapshots are ignored so their channels are never diffed against the target's.
  5. Epoch resets (gNB/Block A restart): a backwards ts_rel_ns or seq regression re-arms the
     detector (fresh baseline + recalibration) instead of freezing it.

Detection runs at the raw snapshot rate via process(); the display payload is built lazily by
render_frame() so the (much slower) broadcast loop pays the resampling cost, not the receiver.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field

import numpy as np

from wire import CsiSnapshot

EPS = 1e-9
SEQ_MOD = 1 << 32


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
    target_rnti: int | None = None   # lock to this UE; None = lock to the first RNTI seen


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
        self._locked_rnti: int | None = self.cfg.target_rnti
        self._ignored = 0
        self._detections = 0
        self._rate = 0.0
        self._latest_snap: CsiSnapshot | None = None
        self._latest_status: dict | None = None
        self._rendered_seq: int | None = None
        self._reset_epoch_state()

    def _reset_epoch_state(self) -> None:
        self._prev: _Prev | None = None
        self._baseline: np.ndarray | None = None  # per-branch EMA of mean|H|
        self._calib: list[float] = []
        self._calib_t0_ns: int | None = None
        self._threshold: float | None = None
        self._hold = 0
        self._state = False

    def rearm(self) -> None:
        """Re-arms baseline + calibration (e.g. after a Block A restart). Keeps counters."""
        self._reset_epoch_state()

    # -- helpers ------------------------------------------------------------------------------

    def _divisor(self, snap: CsiSnapshot) -> np.ndarray:
        """Per-branch normalization divisor (makes branches comparable before combining)."""
        n = snap.nof_rx_ports
        if self.cfg.normalize == "rsrp" and snap.has_metrics:
            return np.sqrt(np.maximum(snap.rsrp[:n], EPS))
        if self.cfg.normalize == "epre" and snap.has_metrics:
            return np.sqrt(np.maximum(snap.epre[:n], EPS))
        return np.maximum(self._baseline[:n], EPS)

    def _resample(self, vec: np.ndarray) -> np.ndarray:
        bins = self.cfg.display_bins
        if vec.size == bins:
            return vec
        xp = np.linspace(0.0, 1.0, vec.size)
        x = np.linspace(0.0, 1.0, bins)
        return np.interp(x, xp, vec).astype(np.float32)

    def _display(self, snap: CsiSnapshot) -> tuple[list[list[float]], list[float]]:
        """Per-branch + composite |H| resampled to display_bins and scaled to ~[0,1] for color."""
        branches: list[list[float]] = []
        acc = np.zeros(self.cfg.display_bins, dtype=np.float32)
        for b in range(snap.nof_rx_ports):
            scale = max(float(self._baseline[b]) * 2.0, EPS)
            disp = np.clip(self._resample(snap.hmag[b]) / scale, 0.0, 1.0)
            branches.append(np.round(disp, 4).tolist())
            acc += disp
        comp = acc / snap.nof_rx_ports
        return branches, np.round(comp, 4).tolist()

    def _is_epoch_reset(self, snap: CsiSnapshot, prev: _Prev) -> bool:
        """Detects a Block A restart: sender clock went backwards or seq regressed."""
        if snap.ts_rel_ns < prev.ts_ns:
            return True
        delta = (snap.seq - prev.seq) % SEQ_MOD
        return delta == 0 or delta > (SEQ_MOD >> 1)

    # -- main entry (runs at raw snapshot rate; keep it lean) ----------------------------------

    def process(self, snap: CsiSnapshot) -> dict | None:
        """Feeds one snapshot; returns the light status dict, or None if the UE was filtered."""
        # UE lock (#4): never mix another UE's channel into the diff chain.
        if snap.rnti is not None:
            if self._locked_rnti is None:
                self._locked_rnti = snap.rnti
            elif snap.rnti != self._locked_rnti:
                self._ignored += 1
                return None

        prev = self._prev

        # Epoch reset (#5): re-arm instead of freezing on a restarted sender.
        if prev is not None and self._is_epoch_reset(snap, prev):
            self.rearm()
            prev = None

        # dt from the snapshot relative timestamp.
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

        # ---- validity gates: seq gap / dt (#3), shape change, contiguity (#1) ----------------
        valid = prev is not None
        if prev is not None:
            drops = ((snap.seq - prev.seq) % SEQ_MOD) - 1
            if drops > self.cfg.max_seq_gap or dt > self.cfg.max_dt_s:
                valid = False
            if prev.hmag.shape[0] != snap.hmag.shape[0]:
                valid = False
        if not snap.is_contiguous:
            # Packed non-contiguous body has no contiguous absolute subcarrier axis: the diff
            # would compare physically different subcarriers. Skip and break the prev chain.
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

        # ---- detection with hold/debounce ----------------------------------------------------
        if self._threshold is not None:
            if combined is not None and combined > self._threshold:
                self._hold = self.cfg.hold_frames
            elif self._hold > 0:
                # Decay on every frame (including invalid steps) so the banner cannot latch
                # "detected" through a long drop gap.
                self._hold -= 1
            active = self._hold > 0
            if active and not self._state:
                self._detections += 1
            self._state = active

        snr_db = (
            [round(10.0 * math.log10(max(float(v), EPS)), 1) for v in snap.snr[:snap.nof_rx_ports]]
            if snap.has_metrics else None
        )

        # Store prev only for snapshots whose axis is trustworthy for the next diff.
        if snap.is_contiguous:
            self._prev = _Prev(snap.seq, snap.ts_rel_ns, snap.abs_sc_start, snap.nof_re, snap.hmag)
        else:
            self._prev = None

        status = {
            "type": "frame",
            "seq": snap.seq,
            "slot": snap.slot_index,
            "snr": snr_db,
            "has_metrics": snap.has_metrics,
            "metric": (round(combined, 5) if combined is not None else None),
            "threshold": (round(self._threshold, 5) if self._threshold is not None else None),
            "detected": self._state,
            "calibrating": calibrating,
            "valid": valid,
            "rate": round(self._rate, 1),
            "detections": self._detections,
            "ignored": self._ignored,
            "rnti": self._locked_rnti,
            "combine": self.cfg.combine,
            "nof_rx": snap.nof_rx_ports,
            "rank": snap.rank,
            "scs_khz": snap.scs_khz,
            "prb_start": snap.prb_start,
            "prb_count": snap.prb_count,
            "nof_re": snap.nof_re,
        }
        self._latest_snap = snap
        self._latest_status = status
        return status

    # -- display (runs at broadcast rate; pays the resampling cost lazily) ---------------------

    def render_frame(self) -> dict | None:
        """Builds the full display frame for the newest snapshot, or None if nothing new."""
        snap, status = self._latest_snap, self._latest_status
        if snap is None or status is None or self._baseline is None:
            return None
        if self._rendered_seq == snap.seq:
            return None  # nothing new since the last broadcast: let the client keep its frame
        branches, comp = self._display(snap)
        self._rendered_seq = snap.seq
        return {**status, "branches": branches, "composite": comp}
