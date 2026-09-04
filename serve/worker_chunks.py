"""Connect resident execution to the existing C action-chunk state machine."""

from __future__ import annotations

import ctypes
from types import SimpleNamespace

import numpy as np

from flashrt_nexus.library import find_library
from .action_chunk import ActionChunkOptions, ActionChunkSession, _CONSUME
from .ffi import NexusActionChunkConfig, bind_nexus
from .worker import ExecutionWorker


_Verb = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p)
_Read = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p,
                        ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64))


class _Executor(ctypes.Structure):
    _fields_ = [("struct_size", ctypes.c_uint32), ("self", ctypes.c_void_p),
                ("submit", _Verb), ("query", _Verb), ("sync", _Verb),
                ("read", _Read)]


class WorkerActionChunks(ActionChunkSession):
    """Host-owned worker; this controller owns only its chunk mode.

    Call from one control thread. Reset drains the worker before resetting the
    mode. Close drains pending work before releasing callbacks and mode state.
    The worker returns a finite float32 array with its declared action shape.
    """

    def __init__(self, worker: ExecutionWorker, *, nexus_lib=None, **kwargs):
        self.worker = worker
        self.options = ActionChunkOptions(**kwargs)
        self.nx = ctypes.CDLL(find_library(nexus_lib, execution_only=True))
        bind_nexus(self.nx, model_runtime=False)
        shape = tuple(worker.description["action_shape"])
        if len(shape) != 2 or any(int(d) != d or d <= 0 for d in shape):
            raise ValueError("worker action_shape must be two positive integers")
        self.session = SimpleNamespace(action_shape=shape)
        self._mode = ctypes.c_void_p()
        self._dag = ctypes.c_void_p()
        self._closed = False
        self._error = None
        self._inputs = None
        self._output = None
        self._executor = _Executor(
            ctypes.sizeof(_Executor), None,
            _Verb(self._submit), _Verb(self._query),
            _Verb(self._sync), _Read(self._read))
        config = NexusActionChunkConfig()
        config.struct_size = ctypes.sizeof(config)
        config.output_port = 0
        config.chunk_length, action_dim = shape
        config.action_bytes = action_dim * 4
        config.ring_slots = self.options.ring_slots
        config.poll_budget = self.options.poll_budget
        config.deadline_steps = self.options.deadline_steps
        config.consume_policy = _CONSUME[self.options.consume]
        if self.options.miss not in {"report", "hold_last"}:
            raise ValueError("miss must be report or hold_last")
        config.miss_policy = int(self.options.miss == "hold_last")
        config.scalar_dtype = 1
        config.fusion_decay = self.options.fusion_decay
        config.fusion_max_chunks = self.options.fusion_max_chunks
        config.switch_offset = self.options.switch_offset
        create = self.nx.nexus_action_chunk_create_external
        create.argtypes = [ctypes.POINTER(_Executor),
                           ctypes.POINTER(NexusActionChunkConfig),
                           ctypes.POINTER(ctypes.c_void_p)]
        create.restype = ctypes.c_int
        rc = create(ctypes.byref(self._executor), ctypes.byref(config),
                    ctypes.byref(self._mode))
        if rc:
            raise RuntimeError(f"nexus_action_chunk_create_external rc={rc}")

    def _submit(self, _):
        try:
            self.worker.submit(self._inputs)
            self._output = None
            return 0
        except Exception as exc:
            self._error = exc
            return -1

    def _collect(self):
        output = np.asarray(self.worker.result(), dtype=np.float32)
        if output.shape != self.session.action_shape or not np.isfinite(output).all():
            raise ValueError("worker returned invalid action shape or non-finite actions")
        self._output = np.ascontiguousarray(output)

    def _query(self, _):
        try:
            if self._output is not None:
                return 0
            if not self.worker.ready:
                return 1
            self._collect()
            return 0
        except Exception as exc:
            self._error = exc
            return -1

    def _sync(self, _):
        try:
            if self._output is None:
                self._collect()
            return 0
        except Exception as exc:
            self._error = exc
            return -1

    def _read(self, _, out, capacity, written):
        if self._output is None or capacity < self._output.nbytes:
            return -1
        ctypes.memmove(out, self._output.ctypes.data, self._output.nbytes)
        written[0] = self._output.nbytes
        return 0

    def request_inputs(self, inputs):
        if self._closed:
            raise RuntimeError("action chunk controller is closed")
        if self.in_flight:
            raise RuntimeError("an action chunk request is already in flight")
        self._inputs = inputs
        rc = self.nx.nexus_action_chunk_request(self._mode)
        self._inputs = None
        self._raise_error()
        if rc:
            raise RuntimeError(f"action chunk request rc={rc}")

    def request(self, images, *, state=None, prompt=None, seed=None):
        self.request_inputs(dict(images=images, state=state, prompt=prompt, seed=seed))

    def _raise_error(self):
        if self._error is not None:
            raise RuntimeError("execution provider failed") from self._error

    def poll(self):
        try:
            return super().poll()
        finally:
            self._raise_error()

    def reset(self):
        self.worker.reset()
        super().reset()
        self._error = None
        self._output = None

    def close(self):
        if not self._closed:
            try:
                if self.in_flight and self._output is None:
                    self.worker.result()
            finally:
                super().close()
