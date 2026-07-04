#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
"""OCUDU UL/DL link-quality diagnoser (offline, batch).

Reads already-captured OCUDU gNB outputs and classifies every observation
window into one of four buckets:

  ENV/RU   ... propagation / interference / RU calibration -> operator action, no code change
  CONFIG   ... a gNB configuration key should be tuned
  CODE     ... an OCUDU implementation issue is the likely cause -> code pointer given
  CAPACITY ... the link is at its theoretical limit (e.g. UL slot ratio) -> no action

Inputs (all optional, at least one required):
  --json FILE     JSON metrics captured from the remote_control WebSocket (:8001,
                  `metrics: enable_json`). One JSON object per line (JSONL), as
                  produced by e.g.:  websocat ws://<gnb>:8001 | tee metrics.jsonl
                  Scheduler UE metrics and OFH (`enable_ru_metrics`) messages are both understood.
  --log FILE      gNB log file (`log: filename`). Understands:
                    * PHY "PUSCH: ..." lines, including the extended measurement set emitted when
                      `expert_phy: pusch_diagnostics_enabled: true` (sinr_ce/sinr_eq/sinr_evm,
                      sinr_lyr, cond, nvar_p, llr_sat, llr_avg, rsrp, t_align, cfo, crc, iter).
                    * scheduler metrics log lines (`metrics: enable_log`), which carry ul_olla/dl_olla.
                    * OFH reception window statistics lines (rx_total/rx_early/rx_on_time/rx_late,
                      nof_missed_uplink_symbols).

Output: one line per observation window and per detected symptom with the numeric evidence,
the bucket, and the next action; followed by a global summary (bucket tally and the top-3
recommended actions).

DL caveat: the DL receiver is the UE (black box). DL findings are attributed only to what the
gNB controls (link adaptation, scheduler, RI/PMI/precoding selection) and are never claimed to
be UE-side receiver bugs.

The MCS <-> required-SNR model embedded here is the scheduler table
(lib/scheduler/support/mcs_calculator.cpp) plus a calibration offset measured with
tests/integrationtests/.../pxsch_bler_test on this code base (about +3 dB at 100 MHz / 273 PRB,
see docs/diagnostics/02_static_review.md). Tune with --table-offset if needed.

Self test: `ocudu_link_diag.py --selftest` synthesizes the field anchor case
(rank1, MCS27, reported SNR 31 dB, BLER 18-29%, MCS not decreasing) and checks that it is
classified as [CODE] (UL link adaptation unable to correct / receiver-side SINR gap).
"""

import argparse
import json
import math
import re
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field

# ---------------------------------------------------------------------------------------------
# Embedded model constants
# ---------------------------------------------------------------------------------------------

# Minimum required SNR per UL MCS (qam64 table), from lib/scheduler/support/mcs_calculator.cpp.
UL_SNR_MCS_TABLE = [
    -5.7998, -3.5500, -2.925, -2.5625, -1.0500, 0.98266, 1.6250, 2.5425, 3.4175, 4.3548,
    5.3695, 5.8250, 6.6375, 7.6375, 9.5875, 10.4000, 11.1540, 12.1070, 12.5250, 13.0625,
    13.5250, 13.9375, 14.1160, 14.5525, 14.9725, 15.3450, 15.9175, 16.0425, 16.591,
]

# DDDSUUDDDD: 2 full UL slots + 4 UL symbols in the special slot, per 10 slots.
DEFAULT_UL_SLOT_RATIO = 2.0 / 10.0

DOC = "docs/diagnostics/02_static_review.md"

BUCKETS = ("ENV/RU", "CONFIG", "CODE", "CAPACITY", "INFO")


def required_snr(mcs: int, table_offset_db: float) -> float:
    """Actual SNR needed to decode the given UL MCS on this code base."""
    mcs = max(0, min(int(round(mcs)), len(UL_SNR_MCS_TABLE) - 1))
    return UL_SNR_MCS_TABLE[mcs] + table_offset_db


def scheduler_mcs_for_snr(snr_db: float) -> int:
    """MCS the scheduler itself would pick for this SNR (raw table, no calibration offset)."""
    mcs = 0
    for i, thr in enumerate(UL_SNR_MCS_TABLE):
        if snr_db >= thr:
            mcs = i
    return mcs


# ---------------------------------------------------------------------------------------------
# Data model
# ---------------------------------------------------------------------------------------------

