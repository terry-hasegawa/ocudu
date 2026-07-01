# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI

"""Wire-format tests: pack (fake_blocka) -> parse (wire) round trip and malformed inputs."""

import os
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from fake_blocka import pack  # noqa: E402
from wire import HDR_SIZE, parse  # noqa: E402


def make_msg(nof_rx=4, prb=8, prb_start=10, seq=7, rnti=0x4601, ts=12345):
    hmag = np.tile(np.linspace(0.2, 0.8, prb * 12, dtype=np.float32), (nof_rx, 1))
    return pack(seq, 1, 5, 25, 30, 1, 1, nof_rx, 2, prb_start, prb, ts,
                np.ones(nof_rx), np.full(nof_rx, 0.1), np.full(nof_rx, 100.0), hmag, rnti), hmag


def test_roundtrip():
    buf, hmag = make_msg()
    s = parse(buf)
    assert s is not None
    assert (s.seq, s.sfn, s.slot_index, s.system_slot) == (7, 1, 5, 25)
    assert (s.scs_khz, s.numerology, s.rank, s.nof_rx_ports) == (30, 1, 1, 4)
    assert (s.prb_start, s.prb_count, s.nof_re) == (10, 8, 96)
    assert s.is_contiguous and s.has_metrics
    assert s.rnti == 0x4601 and s.ts_rel_ns == 12345
    assert s.hmag.shape == (4, 96)
    assert np.allclose(s.hmag, np.abs(hmag), atol=1e-6)
    assert len(s.epre) == len(s.rsrp) == len(s.snr) == 4


def test_roundtrip_fewer_ports():
    # This is the case that used to crash fake_blocka (struct '12f' vs nof_rx-length lists).
    buf, _ = make_msg(nof_rx=2)
    s = parse(buf)
    assert s is not None
    assert s.nof_rx_ports == 2
    assert s.hmag.shape[0] == 2
    assert len(s.snr) == 2  # metric arrays truncated to nof_rx_ports


def test_malformed_never_raises():
    buf, _ = make_msg()
    cases = [
        b"",                                  # empty
        buf[: HDR_SIZE - 1],                  # truncated header
        b"XXXX" + buf[4:],                    # wrong magic
        buf[:4] + b"\x63\x00" + buf[6:],      # wrong version (99)
        buf[:6] + b"\x10\x00" + buf[8:],      # header_bytes (16) < HDR_SIZE
        buf[: HDR_SIZE + 5],                  # truncated / non-multiple-of-4 body
        buf + b"\x00",                        # trailing garbage is tolerated or rejected, not fatal
        os.urandom(256),                      # random noise
    ]
    for c in cases:
        parse(c)  # must not raise; result may be None or a snapshot for the tolerant cases


def test_malformed_rejected():
    buf, _ = make_msg()
    assert parse(b"") is None
    assert parse(buf[: HDR_SIZE - 1]) is None
    assert parse(b"XXXX" + buf[4:]) is None
    assert parse(buf[:4] + b"\x63\x00" + buf[6:]) is None
    assert parse(buf[:6] + b"\x10\x00" + buf[8:]) is None
    assert parse(buf[: HDR_SIZE + 5]) is None
