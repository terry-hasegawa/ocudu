# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI

"""ISAC sensing PoC - Phase 2 zone classifier (AI calibration).

Nearest-centroid fingerprinting over per-branch |H| spectral profiles: during calibration a
person stands in each zone (plus one "empty" capture) and the mean feature vector per label is
stored as that zone's fingerprint. At inference time the live feature is compared against all
fingerprints and converted to per-zone probabilities (softmax over negative distances), which
the UI renders as a room heatmap.

This is supervised machine learning in its smallest useful form (a nearest-centroid / 1-NN
classifier trained on labelled captures). The classifier is swappable: load_model() replaces the
centroid backend with a scikit-learn model trained offline by train_zones.py on recorder.py
captures (D2-b) — same features, same UI, higher robustness.

Features are amplitude-only, per-branch mean-normalized (removes rx-gain and pathloss scale,
keeps the spectral shape that the person's position imprints on the multipath channel) and
EMA-smoothed over ~0.5 s. Fingerprints are environment-specific: recalibrate when the room,
furniture, or UE position changes.
"""

from __future__ import annotations

import json
import math
import time
from dataclasses import dataclass, field

import numpy as np

from wire import CsiSnapshot

EPS = 1e-9
EMPTY_LABEL = "empty"


def profile_feature(mat: np.ndarray, bins: int) -> np.ndarray:
    """Shared feature: per-branch |H| profile resampled to `bins`, mean-normalized, concatenated.

    Used identically by the live classifier (on CsiSnapshot.hmag) and by train_zones.py (on
    recorded captures) so offline-trained models see the same distribution.
    """
    x = np.linspace(0.0, 1.0, bins)
    parts = []
    for b in range(mat.shape[0]):
        vec = mat[b]
        prof = np.interp(x, np.linspace(0.0, 1.0, vec.size), vec)
        parts.append(prof / max(float(prof.mean()), EPS))
    return np.concatenate(parts).astype(np.float32)


@dataclass
class ZoneConfig:
    labels: list[str] = field(default_factory=lambda: ["A", "B", "C", "D"])
    grid: tuple[int, int] = (2, 2)     # rows x cols for the UI room panel
    feature_bins: int = 32             # per-branch spectral resolution
    feature_tc_s: float = 0.5          # EMA time constant of the live feature
    prob_tc_s: float = 0.7             # EMA smoothing of the displayed probabilities
    default_capture_s: float = 15.0    # default per-label calibration capture length


