"""Minimal Act HTTP transport."""

from __future__ import annotations

import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any
from urllib.parse import urlparse

from serve.session import ModelSession

MAX_BODY_BYTES = 64 * 1024 * 1024   # 3 raw camera views fit far below this


def serve_act_http(session: ModelSession, host: str, port: int) -> None:
    handler = _handler(session)
    httpd = ThreadingHTTPServer((host, port), handler)
    try:
        httpd.serve_forever()
    finally:
        httpd.server_close()


def _handler(session: ModelSession):
    class Handler(BaseHTTPRequestHandler):
        server_version = "FlashRTNexusAct/0.1"

        def do_GET(self) -> None:  # noqa: N802
            path = urlparse(self.path).path
            if path == "/healthz":
                self._json(200, session.health())
            elif path == "/v1/state":
                self._json(200, session.state())
            else:
                self._json(404, {"error": "not_found"})

        def do_POST(self) -> None:  # noqa: N802
            path = urlparse(self.path).path
            try:
                body = self._read_json()
                if path == "/v1/act":
                    result = session.act(body)
                    self._json(200, {
                        "actions": result.actions,
                        "chunk_id": result.chunk_id,
                        "latency_ms": result.latency_ms,
                    })
                elif path == "/v1/session/snapshot":
                    cap_id = session.snapshot(body.get("capsule"))
                    self._json(200, {"capsule": cap_id})
                elif path == "/v1/session/reset":
                    cap_id = body.get("capsule")
                    if not cap_id:
                        raise ValueError("capsule is required")
                    session.reset(str(cap_id))
                    self._json(200, {"restored": cap_id})
                else:
                    self._json(404, {"error": "not_found"})
            except KeyError as exc:
                self._json(404, {"error": "unknown_capsule", "capsule": str(exc)})
            except Exception as exc:  # noqa: BLE001
                self._json(400, {"error": str(exc)})

        def log_message(self, fmt: str, *args: Any) -> None:
            return

        def _read_json(self) -> dict[str, Any]:
            n = int(self.headers.get("content-length", "0"))
            if n < 0 or n > MAX_BODY_BYTES:
                raise ValueError(f"body must be 0..{MAX_BODY_BYTES} bytes")
            if n == 0:
                return {}
            data = self.rfile.read(n)
            value = json.loads(data.decode())
            if not isinstance(value, dict):
                raise ValueError("JSON body must be an object")
            return value

        def _json(self, status: int, value: dict[str, Any]) -> None:
            data = json.dumps(value).encode()
            self.send_response(status)
            self.send_header("content-type", "application/json")
            self.send_header("content-length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

    return Handler
