# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI

"""ISAC sensing PoC - Block D server.

Receives CSI snapshots from Block A over ZMQ SUB and runs the detector at the raw snapshot
rate (detection loop), while broadcasting the latest frame to browser clients over WebSocket at
a throttled display rate (10-20 fps). Also serves the visualization page over HTTP.

Robustness: a malformed message or a per-message processing error is logged and skipped —
it never terminates the server.
"""

from __future__ import annotations

import argparse
import asyncio
import functools
import http.server
import json
import os
import threading
import time

import zmq
import zmq.asyncio

from detector import Detector, DetectorConfig
from wire import parse

WEB_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "web")


def connect_endpoint(ep: str) -> str:
    # PUB binds with '*'; SUB must connect to a concrete host.
    return ep.replace("//*:", "//127.0.0.1:").replace("//0.0.0.0:", "//127.0.0.1:")


def serve_http(port: int) -> None:
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=WEB_DIR)
    httpd = http.server.ThreadingHTTPServer(("0.0.0.0", port), handler)
    httpd.serve_forever()


class App:
    MAX_ERROR_LOGS = 10

    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.det = Detector(DetectorConfig(
            calib_seconds=args.calib_seconds, calib_k=args.k, hold_frames=args.hold,
            combine=args.combine, normalize=args.normalize, target_rnti=args.rnti))
        self.clients: set = set()
        self.t0 = time.time()
        self.malformed = 0
        self.errors = 0

    async def recv_loop(self) -> None:
        ctx = zmq.asyncio.Context.instance()
        sub = ctx.socket(zmq.SUB)
        sub.connect(self.args.zmq)
        sub.setsockopt(zmq.SUBSCRIBE, b"")
        print(f"[blockd] SUB connected to {self.args.zmq}")
        while True:
            raw = await sub.recv()
            snap = parse(raw)
            if snap is None:
                self.malformed += 1
                if self.malformed <= self.MAX_ERROR_LOGS:
                    print(f"[blockd] dropped malformed message ({len(raw)} bytes, total {self.malformed})")
                continue
            try:
                self.det.process(snap)  # detection at raw rate (display is built lazily)
            except Exception as exc:  # noqa: BLE001 - one bad snapshot must not kill the server
                self.errors += 1
                if self.errors <= self.MAX_ERROR_LOGS:
                    print(f"[blockd] detector error (total {self.errors}): {exc!r}")

    async def broadcast_loop(self) -> None:
        period = 1.0 / self.args.fps
        while True:
            await asyncio.sleep(period)
            if not self.clients:
                continue
            frame = self.det.render_frame()  # None when nothing new arrived -> skip the tick
            if frame is None:
                continue
            frame["uptime"] = int(time.time() - self.t0)
            msg = json.dumps(frame)
            await asyncio.gather(*(c.send(msg) for c in list(self.clients)), return_exceptions=True)

    async def ws_handler(self, ws) -> None:
        self.clients.add(ws)
        print(f"[blockd] client connected ({len(self.clients)} total)")
        try:
            async for _ in ws:  # ignore inbound; keep the connection open
                pass
        except Exception:
            pass
        finally:
            self.clients.discard(ws)
            print(f"[blockd] client disconnected ({len(self.clients)} total)")

    async def run(self) -> None:
        import websockets
        threading.Thread(target=serve_http, args=(self.args.http_port,), daemon=True).start()
        print(f"[blockd] HTTP serving {WEB_DIR} on http://0.0.0.0:{self.args.http_port}")
        async with websockets.serve(self.ws_handler, "0.0.0.0", self.args.ws_port):
            print(f"[blockd] WebSocket on ws://0.0.0.0:{self.args.ws_port}")
            print(f"[blockd] open  http://localhost:{self.args.http_port}/index.html"
                  f"?ws={self.args.ws_port}")
            await asyncio.gather(self.recv_loop(), self.broadcast_loop())


def main() -> None:
    env_ep = os.environ.get("OCUDU_ISAC_ZMQ_ENDPOINT", "tcp://127.0.0.1:5599")
    ap = argparse.ArgumentParser(description="ISAC Block D detector + visualization server")
    ap.add_argument("--zmq", default=connect_endpoint(env_ep), help="Block A PUB endpoint to connect to")
    ap.add_argument("--ws-port", type=int, default=8765)
    ap.add_argument("--http-port", type=int, default=8080)
    ap.add_argument("--fps", type=float, default=15.0, help="display broadcast rate")
    ap.add_argument("--combine", default="mean", choices=["mean", "l2", "max", "snr"])
    ap.add_argument("--normalize", default="baseline", choices=["baseline", "rsrp", "epre"])
    ap.add_argument("--calib-seconds", type=float, default=4.0)
    ap.add_argument("--k", type=float, default=4.0)
    ap.add_argument("--hold", type=int, default=12)
    ap.add_argument("--rnti", type=lambda v: int(v, 0), default=None,
                    help="lock to this UE RNTI (e.g. 0x4601); default: first RNTI seen")
    args = ap.parse_args()
    args.zmq = connect_endpoint(args.zmq)
    try:
        asyncio.run(App(args).run())
    except KeyboardInterrupt:
        print("\n[blockd] stopped")


if __name__ == "__main__":
    main()
