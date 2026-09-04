"""Execution provider for an independent Nexus service.

Run inside ExecutionWorker to keep network waits off the control thread.
Mutating requests are never retried. A failed connection requires explicit
reset or a new session; an old epoch cannot consume a restarted service.
"""

import os
import math
import time
import uuid
import warnings
from urllib.error import HTTPError, URLError
from urllib.request import Request, HTTPRedirectHandler, build_opener

from .transports import wire


class RemoteExecutionError(RuntimeError):
    pass


class _NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


class RemoteProvider:
    def __init__(self, config):
        self.url = config["url"].rstrip("/")
        self.timeout = float(config.get("request_timeout_s", 2))
        self.execution_timeout = float(config.get("execution_timeout_s", 600))
        self.poll_interval = float(config.get("poll_interval_s", 0.01))
        budgets = (self.timeout, self.execution_timeout, self.poll_interval)
        if not all(math.isfinite(v) and v > 0 for v in budgets):
            raise ValueError("remote timeouts and polling interval must be positive")
        self._http = build_opener(_NoRedirect())
        self.token = os.environ.get(config.get("token_env", "NEXUS_API_TOKEN"), "")
        self.lease = self._call("acquire", {})
        if self.lease.get("version") != 1:
            raise RemoteExecutionError("unsupported execution protocol")
        self._closed = False

    def _call(self, method, body):
        headers = {"Content-Type": "application/json"}
        if self.token:
            headers["Authorization"] = "Bearer " + self.token
        request = Request(self.url + "/v1/execution/" + method,
                          data=wire.dumps(body), headers=headers, method="POST")
        try:
            with self._http.open(request, timeout=self.timeout) as response:
                result = wire.loads(response.read(wire.MAX_BYTES + 1))
                if not isinstance(result, dict):
                    raise ValueError("invalid service response")
                return result
        except HTTPError as exc:
            raise RemoteExecutionError(f"execution service HTTP {exc.code}") from exc
        except (URLError, OSError, ValueError) as exc:
            raise RemoteExecutionError("execution service connection or protocol failure") from exc

    def _body(self, **kwargs):
        return {"epoch": self.lease["epoch"], "lease": self.lease["lease"], **kwargs}

    def describe(self):
        return {**self.lease["description"], "transport": "execution_http"}

    def execute(self, inputs):
        request = uuid.uuid4().hex
        body = self._body(request=request)
        self._call("submit", {**body, "inputs": inputs})
        deadline = time.monotonic() + self.execution_timeout
        while time.monotonic() < deadline:
            result = self._call("result", body)
            if result.get("request") != request:
                raise RemoteExecutionError("execution request identity mismatch")
            if result["state"] == "ready":
                self._call("ack", body)
                return result["output"]
            if result["state"] == "error":
                self._call("ack", body)
                raise RemoteExecutionError(
                    "remote provider failed: " + result["provider_error_type"])
            time.sleep(self.poll_interval)
        raise TimeoutError("remote execution deadline exceeded")

    def reset(self):
        self._call("reset", self._body())
        deadline = time.monotonic() + self.execution_timeout
        while time.monotonic() < deadline:
            if self._call("status", self._body())["state"] == "idle":
                return
            time.sleep(self.poll_interval)
        raise TimeoutError("remote reset deadline exceeded")

    def close(self):
        if self._closed:
            return
        self._closed = True
        try:
            self._call("release", self._body())
        except RemoteExecutionError:
            warnings.warn("remote lease release unavailable; server expiry will drain it")


def build(config):
    return RemoteProvider(config)