@dataclass
class WindowStats:
    """Aggregated observables of one observation window (default: one metrics period)."""

    label: str = ""
    # --- UL (scheduler metrics) ---
    ul_snr_db: float = math.nan          # reported PUSCH SNR (what link adaptation sees)
    ul_rsrp_db: float = math.nan
    ul_mcs: float = math.nan
    ul_ok: int = 0
    ul_nok: int = 0
    ul_brate_bps: float = math.nan
    ul_ri: float = math.nan
    ul_olla_db: float = math.nan
    bsr: int = 0
    ul_prb_ratio: float = math.nan       # PUSCH PRB usage ratio 0..1 if available
    # --- DL ---
    cqi: float = math.nan
    dl_ri: float = math.nan
    dl_mcs: float = math.nan
    dl_ok: int = 0
    dl_nok: int = 0
    dl_brate_bps: float = math.nan
    dl_bs: int = 0
    dl_olla_db: float = math.nan
    # --- PHY diagnostics (averaged over the PUSCH lines of the window) ---
    n_pusch: int = 0
    sinr_ce_db: float = math.nan
    sinr_eq_db: float = math.nan
    sinr_evm_db: float = math.nan
    evm: float = math.nan
    sinr_lyr_db: list = field(default_factory=list)
    cond_db: float = math.nan
    nvar_p_db: list = field(default_factory=list)
    rsrp_p_db: list = field(default_factory=list)
    llr_sat: float = math.nan
    llr_avg: float = math.nan
    t_align_us: float = math.nan
    cfo_hz: float = math.nan
    cfo_spread_hz: float = math.nan
    ldpc_iter_avg: float = math.nan
    ldpc_iter_max: float = math.nan
    crc_ko_ratio: float = math.nan
    # --- OFH ---
    ofh_early: int = 0
    ofh_on_time: int = 0
    ofh_late: int = 0
    ofh_missed_ul_symbols: int = 0
    ofh_missed_prach: int = 0

    def ul_bler(self):
        tot = self.ul_ok + self.ul_nok
        return (self.ul_nok / tot) if tot else math.nan

    def dl_bler(self):
        tot = self.dl_ok + self.dl_nok
        return (self.dl_nok / tot) if tot else math.nan


@dataclass
class Finding:
    window: str
    link: str        # "UL" | "DL" | "OFH"
    symptom: str
    evidence: str
    bucket: str
    action: str


# ---------------------------------------------------------------------------------------------
# Parsers
# ---------------------------------------------------------------------------------------------

def _mean(values):
    values = [v for v in values if v is not None and not (isinstance(v, float) and math.isnan(v))]
    return sum(values) / len(values) if values else math.nan


class JsonMetricsParser:
    """Parses JSONL captured from the remote_control WebSocket."""

    def __init__(self):
        self.windows = []

    def parse(self, path):
        with open(path, "r", errors="replace") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    obj = json.loads(line)
                except json.JSONDecodeError:
                    continue
                self._handle(obj)
        return self.windows

    def _handle(self, obj):
        ts = obj.get("timestamp")
        # Scheduler cell metrics: {"timestamp":..., "cells":[{"cell_metrics": {...}, "ue_list":[...]}]}
        for cell in obj.get("cells", []) or []:
            w = WindowStats(label=f"json@{ts}")
            cm = cell.get("cell_metrics", cell)
            prbs = cm.get("pusch_prbs_used_per_tdd_slot_idx")
            if isinstance(prbs, list) and prbs:
                # Ratio of used PRBs on UL slots (273 PRB grid).
                used = [p for p in prbs if p]
                w.ul_prb_ratio = _mean(used) / 273.0 if used else 0.0
            ues = cell.get("ue_list", []) or []
            if not ues:
                # Keep OFH/cell-only windows too, if they carry PRB info.
                if not math.isnan(w.ul_prb_ratio):
                    self.windows.append(w)
                continue
            # Aggregate over UEs (single-UE test rigs are the main use case).
            for container in ues:
                ue = container.get("ue_container", container)
                w.ul_snr_db = _mean([w.ul_snr_db, ue.get("pusch_snr_db")])
                w.ul_rsrp_db = _mean([w.ul_rsrp_db, ue.get("pusch_rsrp_db")])
                w.ul_mcs = _mean([w.ul_mcs, ue.get("ul_mcs")])
                w.ul_ok += int(ue.get("ul_nof_ok", 0))
                w.ul_nok += int(ue.get("ul_nof_nok", 0))
                w.ul_brate_bps = _mean([w.ul_brate_bps, ue.get("ul_brate")])
                w.ul_ri = _mean([w.ul_ri, ue.get("ul_ri")])
                w.bsr = max(w.bsr, int(ue.get("bsr", 0)))
                w.cqi = _mean([w.cqi, ue.get("cqi")])
                w.dl_ri = _mean([w.dl_ri, ue.get("dl_ri")])
                w.dl_mcs = _mean([w.dl_mcs, ue.get("dl_mcs")])
                w.dl_ok += int(ue.get("dl_nof_ok", 0))
                w.dl_nok += int(ue.get("dl_nof_nok", 0))
                w.dl_brate_bps = _mean([w.dl_brate_bps, ue.get("dl_brate")])
                w.dl_bs = max(w.dl_bs, int(ue.get("dl_bs", 0)))
            self.windows.append(w)
        # OFH metrics (enable_ru_metrics): search for the known counter block anywhere in the object.
        self._extract_ofh(obj, ts)

    def _extract_ofh(self, obj, ts):
        found = {}

        def walk(node):
            if isinstance(node, dict):
                if "received_packets" in node and isinstance(node["received_packets"], dict):
                    found.setdefault("rx", node["received_packets"])
                if "rx_window_stats" in node and isinstance(node["rx_window_stats"], dict):
                    found.setdefault("win", node["rx_window_stats"])
                for v in node.values():
                    walk(v)
            elif isinstance(node, list):
                for v in node:
                    walk(v)

        walk(obj)
        if not found:
            return
        w = WindowStats(label=f"json-ofh@{ts}")
        rx = found.get("rx", {})
        w.ofh_early = int(rx.get("early", 0))
        w.ofh_on_time = int(rx.get("on_time", 0))
        w.ofh_late = int(rx.get("late", 0))
        win = found.get("win", {})
        w.ofh_missed_ul_symbols = int(win.get("nof_missed_uplink_symbols", 0))
        w.ofh_missed_prach = int(win.get("nof_missed_prach_occasions", 0))
        self.windows.append(w)