class ZoneClassifier:
    """Nearest-centroid zone classifier with interactive calibration."""

    def __init__(self, cfg: ZoneConfig) -> None:
        self.cfg = cfg
        self.centroids: dict[str, np.ndarray] = {}   # label -> feature centroid
        self.spread: float | None = None             # distance scale (tau) for softmax
        self._feat: np.ndarray | None = None         # EMA'd live feature
        self._feat_ts: int | None = None
        self._probs: np.ndarray | None = None        # EMA'd display probabilities
        self._collect_label: str | None = None
        self._collect_until: float = 0.0
        self._collect_buf: list[np.ndarray] = []
        self.model = None                            # D2-b: sklearn model (predict_proba)
        self._model_classes: list[str] = []

    # -- features -------------------------------------------------------------------------------

    def _feature(self, snap: CsiSnapshot) -> np.ndarray:
        """Per-branch mean-normalized spectral profile, concatenated across branches."""
        return profile_feature(snap.hmag, self.cfg.feature_bins)

    def feed(self, snap: CsiSnapshot) -> None:
        """Feeds one accepted snapshot (call after the detector accepted it)."""
        f = self._feature(snap)
        if self._feat is None or self._feat.shape != f.shape:
            # First frame or antenna-count change: restart the EMA (and invalidate fingerprints
            # on a dimensionality change, since stored centroids no longer compare).
            if self._feat is not None and (self.centroids or self.model is not None):
                self.clear()
                self.model = None
                self._model_classes = []
            self._feat = f
        else:
            dt = (snap.ts_rel_ns - self._feat_ts) / 1e9 if self._feat_ts is not None else 0.0
            a = 1.0 - math.exp(-dt / self.cfg.feature_tc_s) if dt > 0 else 1.0
            self._feat += a * (f - self._feat)
        self._feat_ts = snap.ts_rel_ns

        if self._collect_label is not None:
            self._collect_buf.append(f.copy())
            if time.monotonic() >= self._collect_until:
                self._finish_collect()

    # -- calibration ----------------------------------------------------------------------------

    def start_collect(self, label: str, seconds: float | None = None) -> bool:
        """Starts capturing the fingerprint for one label ('empty' or a zone label)."""
        if label != EMPTY_LABEL and label not in self.cfg.labels:
            return False
        self._collect_label = label
        self._collect_until = time.monotonic() + (seconds if seconds is not None else self.cfg.default_capture_s)
        self._collect_buf = []
        return True

    def _finish_collect(self) -> None:
        label, buf = self._collect_label, self._collect_buf
        self._collect_label, self._collect_buf = None, []
        if label is None or len(buf) < 5:
            return
        arr = np.stack(buf)
        self.centroids[label] = arr.mean(axis=0)
        # Distance scale: median intra-class deviation across all captured labels so far.
        dev = np.linalg.norm(arr - self.centroids[label], axis=1)
        med = float(np.median(dev))
        self.spread = med if self.spread is None else float(np.median([self.spread, med]))
        self.spread = max(self.spread, EPS)

    def clear(self) -> None:
        self.centroids = {}
        self.spread = None
        self._probs = None
        self._collect_label = None
        self._collect_buf = []

    # -- persistence ----------------------------------------------------------------------------

    def save(self, path: str) -> bool:
        if not self.ready:
            return False
        data = {
            "labels": self.cfg.labels,
            "grid": list(self.cfg.grid),
            "feature_bins": self.cfg.feature_bins,
            "spread": self.spread,
            "centroids": {k: v.tolist() for k, v in self.centroids.items()},
        }
        with open(path, "w", encoding="utf-8") as f:
            json.dump(data, f)
        return True

    def load(self, path: str) -> bool:
        try:
            with open(path, encoding="utf-8") as f:
                data = json.load(f)
            if data.get("feature_bins") != self.cfg.feature_bins:
                return False
            self.centroids = {k: np.asarray(v, dtype=np.float32) for k, v in data["centroids"].items()}
            self.spread = float(data["spread"])
            self._probs = None
            return True
        except (OSError, KeyError, ValueError, TypeError):
            return False

    def load_model(self, path: str) -> bool:
        """Loads a D2-b model bundle (train_zones.py output); replaces the centroid backend."""
        try:
            import joblib

            bundle = joblib.load(path)
            if bundle.get("feature_bins") != self.cfg.feature_bins:
                return False
            classes = [str(c) for c in bundle["classes"]]
            allowed = {EMPTY_LABEL, *self.cfg.labels}
            if not set(classes) <= allowed or EMPTY_LABEL not in classes:
                return False
            self.model = bundle["model"]
            self._model_classes = classes
            self._probs = None
            return True
        except Exception:  # noqa: BLE001 - a bad model file must not kill the server
            return False

    # -- inference ------------------------------------------------------------------------------

    @property
    def ready(self) -> bool:
        """Ready with a trained model, or once 'empty' and at least one zone are calibrated."""
        if self.model is not None:
            return True
        return EMPTY_LABEL in self.centroids and any(l in self.centroids for l in self.cfg.labels)

    def state(self) -> dict:
        """Zone-panel state for the UI frame; call at broadcast rate."""
        collecting = self._collect_label
        progress = 0.0
        if collecting is not None:
            total = self._collect_until - time.monotonic()
            cap = self.cfg.default_capture_s
            progress = min(max(1.0 - total / cap, 0.0), 1.0)

        out = {
            "labels": self.cfg.labels,
            "grid": list(self.cfg.grid),
            "backend": "model" if self.model is not None else "centroid",
            "calibrated": (sorted(self._model_classes) if self.model is not None
                           else sorted(self.centroids.keys())),
            "state": f"collecting:{collecting}" if collecting else ("ready" if self.ready else "idle"),
            "progress": round(progress, 2),
            "probs": None,
            "presence": None,
        }

        if self.model is not None and self._feat is not None:
            labels = self._model_classes
            p = np.asarray(self.model.predict_proba(self._feat.reshape(1, -1))[0], dtype=np.float64)
            # Reorder so index 0 is 'empty' (presence logic below relies on it).
            order = [labels.index(EMPTY_LABEL)] + [i for i, l in enumerate(labels) if l != EMPTY_LABEL]
            labels = [labels[i] for i in order]
            p = p[order]
            p /= max(p.sum(), EPS)
        elif self.ready and self._feat is not None and self.spread is not None:
            labels = [EMPTY_LABEL] + [l for l in self.cfg.labels if l in self.centroids]
            d = np.array([np.linalg.norm(self._feat - self.centroids[l]) for l in labels])
            logits = -d / (2.0 * self.spread)
            logits -= logits.max()
            p = np.exp(logits)
            p /= p.sum()
        else:
            return out

        if self._probs is None or self._probs.shape != p.shape:
            self._probs = p
        else:
            # Display smoothing at broadcast cadence (~15 fps).
            a = 1.0 - math.exp(-(1.0 / 15.0) / self.cfg.prob_tc_s)
            self._probs = self._probs + a * (p - self._probs)

        sm = self._probs / max(float(self._probs.sum()), EPS)
        zone_probs = {l: 0.0 for l in self.cfg.labels}
        for l, v in zip(labels, sm):
            if l != EMPTY_LABEL:
                zone_probs[l] = float(v)
        out["probs"] = [round(zone_probs[l], 3) for l in self.cfg.labels]
        out["presence"] = bool(sm[0] < 0.5)  # empty prob below 50% -> someone is present
        return out
