"""HTTP adapter over the common execution lifecycle."""

import hmac
from http.server import BaseHTTPRequestHandler, HTTPServer

from ..execution import ServiceError
from . import wire


class ExecutionHTTPServer(HTTPServer):
    request_queue_size = 16

    def __init__(self, address, service, *, token="", socket_timeout_s=2.0):
        if address[0] not in {"127.0.0.1", "localhost"} and not token:
            raise ValueError("non-loopback serving requires an API token and protected network")
        self.service = service
        self.token = token
        self.socket_timeout_s = socket_timeout_s
        super().__init__(address, Handler)

    def get_request(self):
        connection, address = super().get_request()
        connection.settimeout(self.socket_timeout_s)
        return connection, address

    def service_actions(self):
        self.service.maintain()


class Handler(BaseHTTPRequestHandler):
    server_version = "NexusExecution/1"

    def _authorized(self):
        expected = self.server.token
        return not expected or hmac.compare_digest(
            self.headers.get("Authorization", ""), "Bearer " + expected)

    def do_GET(self):
        if not self._authorized():
            self._reply(401, {"error": "unauthorized"})
        elif self.path == "/healthz":
            self._reply(200, self.server.service.health())
        else:
            self._reply(404, {"error": "not_found"})

    def do_POST(self):
        if not self._authorized():
            self._reply(401, {"error": "unauthorized"})
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if not 0 < length <= wire.MAX_BYTES:
                raise ValueError("invalid body size")
            raw = self.rfile.read(length)
            if len(raw) != length:
                raise ValueError("incomplete body")
            body = wire.loads(raw)
            if not isinstance(body, dict):
                raise ValueError("body must be an object")
            service = self.server.service
            routes = {
                "/v1/execution/acquire": lambda _: service.acquire(),
                "/v1/execution/submit": service.submit,
                "/v1/execution/result": service.result,
                "/v1/execution/ack": service.acknowledge,
                "/v1/execution/reset": service.reset,
                "/v1/execution/status": service.status,
                "/v1/execution/release": service.release,
            }
            if self.path not in routes:
                self._reply(404, {"error": "not_found"})
                return
            self._reply(200, routes[self.path](body))
        except ServiceError as exc:
            self._reply(exc.status, {"error": exc.code})
        except (ValueError, TypeError, KeyError, OverflowError, RecursionError):
            self._reply(400, {"error": "bad_request"})
        except (BrokenPipeError, ConnectionResetError, TimeoutError):
            # The lifecycle retains the request/result until ack or lease expiry.
            return

    def _reply(self, status, value):
        data = wire.dumps(value)
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):
        pass