# PUSCH log line fields (emitted by logging_pusch_processor_decorator + CSI formatter, with the
# extended set present when pusch_diagnostics_enabled is set).
RE_FLOAT = r"[+-]?\d+(?:\.\d+)?"
RE_PUSCH = re.compile(r"PUSCH: .*?rnti=0x[0-9a-fA-F]+", re.S)
RE_FIELDS = {
    "crc": re.compile(r"crc=(OK|KO)"),
    "iter": re.compile(r"iter=({0})".format(RE_FLOAT)),
    "sinr": re.compile(r"sinr=({0})dB".format(RE_FLOAT)),
    "sinr_ce": re.compile(r"sinr_ce=({0})dB".format(RE_FLOAT)),
    "sinr_eq": re.compile(r"sinr_eq=({0})dB".format(RE_FLOAT)),
    "sinr_evm": re.compile(r"sinr_evm=({0})dB".format(RE_FLOAT)),
    "evm": re.compile(r"(?<![a-zA-Z_])evm=({0})".format(RE_FLOAT)),
    "cond": re.compile(r"cond=({0})dB".format(RE_FLOAT)),
    "llr_sat": re.compile(r"llr_sat=({0})".format(RE_FLOAT)),
    "llr_avg": re.compile(r"llr_avg=({0})".format(RE_FLOAT)),
    "t_align": re.compile(r"t_align=({0})us".format(RE_FLOAT)),
    "cfo": re.compile(r"cfo=({0})Hz".format(RE_FLOAT)),
    "epre": re.compile(r"epre=({0})dB".format(RE_FLOAT)),
}
RE_VEC_FIELDS = {
    "sinr_lyr": re.compile(r"sinr_lyr=\[([^\]]*)\]dB"),
    "nvar_p": re.compile(r"nvar_p=\[([^\]]*)\]dB"),
    "rsrp": re.compile(r"rsrp=\[([^\]]*)\]dB"),
}
RE_MCS_LOG = re.compile(r"mcs=(\d+)")
RE_UL_OLLA = re.compile(r"ul_olla=({0})".format(RE_FLOAT))
RE_DL_OLLA = re.compile(r"dl_olla=({0})".format(RE_FLOAT))
RE_OFH_STATS = re.compile(
    r"rx_total=(\d+).*?rx_early=(\d+).*?rx_on_time=(\d+).*?rx_late=(\d+)", re.S)
RE_OFH_MISSED = re.compile(r"nof_missed_uplink_symbols=(\d+)")


def _parse_vec(text):
    out = []
    for tok in re.split(r"[,\s]+", text.strip()):
        if not tok:
            continue
        try:
            out.append(float(tok))
        except ValueError:
            pass
    return out


