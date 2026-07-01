# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI

"""ISAC sensing PoC - D2-b offline trainer.

Trains a scikit-learn classifier on labelled recorder.py captures and saves a model bundle
that the live zone classifier loads with `server.py --zone-model <file>`. Same features as the
live path (zones.profile_feature), so training and inference distributions match.

Labels: name captures exactly 'empty' and the zone labels used at runtime (e.g. A,B,C,D):

    python3 recorder.py --label empty --seconds 20 --out s1_empty.npz
    python3 recorder.py --label A     --seconds 20 --out s1_A.npz
    ...
    python3 train_zones.py --data 's1_*.npz' --out zones_model.joblib
    python3 server.py --zones A,B,C,D --zone-model zones_model.joblib

Evaluation note: the printed cross-validation is stratified over snapshots, which is optimistic
when all captures come from one session (adjacent snapshots are highly correlated). For an
honest number, record a second session and hold it out with --eval 's2_*.npz'.
"""

from __future__ import annotations

import argparse
import glob
import sys

import joblib
import numpy as np

from zones import profile_feature


def load_captures(patterns: list[str], bins: int, smooth_s: float) -> tuple[np.ndarray, np.ndarray]:
    """Loads recorder .npz files -> (features, labels). Applies a short rolling mean over time
    to mimic the live feature EMA (~smooth_s seconds, using the recorded timestamps)."""
    feats, labels = [], []
    files = sorted(f for p in patterns for f in glob.glob(p))
    if not files:
        raise SystemExit(f"no capture files match {patterns}")
    for path in files:
        d = np.load(path, allow_pickle=False)
        label = str(d["label"])
        hmag = d["hmag"]            # (n, nof_rx, rec_bins)
        ts = d["meta"][:, 1]        # ns
        n = hmag.shape[0]
        if n < 5:
            print(f"[train] skipping {path}: only {n} snapshots")
            continue
        # Rolling mean window matched to the recorded rate (approximates the live EMA).
        dt = float(np.median(np.diff(ts))) / 1e9 if n > 1 else 0.01
        win = max(1, int(round(smooth_s / max(dt, 1e-4))))
        raw = np.stack([profile_feature(hmag[i], bins) for i in range(n)])
        if win > 1:
            kernel = np.ones(win, dtype=np.float64) / win
            sm = np.apply_along_axis(lambda c: np.convolve(c, kernel, mode="valid"), 0, raw)
        else:
            sm = raw
        feats.append(sm.astype(np.float32))
        labels.extend([label] * sm.shape[0])
        print(f"[train] {path}: label='{label}' snapshots={n} -> samples={sm.shape[0]}")
    return np.concatenate(feats), np.asarray(labels)


def build_model(kind: str):
    if kind == "rf":
        from sklearn.ensemble import RandomForestClassifier

        return RandomForestClassifier(n_estimators=200, class_weight="balanced", random_state=0)
    if kind == "logreg":
        from sklearn.linear_model import LogisticRegression
        from sklearn.pipeline import make_pipeline
        from sklearn.preprocessing import StandardScaler

        return make_pipeline(StandardScaler(), LogisticRegression(max_iter=2000, class_weight="balanced"))
    raise SystemExit(f"unknown model kind '{kind}'")


def main() -> None:
    ap = argparse.ArgumentParser(description="ISAC D2-b zone-model trainer")
    ap.add_argument("--data", nargs="+", required=True, help="training capture globs (recorder .npz)")
    ap.add_argument("--eval", nargs="+", default=None, help="held-out session globs (honest eval)")
    ap.add_argument("--out", default="zones_model.joblib")
    ap.add_argument("--model", default="rf", choices=["rf", "logreg"])
    ap.add_argument("--bins", type=int, default=32, help="feature bins (must match ZoneConfig.feature_bins)")
    ap.add_argument("--smooth", type=float, default=0.5, help="rolling-mean seconds (mimics live EMA)")
    args = ap.parse_args()

    x, y = load_captures(args.data, args.bins, args.smooth)
    classes = sorted(set(y))
    print(f"[train] {x.shape[0]} samples, {x.shape[1]} dims, classes={classes}")
    if "empty" not in classes or len(classes) < 2:
        raise SystemExit("need an 'empty' capture and at least one zone capture")

    # Quick sanity score (optimistic if single-session; see module docstring).
    from sklearn.model_selection import StratifiedKFold, cross_val_score

    model = build_model(args.model)
    folds = min(5, int(np.min(np.unique(y, return_counts=True)[1])))
    if folds >= 2:
        scores = cross_val_score(model, x, y, cv=StratifiedKFold(folds, shuffle=True, random_state=0))
        print(f"[train] {folds}-fold CV accuracy: {scores.mean():.3f} +/- {scores.std():.3f} "
              "(optimistic if all captures are one session)")

    model.fit(x, y)

    if args.eval:
        from sklearn.metrics import accuracy_score, confusion_matrix

        xe, ye = load_captures(args.eval, args.bins, args.smooth)
        pred = model.predict(xe)
        print(f"[train] held-out session accuracy: {accuracy_score(ye, pred):.3f}")
        print(confusion_matrix(ye, pred, labels=classes))

    joblib.dump({"model": model, "classes": list(model.classes_), "feature_bins": args.bins,
                 "kind": args.model}, args.out)
    print(f"[train] model bundle written to {args.out}")


if __name__ == "__main__":
    sys.exit(main())
