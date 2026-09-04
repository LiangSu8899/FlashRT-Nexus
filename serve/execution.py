"""Single-consumer execution lifecycle, independent of its transport."""

from concurrent.futures import ThreadPoolExecutor
import math
import time
import uuid

from .worker import ExecutionWorker


class ServiceError(RuntimeError):
    def __init__(self, status, code):
        super().__init__(code)
        self.status = status
        self.code = code


class ExecutionService:
    """One lease, one retained result, serialized execute/reset/close.

    One host thread calls the management methods. Blocking worker operations
    run on one executor thread; expiry discards results and queues reset behind
    execution. It never cancels GPU work or admits a second model invocation.
    """

    def __init__(self, entry, config, *, lease_timeout_s=30.0):
        if not math.isfinite(lease_timeout_s) or lease_timeout_s <= 0:
            raise ValueError("lease_timeout_s must be positive")
        self.worker = ExecutionWorker(entry, config)
        self.description = self.worker.description
        self.epoch = uuid.uuid4().hex
        self.lease_timeout_s = lease_timeout_s
        self._pool = ThreadPoolExecutor(max_workers=1)
        self._lease = None
        self._seen = 0.0
        self._pending = None
        self._reset = None
        self._request = None
        self._failed = False
        self._closed = False

    def maintain(self):
        if self._lease and time.monotonic() - self._seen >= self.lease_timeout_s:
            self._begin_reset(release=True)
        if self._reset is not None and self._reset.done():
            try:
                self._reset.result()
            except Exception:
                self._failed = True
            self._reset = None

    def health(self):
        self.maintain()
        return {"version": 1, "epoch": self.epoch,
                "ready": not self._closed and not self._failed,
                "leased": self._lease is not None,
                "resetting": self._reset is not None,
                "outstanding": int(self._pending is not None)}

    def acquire(self):
        self.maintain()
        if self._closed or self._failed:
            raise ServiceError(503, "unavailable")
        if self._lease is not None or self._reset is not None:
            raise ServiceError(409, "busy")
        self._lease = uuid.uuid4().hex
        self._seen = time.monotonic()
        return {"version": 1, "epoch": self.epoch, "lease": self._lease,
                "description": self.description,
                "lease_timeout_s": self.lease_timeout_s}

    def _check(self, body):
        self.maintain()
        if (body.get("epoch") != self.epoch or not self._lease
                or body.get("lease") != self._lease):
            raise ServiceError(409, "stale_session")
        if self._closed or self._failed:
            raise ServiceError(503, "unavailable")
        self._seen = time.monotonic()

    def _execute(self, inputs):
        self.worker.submit(inputs)
        return self.worker.result()

    def submit(self, body):
        self._check(body)
        if self._pending is not None or self._reset is not None:
            raise ServiceError(409, "busy")
        request = body.get("request")
        if not isinstance(request, str) or not 1 <= len(request) <= 128:
            raise ServiceError(400, "invalid_request_id")
        inputs = body["inputs"]
        self._request = request
        self._pending = self._pool.submit(self._execute, inputs)
        return {"request": request, "state": "pending"}

    def result(self, body):
        self._check(body)
        if self._pending is None or body.get("request") != self._request:
            raise ServiceError(409, "stale_request")
        result = {"request": self._request, "state": "pending"}
        if self._pending.done():
            try:
                result.update(state="ready", output=self._pending.result())
            except Exception as exc:
                result.update(state="error", error="provider_error",
                              provider_error_type=type(exc).__name__)
        return result

    def acknowledge(self, body):
        self._check(body)
        if (self._pending is None or body.get("request") != self._request
                or not self._pending.done()):
            raise ServiceError(409, "stale_request")
        self._pending = None
        self._request = None
        return {"state": "idle"}

    def _begin_reset(self, *, release=False):
        self._pending = None
        self._request = None
        if self._reset is None:
            self._reset = self._pool.submit(self.worker.reset)
        if release:
            self._lease = None

    def reset(self, body):
        self._check(body)
        self._begin_reset()
        return {"state": "resetting"}

    def status(self, body):
        self._check(body)
        return {"state": "resetting" if self._reset is not None else "idle"}

    def release(self, body):
        self._check(body)
        self._begin_reset(release=True)
        return {"state": "released"}

    def close(self):
        if self._closed:
            return
        self._closed = True
        self._lease = None
        try:
            self._pool.submit(self.worker.close).result()
        finally:
            self._pool.shutdown(wait=True)


def open_execution_service(manifest):
    producer = manifest["producer"]
    if producer["kind"] == "worker":
        entry, config = producer["entry"], producer.get("config", {})
    elif producer["kind"] in {"native", "python"}:
        entry, config = "serve.native_worker:build", manifest
    else:
        raise ValueError("execution service requires a local producer")
    return ExecutionService(entry, config, lease_timeout_s=float(
        manifest.get("serve", {}).get("lease_timeout_s", 30)))