class GnbLogParser:
    """Parses the gNB log; groups PUSCH lines into windows of --log-window lines."""

    def __init__(self, window_size):
        self.window_size = max(1, window_size)

    def parse(self, path):
        pusch_rows = []
        olla_ul = []
        olla_dl = []
        ofh_rows = []
        with open(path, "r", errors="replace") as f:
            for line in f:
                if "PUSCH: " in line and "rnti=" in line:
                    row = {}
                    for key, rex in RE_FIELDS.items():
                        m = rex.search(line)
                        if m:
                            row[key] = m.group(1)
                    for key, rex in RE_VEC_FIELDS.items():
                        m = rex.search(line)
                        if m:
                            row[key] = _parse_vec(m.group(1))
                    m = RE_MCS_LOG.search(line)
                    if m:
                        row["mcs"] = int(m.group(1))
                    if row:
                        pusch_rows.append(row)
                    continue
                m = RE_UL_OLLA.search(line)
                if m:
                    olla_ul.append(float(m.group(1)))
                m = RE_DL_OLLA.search(line)
                if m:
                    olla_dl.append(float(m.group(1)))
                m = RE_OFH_STATS.search(line)
                if m:
                    ofh_rows.append(tuple(int(x) for x in m.groups()))
                m = RE_OFH_MISSED.search(line)
                if m:
                    ofh_rows.append((0, 0, 0, 0, int(m.group(1))))

        windows = []
        for i in range(0, len(pusch_rows), self.window_size):
            chunk = pusch_rows[i:i + self.window_size]
            w = WindowStats(label=f"log#{i // self.window_size}")
            w.n_pusch = len(chunk)
            crc_ko = sum(1 for r in chunk if r.get("crc") == "KO")
            w.crc_ko_ratio = crc_ko / len(chunk)
            w.ul_nok = crc_ko
            w.ul_ok = len(chunk) - crc_ko

            def favg(key):
                return _mean([float(r[key]) for r in chunk if key in r])

            w.sinr_ce_db = favg("sinr_ce")
            w.sinr_eq_db = favg("sinr_eq")
            w.sinr_evm_db = favg("sinr_evm")
            w.evm = favg("evm")
            w.cond_db = favg("cond")
            w.llr_sat = favg("llr_sat")
            w.llr_avg = favg("llr_avg")
            w.t_align_us = favg("t_align")
            w.ldpc_iter_avg = favg("iter")
            iters = [float(r["iter"]) for r in chunk if "iter" in r]
            w.ldpc_iter_max = max(iters) if iters else math.nan
            w.ul_snr_db = favg("sinr")
            cfos = [float(r["cfo"]) for r in chunk if "cfo" in r]
            w.cfo_hz = _mean(cfos)
            if len(cfos) > 1:
                w.cfo_spread_hz = max(cfos) - min(cfos)
            mcss = [r["mcs"] for r in chunk if "mcs" in r]
            w.ul_mcs = _mean(mcss)
            for key, attr in (("sinr_lyr", "sinr_lyr_db"), ("nvar_p", "nvar_p_db"), ("rsrp", "rsrp_p_db")):
                vecs = [r[key] for r in chunk if key in r and r[key]]
                if vecs:
                    n = max(len(v) for v in vecs)
                    avg = [
                        _mean([v[j] for v in vecs if len(v) > j and not math.isinf(v[j])])
                        for j in range(n)
                    ]
                    setattr(w, attr, avg)
            if olla_ul:
                w.ul_olla_db = olla_ul[-1]
            if olla_dl:
                w.dl_olla_db = olla_dl[-1]
            windows.append(w)

        for tup in ofh_rows:
            w = WindowStats(label="log-ofh")
            if len(tup) == 4:
                _, w.ofh_early, w.ofh_on_time, w.ofh_late = tup
            else:
                w.ofh_missed_ul_symbols = tup[4]
            windows.append(w)
        return windows


# ---------------------------------------------------------------------------------------------
# Rule engine
# ---------------------------------------------------------------------------------------------

