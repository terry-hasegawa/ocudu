# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI

"""ISAC sensing PoC - Block D wire parser.

Decodes the per-PUSCH-PDU CSI snapshot published by Block A. The layout here is the
authoritative one from lib/isac/README.md: a fixed 100-byte little-endian header (v2) followed
by a branch-major float32 body of interleaved (real, imag) channel coefficients.

parse() never raises: any malformed, truncated, foreign, or unsupported-version message
returns None.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

import numpy as np

# Authoritative format (see lib/isac/README.md and lib/isac/isac_csi_payload.h).
# v2: ...Q ts_rel_ns, 12f epre[4]/rsrp[4]/snr[4], H rnti, B rnti_valid, 3x reserved.
HDR_FMT = "<IHHIIIIHBBBBHHHBBQ12fHB3x"
HDR_SIZE = struct.calcsize(HDR_FMT)
assert HDR_SIZE == 100, f"unexpected header size {HDR_SIZE}"

MAGIC = 0x43415349  # "ISAC"
VERSION = 2
MAX_RX_PORTS = 4
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
    rnti: int | None  # UE identity, None if Block A could not stamp it
    epre: np.ndarray  # linear, length nof_rx_ports
    rsrp: np.ndarray  # linear, length nof_rx_ports
    snr: np.ndarray   # linear, length nof_rx_ports
    hmag: np.ndarray  # |H|, shape (nof_rx_ports, nof_re), float32

    @property
    def abs_sc_start(self) -> int:
        """Absolute subcarrier index (CRB-based) of the first body subcarrier.

        Only meaningful when is_contiguous is True; for non-contiguous allocations the body
        packs the allocated PRBs densely and there is no single contiguous absolute range.
        """
        return self.prb_start * SC_PER_PRB


def parse(buf: bytes) -> CsiSnapshot | None:
    """Parses a raw ZMQ message into a CsiSnapshot, or None if malformed."""
    try:
        if len(buf) < HDR_SIZE:
            return None

        f = struct.unpack(HDR_FMT, buf[:HDR_SIZE])
        (magic, version, header_bytes, seq, sfn, slot_index, system_slot,
         scs_khz, numerology, rank, nof_rx_ports, dmrs_symbol,
         prb_start, prb_count, nof_re, is_contiguous, has_metrics, ts_rel_ns) = f[:18]
        rnti_val, rnti_valid = f[30], f[31]

        if magic != MAGIC or version != VERSION:
            return None
        # header_bytes allows a future producer to grow the header; it must at least cover v2.
        if header_bytes < HDR_SIZE or header_bytes > len(buf):
            return None
        if not (1 <= nof_rx_ports <= MAX_RX_PORTS) or nof_re == 0:
            return None

        need = nof_rx_ports * nof_re * 2
        if len(buf) < header_bytes + need * 4:
            return None

        epre = np.asarray(f[18:22], dtype=np.float32)[:nof_rx_ports]
        rsrp = np.asarray(f[22:26], dtype=np.float32)[:nof_rx_ports]
        snr = np.asarray(f[26:30], dtype=np.float32)[:nof_rx_ports]

        body = np.frombuffer(buf, dtype="<f4", count=need, offset=header_bytes)
        hc = body.reshape(nof_rx_ports, nof_re, 2)
        hmag = np.abs(hc[..., 0] + 1j * hc[..., 1]).astype(np.float32)

        return CsiSnapshot(
            seq=seq, sfn=sfn, slot_index=slot_index, system_slot=system_slot,
            scs_khz=scs_khz, numerology=numerology, rank=rank, nof_rx_ports=nof_rx_ports,
            dmrs_symbol=dmrs_symbol, prb_start=prb_start, prb_count=prb_count, nof_re=nof_re,
            is_contiguous=bool(is_contiguous), has_metrics=bool(has_metrics), ts_rel_ns=ts_rel_ns,
            rnti=(rnti_val if rnti_valid else None),
            epre=epre, rsrp=rsrp, snr=snr, hmag=hmag,
        )
    except (struct.error, ValueError):
        # Belt and braces: a parser bug or hostile input must never take Block D down.
        return None
