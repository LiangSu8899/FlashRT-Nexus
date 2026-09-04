"""Resident execution in one spawned process, independent of model runtimes."""

from __future__ import annotations

import importlib
import contextlib
import multiprocessing
import pickle
import sys
from concurrent.futures import ProcessPoolExecutor
from typing import Any


_provider = None


def _open(entry: str, config: dict):
    global _provider
    module, name = entry.split(":", 1)
    with contextlib.redirect_stdout(sys.stderr):
        _provider = getattr(importlib.import_module(module), name)(config)
        try:
            return _provider.describe()
        except BaseException:
            _provider.close()
            raise


def _call(method: str, payload: bytes | None = None):
    fn = getattr(_provider, method)
    with contextlib.redirect_stdout(sys.stderr):
        return fn() if payload is None else fn(pickle.loads(payload))


class ExecutionWorker:
    """Serialize a provider's describe/execute/reset/close lifecycle.

    The factory runs inside a spawned process, so CUDA is not inherited from
    the parent. One uncollected request is allowed. Input serialization happens
    at submit time; callers may reuse observation buffers after submit returns.
    Payloads are trusted local Python objects, not a network protocol.
    """

    def __init__(self, entry: str, config: dict[str, Any]):
        if ":" not in entry:
            raise ValueError("worker entry must use module:callable")
        self._pool = ProcessPoolExecutor(
            max_workers=1, mp_context=multiprocessing.get_context("spawn"))
        self._pending = None
        self._closed = False
        try:
            self.description = self._pool.submit(_open, entry, config).result()
        except BaseException:
            self._pool.shutdown(wait=True, cancel_futures=True)
            self._closed = True
            raise

    def submit(self, inputs: Any) -> None:
        if self._closed:
            raise RuntimeError("execution worker is closed")
        if self._pending is not None:
            raise RuntimeError("execution worker already has a request")
        payload = pickle.dumps(inputs, protocol=pickle.HIGHEST_PROTOCOL)
        self._pending = self._pool.submit(_call, "execute", payload)

    @property
    def ready(self) -> bool:
        return self._pending is not None and self._pending.done()

    def result(self, timeout: float | None = None) -> Any:
        if self._pending is None:
            raise RuntimeError("execution worker has no request")
        pending = self._pending
        try:
            return pending.result(timeout=timeout)
        finally:
            if pending.done():
                self._pending = None

    def reset(self) -> None:
        if self._closed:
            raise RuntimeError("execution worker is closed")
        # Serialized behind the current request. Old output cannot cross reset.
        self._pool.submit(_call, "reset").result()
        self._pending = None

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        try:
            self._pool.submit(_call, "close").result()
        finally:
            self._pool.shutdown(wait=True, cancel_futures=True)
            self._pending = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