class Diagnoser:
    def __init__(self, args):
        self.args = args

    # -- helpers --------------------------------------------------------------------------
    def implied_effective_sinr(self, mcs, bler):
        """Effective SINR band implied by the observed BLER at the given MCS (LDPC cliff model)."""
        if math.isnan(mcs) or math.isnan(bler):
            return None
        req = required_snr(mcs, self.args.table_offset)
        if bler >= 0.99:
            return ("<", req - 0.5)
        if bler <= 0.005:
            return (">", req)
        return ("~", req)  # sitting on the cliff

    # -- UL rules -------------------------------------------------------------------------
    def diagnose_ul(self, w):
        out = []
        a = self.args
        bler = w.ul_bler()
        snr = w.ul_snr_db if not math.isnan(w.ul_snr_db) else w.sinr_eq_db
        have_bler = not math.isnan(bler) and (w.ul_ok + w.ul_nok) >= a.min_samples

        if not have_bler or math.isnan(snr):
            return out

        req = required_snr(w.ul_mcs, a.table_offset) if not math.isnan(w.ul_mcs) else math.nan
        implied = self.implied_effective_sinr(w.ul_mcs, bler)
        gap_evm = (w.sinr_eq_db - w.sinr_evm_db) if not (
            math.isnan(w.sinr_eq_db) or math.isnan(w.sinr_evm_db)) else math.nan

        # CAPACITY: near the TDD UL ceiling with a healthy link.
        if (not math.isnan(w.ul_prb_ratio) and w.ul_prb_ratio > 0.9 and bler < a.bler_ok
                and not math.isnan(w.ul_mcs) and w.ul_mcs >= 26):
            out.append(Finding(
                w.label, "UL", "PRB使用率≈100%・BLER低・高MCS",
                f"prb={w.ul_prb_ratio:.0%} bler={bler:.1%} mcs={w.ul_mcs:.0f}",
                "CAPACITY",
                f"UL スロット比 {DEFAULT_UL_SLOT_RATIO:.0%} (DDDSUUDDDD) の理論上限。対処不要。"
                "増速には tdd_ul_dl_cfg の UL 比か帯域/レイヤ増。"))
            return out

        # Healthy link.
        if bler <= a.bler_ok:
            return out

        # From here: BLER is elevated.
        snr_margin = snr - req if not math.isnan(req) else math.nan
        sched_mcs = scheduler_mcs_for_snr(snr)

        if not math.isnan(w.ul_mcs) and w.ul_mcs > sched_mcs + 2:
            # The scheduler's own table would not pick this MCS at this SNR: link adaptation is
            # not tracking at all (independently of table calibration).
            out.append(Finding(
                w.label, "UL", "観測MCSが報告SNRからの選択レンジを超過(リンクアダプ不追従)",
                f"snr={snr:.1f}dB -> テーブル選択MCS={sched_mcs} だが観測mcs={w.ul_mcs:.0f}, bler={bler:.1%}",
                "CODE",
                "SNR→MCS 選択が機能していない。ue_link_adaptation_controller.cpp / mcs_calculator.cpp、"
                "および min/max MCS 固定設定(pusch.min_ue_mcs/max_ue_mcs)を確認。"))
        elif not math.isnan(gap_evm) and gap_evm > a.gap_db / 2:
            # Receiver-side degradation invisible to the DMRS-based noise estimate. This check does not
            # need the MCS, so it also works on PHY-log-only inputs.
            sub = self._subdivide_receiver_gap(w)
            ev = (f"sinr_eq={w.sinr_eq_db:.1f}dB sinr_evm={w.sinr_evm_db:.1f}dB "
                  f"(gap={gap_evm:.1f}dB) bler={bler:.1%}")
            if not math.isnan(req):
                ev += f" mcs={w.ul_mcs:.0f} (要求SNR≈{req:.1f}dB)"
            out.append(Finding(
                w.label, "UL", "報告SNR高・実効SINR低(受信劣化が雑音推定に乗っていない)",
                ev, sub[0], sub[1]))
        elif not math.isnan(snr_margin) and snr_margin > a.gap_db:
            # Reported SNR says the MCS should easily pass, BLER is high, and no (or no visible)
            # eq-vs-evm gap: the link adaptation side cannot correct.
            ev = f"snr={snr:.1f}dB 要求SNR≈{req:.1f}dB (margin {snr_margin:+.1f}dB) bler={bler:.1%}"
            if not math.isnan(w.ul_olla_db):
                ev += f" ul_olla={w.ul_olla_db:+.1f}dB"
                if abs(w.ul_olla_db) >= a.olla_clamp_db - 0.2:
                    ev += " (OLLA 飽和)"
            action = ("UL OLLA が BLER を吸収できていない。config: pusch.olla_max_snr_offset(既定5dB)拡大 / "
                      "olla_target_bler 緩和 / expert_phy.pusch_sinr_calc_method: evm を試す。"
                      "コード: lib/scheduler/ue_context/ue_link_adaptation_controller.cpp:65-67 の"
                      "更新スキップと outer_loop_link_adaptation.h のクランプ。"
                      "sinr_evm 不明なら pusch_diagnostics_enabled で受信ギャップの有無も確認。")
            out.append(Finding(
                w.label, "UL", "報告SNR高・BLER高・MCS据え置き(リンクアダプ非追従)", ev, "CODE", action))
        else:
            # Reported SNR is consistent with the observed BLER -> physics.
            ev = f"snr={snr:.1f}dB bler={bler:.1%}"
            if not math.isnan(req):
                ev = f"snr={snr:.1f}dB 要求SNR≈{req:.1f}dB bler={bler:.1%}"
            if not math.isnan(w.ul_rsrp_db) and w.ul_rsrp_db < a.low_rsrp_dbm:
                out.append(Finding(
                    w.label, "UL", "低SNR・BLERはSNRに整合(リンクバジェット不足)",
                    ev + f" rsrp={w.ul_rsrp_db:.1f}dBm", "ENV/RU",
                    "UE 位置/送信電力/アンテナを確認。コード不要。"))
            else:
                out.append(Finding(
                    w.label, "UL", "BLERは報告SNRに整合(伝搬/干渉)",
                    ev, "ENV/RU",
                    "干渉源・伝搬変動を確認(スペアナ/別時間帯測定)。コード不要。"
                    "OLLA が正しく MCS を下げているなら動作は正常。"))

        # Independent add-on symptoms.
        out.extend(self._ul_addons(w, bler))
        return out

    def _subdivide_receiver_gap(self, w):
        a = self.args
        # 1) OFH window problems dominate everything.
        if (w.ofh_late + w.ofh_early) > 0 or w.ofh_missed_ul_symbols > 0:
            return ("CONFIG",
                    "OFH 受信窓外れあり: まず ta4_min/ta4_max を広げ、輸送遅延/PTP を確認。"
                    "正しい設定でも残るなら DU の受信窓ロジック"
                    "(lib/ofh/receiver/ofh_rx_window_checker.cpp)を調査 [CONFIG→CODE]。")
        # 2) Rx branch imbalance.
        if len(w.nvar_p_db) >= 2:
            imb = max(w.nvar_p_db) - min(w.nvar_p_db)
            if imb > a.branch_imbalance_db:
                return ("ENV/RU",
                        f"ポート間雑音分散差 {imb:.1f}dB > {a.branch_imbalance_db}dB: "
                        "RU 受信ブランチ校正/故障を確認。コード不要。")
        if len(w.rsrp_p_db) >= 2:
            imb = max(w.rsrp_p_db) - min(w.rsrp_p_db)
            if imb > a.branch_imbalance_db:
                return ("ENV/RU",
                        f"ポート間 RSRP 差 {imb:.1f}dB > {a.branch_imbalance_db}dB: "
                        "RU ブランチゲイン校正を確認。コード不要。")
        # 3) Rank-2 separability.
        if not math.isnan(w.cond_db) and w.cond_db > a.cond_high_db:
            return ("ENV/RU",
                    f"cond={w.cond_db:.1f}dB(レイヤ分離悪条件): 空間相関大 [ENV]。"
                    "併せて RI 判定の楽観(ue_channel_state_manager.cpp:87-137, SRS 雑音-30dB固定仮定)"
                    "を確認 [CODE]。短期は pusch.max_rank=1。")
        # 4) Phase drift (CFO instability between PUSCH transmissions).
        if not math.isnan(w.cfo_spread_hz) and abs(w.cfo_spread_hz) > a.cfo_spread_hz:
            return ("CODE",
                    f"CFO 変動 {w.cfo_spread_hz:.0f}Hz/窓: 位相追従不足の疑い。"
                    "config: expert_phy.pusch_channel_estimator_td_strategy: interpolate、"
                    "dmrs 追加ポジション増を試す。改善しない場合は位相雑音 [ENV/RU]。"
                    "シンボル別 EVM(phy_level: debug)で単調勾配なら位相系で確定。")
        # 5) Default: data-RE-local corruption (E3 signature).
        extra = ""
        if not math.isnan(w.ldpc_iter_max) and w.ldpc_iter_max >= 9.5:
            extra = f" LDPC max-iter 頻発(iter_max={w.ldpc_iter_max:.0f})が裏付け。"
        return ("ENV/RU",
                "バースト/狭帯域干渉 or RU 内部のインパルス性障害の疑い(DMRS を外れた RE のみ劣化)。"
                "スペアナ/周波数を変えた再測定、RU 交換試験を推奨。" + extra +
                " 恒常的なら IRC 未実装(channel_equalizer_generic_impl.cpp:528, スカラー雑音)も影響 [CODE 提案]。")

    def _ul_addons(self, w, bler):
        a = self.args
        out = []
        # LLR saturation at low-mid SINR (demapper scaling suspicion).
        if (not math.isnan(w.llr_sat) and not math.isnan(w.sinr_eq_db)
                and w.sinr_eq_db < 15.0 and w.llr_sat > 0.6):
            out.append(Finding(
                w.label, "UL", "中低SINRでLLR飽和率が異常に高い",
                f"sinr_eq={w.sinr_eq_db:.1f}dB llr_sat={w.llr_sat:.2f}",
                "CODE",
                "デマップの雑音分散スケーリングを調査: "
                "lib/phy/upper/channel_modulation/demodulation_mapper_*.cpp (RANGE_LIMIT_FLOAT=20)。"))
        # Scheduler starvation: buffer but low PRB usage.
        if (not math.isnan(w.ul_prb_ratio) and w.ul_prb_ratio < 0.7 and w.bsr > a.bsr_backlog
                and bler < 0.05):
            out.append(Finding(
                w.label, "UL", "PRB<100%・BSR滞留・BLER低(グラント不足)",
                f"prb={w.ul_prb_ratio:.0%} bsr={w.bsr}",
                "CONFIG",
                "スケジューラ/BSR: pusch 期待レート・qos 設定・sr_period を確認。"
                "改善なければ lib/scheduler/policy を調査 [CODE]。"))
        return out

    # -- DL rules -------------------------------------------------------------------------
    def diagnose_dl(self, w):
        a = self.args
        out = []
        bler = w.dl_bler()
        if math.isnan(bler) or (w.dl_ok + w.dl_nok) < a.min_samples:
            return out
        if bler <= a.bler_ok:
            return out
        note = "(DL 受信機は UE のためコード帰着は gNB 側 LA/スケジューラ/プリコーディングに限定)"
        if not math.isnan(w.cqi) and w.cqi >= 12:
            ev = f"cqi={w.cqi:.1f} bler={bler:.1%} dl_mcs={w.dl_mcs:.0f}"
            if not math.isnan(w.dl_olla_db):
                ev += f" dl_olla={w.dl_olla_db:+.2f}"
            out.append(Finding(
                w.label, "DL", "CQI高なのにDL BLER高・MCS据え置き", ev,
                "CODE",
                "DL OLLA(CQI 楽観の補正)が不足。config: pdsch.olla_max_cqi_offset(既定4)拡大 / "
                "olla_target_bler 緩和。コード: ue_link_adaptation_controller.cpp:32-52。" + note))
        elif not math.isnan(w.cqi) and w.cqi < 7:
            out.append(Finding(
                w.label, "DL", "DL BLERは低CQIに整合(伝搬/干渉)",
                f"cqi={w.cqi:.1f} bler={bler:.1%}", "ENV/RU",
                "DL リンクバジェット/干渉を確認。コード不要。" + note))
        else:
            out.append(Finding(
                w.label, "DL", "DL BLER高(中CQI)",
                f"cqi={w.cqi:.1f} ri={w.dl_ri:.1f} bler={bler:.1%}", "CODE",
                "rank 遷移時に跳ねるなら RI/PMI 追従(ue_channel_state_manager.cpp)、"
                "恒常なら DL OLLA 設定を確認。" + note))
        return out

    # -- OFH rules ------------------------------------------------------------------------
    def diagnose_ofh(self, w):
        out = []
        tot = w.ofh_early + w.ofh_on_time + w.ofh_late
        if w.ofh_late > 0 or w.ofh_early > 0:
            ratio = (w.ofh_late + w.ofh_early) / tot if tot else 1.0
            out.append(Finding(
                w.label, "OFH", "UL U-plane が受信窓(ta4)外",
                f"early={w.ofh_early} on_time={w.ofh_on_time} late={w.ofh_late} ({ratio:.2%})",
                "CONFIG",
                "ta4_min/ta4_max を実測 earliest/latest_msg_us に合わせ拡大。輸送遅延(スイッチ/PTP)確認。"
                "設定が正しくても出るなら lib/ofh/receiver/ofh_rx_window_checker.cpp と RU の"
                "T2a/Ta3 プロファイル整合を調査 [CONFIG→CODE]。窓外シンボルは 0 のまま復調され UL BLER に直結。"))
        if w.ofh_missed_ul_symbols > 0:
            out.append(Finding(
                w.label, "OFH", "UL シンボル取りこぼし(窓クローズ時未達)",
                f"nof_missed_uplink_symbols={w.ofh_missed_ul_symbols}",
                "CONFIG",
                "ta4_max 拡大 or フロントホール輸送を確認。恒常的なら eAxC/帯域設定と RU 能力を照合。"))
        return out

    # -- driver ---------------------------------------------------------------------------
    def run(self, windows):
        findings = []
        for w in windows:
            findings.extend(self.diagnose_ul(w))
            findings.extend(self.diagnose_dl(w))
            findings.extend(self.diagnose_ofh(w))
        return findings


