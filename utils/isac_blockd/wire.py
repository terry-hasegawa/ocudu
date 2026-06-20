# SPDX-FileCopyrightText: 2026 OCUDU ISAC sensing PoC
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
"""ISAC sensing PoC - Block D wire parser.

Decodes the per-PUSCH-PDU CSI snapshot published by Block A. The layout here is the
authoritative one from lib/isac/README.md: a fixed 94-byte little-endian header followed by
a branch-major float32 body of interleaved (real, imag) channel coefficients.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

import numpy as np

# Authoritative format (see lib/isac/README.md and lib/isac/isac_csi_payload.h).
HDR_FMT = "<IHHIIIIHBBBBHHHBBQ12f"  # 12f = epre[4], rsrp[4], snr[4]
HDR_SIZE = struct.calcsize(HDR_FMT)
assert HDR_SIZE == 94, f"unexpected header size {HDR_SIZE}"

MAGIC = 0x43415349  # "ISAC"
SC_PER_PRB = 12


@dataclass(slots=True)
class CsiSnapshot:
    """One decoded per-slot CSI snapshot."""

    seq: int
    sfn: int
    slot_index: int
    system_slot: int
    scs_khz: int
    numerology: int
    rank: int
    nof_rx_ports: int
    dmrs_symbol: int
    prb_start: int
    prb_count: int
    nof_re: int
    is_contiguous: bool
    has_metrics: bool
    ts_rel_ns: int
    epre: np.ndarray  # linear, length nof_rx_ports
    rsrp: np.ndarray  # linear, length nof_rx_ports
    snr: np.ndarray   # linear, length nof_rx_ports
    hmag: np.ndarray  # |H|, shape (nof_rx_ports, nof_re), float32

    @property
    def abs_sc_start(self) -> int:
        """Absolute subcarrier index (CRB-based) of the first body subcarrier."""
        return self.prb_start * SC_PER_PRB


def parse(buf: bytes) -> CsiSnapshot | None:
    """Parses a raw ZMQ message into a CsiSnapshot, or None if malformed."""
    if len(buf) < HDR_SIZE:
        return None

    f = struct.unpack(HDR_FMT, buf[:HDR_SIZE])
    (magic, _version, header_bytes, seq, sfn, slot_index, system_slot,
     scs_khz, numerology, rank, nof_rx_ports, dmrs_symbol,
     prb_start, prb_count, nof_re, is_contiguous, has_metrics, ts_rel_ns) = f[:18]

    if magic != MAGIC or nof_rx_ports == 0 or nof_re == 0:
        return None

    epre = np.asarray(f[18:22], dtype=np.float32)[:nof_rx_ports]
    rsrp = np.asarray(f[22:26], dtype=np.float32)[:nof_rx_ports]
    snr = np.asarray(f[26:30], dtype=np.float32)[:nof_rx_ports]

    body = np.frombuffer(buf[header_bytes:], dtype="<f4")
    need = nof_rx_ports * nof_re * 2
    if body.size < need:
        return None

    hc = body[:need].reshape(nof_rx_ports, nof_re, 2)
    hmag = np.abs(hc[..., 0] + 1j * hc[..., 1]).astype(np.float32)

    return CsiSnapshot(
        seq=seq, sfn=sfn, slot_index=slot_index, system_slot=system_slot,
        scs_khz=scs_khz, numerology=numerology, rank=rank, nof_rx_ports=nof_rx_ports,
        dmrs_symbol=dmrs_symbol, prb_start=prb_start, prb_count=prb_count, nof_re=nof_re,
        is_contiguous=bool(is_contiguous), has_metrics=bool(has_metrics), ts_rel_ns=ts_rel_ns,
        epre=epre, rsrp=rsrp, snr=snr, hmag=hmag,
    )
