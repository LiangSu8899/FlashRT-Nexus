"""Model-agnostic execution of a producer-owned model-runtime export."""

import ctypes

from flashrt_nexus.library import find_library
from .ffi import bind_nexus


class AdoptedRuntime:
    """Adopt an export while retaining its graph and tensor owners.

    The caller updates declared input windows between steps. step() executes
    the declared plan and waits for completion. It is intended for a resident
    execution worker; it does not perform policy preprocessing or action IO.
    Closing releases Nexus adoption; the producer still owns its export.
    """

    def __init__(self, export, *, owners=(), nexus_lib=None):
        self._export = export
        self._owners = tuple(owners)
        self.nx = ctypes.CDLL(find_library(nexus_lib))
        bind_nexus(self.nx)
        self.model = ctypes.c_void_p()
        self.ctx = None
        self._streams = set()
        rc = self.nx.flashrt_adopt_model_runtime(
            ctypes.c_void_p(export.ptr), ctypes.byref(self.model))
        if rc:
            raise RuntimeError(f"model-runtime adoption rc={rc}")
        try:
            backend = self.nx.cap_model_backend(self.model)
            self.ctx = self.nx.cap_ctx_create(backend) if backend else None
            if backend and not self.ctx:
                raise RuntimeError("runtime context creation failed")
            self._streams = set(self.nx.cap_model_stage_stream(self.model, i)
                                for i in range(self.nx.cap_model_n_stages(self.model)))
        except BaseException:
            self.close()
            raise

    def step(self):
        if not self.model:
            raise RuntimeError("adopted runtime is closed")
        rc = self.nx.cap_model_tick(self.ctx, self.model)
        self._sync()
        if rc:
            raise RuntimeError(f"model-runtime tick rc={rc}")

    def _sync(self):
        if self.ctx:
            for stream in self._streams:
                rc = self.nx.cap_sync(self.ctx, stream)
                if rc:
                    raise RuntimeError(f"model-runtime sync rc={rc}")

    def close(self):
        if self.model:
            try:
                self._sync()
            finally:
                if self.ctx:
                    self.nx.cap_ctx_destroy(self.ctx)
                    self.ctx = None
                self.nx.flashrt_model_close(self.model)
                self.model = None
                self._export = None
                self._owners = ()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