# ---------------------------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------------------------

def print_report(findings, n_windows, out=sys.stdout):
    if not findings:
        print(f"観測窓 {n_windows} 個: 異常は検出されませんでした。", file=out)
        return
    print(f"# OCUDU リンク診断レポート ({n_windows} 窓, {len(findings)} 所見)", file=out)
    print("", file=out)
    for f in findings:
        print(f"[{f.bucket:<8}] {f.window} {f.link}: {f.symptom}", file=out)
        print(f"    根拠   : {f.evidence}", file=out)
        print(f"    アクション: {f.action}", file=out)
    print("", file=out)
    tally = Counter(f.bucket for f in findings)
    print("## サマリ(バケツ別件数)", file=out)
    for b in BUCKETS:
        if tally.get(b):
            print(f"  {b:<8}: {tally[b]}", file=out)
    actions = Counter((f.bucket, f.symptom, f.action) for f in findings)
    print("## 推奨アクション上位", file=out)
    for (bucket, symptom, action), n in actions.most_common(3):
        print(f"  {n:>3}x [{bucket}] {symptom}", file=out)
        print(f"       -> {action}", file=out)
    print(f"\n(判定モデルと閾値の根拠は {DOC} を参照)", file=out)


# ---------------------------------------------------------------------------------------------
# Self test (the field anchor case)
# ---------------------------------------------------------------------------------------------

def selftest(args):
    ok = True

    def check(name, findings, expect_bucket, expect_kw):
        nonlocal ok
        match = [f for f in findings if f.bucket == expect_bucket and expect_kw in (f.symptom + f.action)]
        status = "PASS" if match else "FAIL"
        if not match:
            ok = False
        print(f"[{status}] {name}: expected [{expect_bucket}] *{expect_kw}*")
        for f in findings:
            print(f"        got [{f.bucket}] {f.symptom}")

    diag = Diagnoser(args)

    # Case 1: the field anchor - rank1, MCS27, reported SNR 31.2 dB, BLER 24%, no diagnostics.
    w = WindowStats(label="anchor", ul_snr_db=31.2, ul_rsrp_db=-35.0, ul_mcs=27,
                    ul_ok=760, ul_nok=240, ul_ri=1, ul_olla_db=-5.0)
    check("anchor rank1 MCS27 SNR31 BLER24% (diag無し)", diag.diagnose_ul(w), "CODE", "リンクアダプ")

    # Case 2: same anchor with diagnostics showing the receiver gap (sinr_evm collapsed).
    w = WindowStats(label="anchor+diag", ul_snr_db=31.2, ul_mcs=27, ul_ok=760, ul_nok=240,
                    sinr_ce_db=31.4, sinr_eq_db=31.2, sinr_evm_db=19.0, ldpc_iter_max=10)
    check("anchor + diag (sinr_evm 19dB)", diag.diagnose_ul(w), "ENV/RU", "干渉")

    # Case 3: receiver gap + OFH late packets -> CONFIG first.
    w = WindowStats(label="ofh-late", ul_snr_db=31.2, ul_mcs=27, ul_ok=760, ul_nok=240,
                    sinr_ce_db=31.4, sinr_eq_db=31.2, sinr_evm_db=19.0, ofh_late=120)
    check("受信ギャップ + OFH late", diag.diagnose_ul(w), "CONFIG", "ta4")

    # Case 4: branch imbalance.
    w = WindowStats(label="branch", ul_snr_db=31.2, ul_mcs=27, ul_ok=760, ul_nok=240,
                    sinr_eq_db=31.2, sinr_evm_db=19.0, nvar_p_db=[-42.0, -41.5, -30.0, -41.8])
    check("受信ギャップ + ブランチ雑音アンバランス", diag.diagnose_ul(w), "ENV/RU", "ブランチ")

    # Case 5: low SNR, MCS tracked by the scheduler table, BLER on the cliff -> ENV.
    w = WindowStats(label="lowsnr", ul_snr_db=14.0, ul_rsrp_db=-95.0, ul_mcs=21, ul_ok=700, ul_nok=300)
    check("低SNRでBLER整合", diag.diagnose_ul(w), "ENV/RU", "リンクバジェット")

    # Case 5b: MCS far above what the SNR->MCS table would ever pick -> broken link adaptation.
    w = WindowStats(label="mcs-runaway", ul_snr_db=14.0, ul_mcs=27, ul_ok=100, ul_nok=900)
    check("SNR14dBでMCS27(選択レンジ超過)", diag.diagnose_ul(w), "CODE", "不追従")

    # Case 6: capacity.
    w = WindowStats(label="cap", ul_snr_db=31.0, ul_mcs=28, ul_ok=1000, ul_nok=2, ul_prb_ratio=0.99)
    check("上限張り付き", diag.diagnose_ul(w), "CAPACITY", "上限")

    # Case 7: DL OLLA.
    w = WindowStats(label="dl", cqi=14.2, dl_mcs=27, dl_ok=900, dl_nok=100, dl_olla_db=-4.0)
    check("DL: CQI高・BLER高", diag.diagnose_dl(w), "CODE", "OLLA")

    # Case 8: OFH window.
    w = WindowStats(label="ofh", ofh_early=0, ofh_on_time=5000, ofh_late=250)
    check("OFH late カウンタ", diag.diagnose_ofh(w), "CONFIG", "ta4")

    print("\nSELFTEST:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


# ---------------------------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------------------------

def main():
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--json", help="JSONL metrics captured from remote_control :8001")
    p.add_argument("--log", help="gNB log file (PHY PUSCH lines / metrics log / OFH stats)")
    p.add_argument("--log-window", type=int, default=100,
                   help="number of PUSCH log lines per observation window [100]")
    p.add_argument("--table-offset", type=float, default=3.0,
                   help="dB added to the scheduler SNR->MCS table to model the real requirement "
                        "at 100MHz/273PRB (measured ~3dB, see docs) [3.0]")
    p.add_argument("--bler-ok", dest="bler_ok", type=float, default=0.02,
                   help="BLER below this is considered healthy [0.02]")
    p.add_argument("--gap-db", dest="gap_db", type=float, default=6.0,
                   help="SNR margin / eq-vs-evm gap regarded as anomalous [6.0]")
    p.add_argument("--branch-imbalance-db", type=float, default=6.0,
                   help="per-port noise/RSRP spread regarded as branch imbalance [6.0]")
    p.add_argument("--cond-high-db", type=float, default=10.0,
                   help="2-layer Gram condition number regarded as ill-conditioned [10.0]")
    p.add_argument("--cfo-spread-hz", type=float, default=200.0,
                   help="CFO spread within a window regarded as phase instability [200.0]")
    p.add_argument("--olla-clamp-db", type=float, default=5.0,
                   help="configured pusch.olla_max_snr_offset, to flag OLLA saturation [5.0]")
    p.add_argument("--low-rsrp-dbm", type=float, default=-90.0,
                   help="UL RSRP below this is regarded as link-budget limited [-90.0]")
    p.add_argument("--bsr-backlog", type=int, default=10000,
                   help="BSR bytes regarded as backlog for the scheduler rule [10000]")
    p.add_argument("--min-samples", type=int, default=20,
                   help="minimum CRC samples per window to evaluate BLER rules [20]")
    p.add_argument("--selftest", action="store_true", help="run the built-in rule self test")
    args = p.parse_args()

    if args.selftest:
        sys.exit(selftest(args))

    if not args.json and not args.log:
        p.error("at least one of --json / --log (or --selftest) is required")

    windows = []
    if args.json:
        windows.extend(JsonMetricsParser().parse(args.json))
    if args.log:
        windows.extend(GnbLogParser(args.log_window).parse(args.log))

    findings = Diagnoser(args).run(windows)
    print_report(findings, len(windows))


if __name__ == "__main__":
    main()
