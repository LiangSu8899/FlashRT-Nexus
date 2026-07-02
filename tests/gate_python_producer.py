"""Gate — the phase-1 deployment shape, end to end across the language seam.

A resident Python producer captures a trivial graph and packages it with
FlashRT's REAL export builder (flash_rt.runtime.export); this process then
hands the frt_runtime_export_v1 pointer to Nexus (libcapsule_nexus_flashrt.so
via ctypes) and drives fire / snapshot / restore THROUGH THE CAPSULE CORE.

What only this gate can prove (the C++ tests fabricate the export by hand):
  - the Python-built struct is byte-compatible with the C consumer;
  - the producer fingerprint (computed by the C builder) reaches cap_ctx;
  - retain/release across the seam is GIL-safe: binding fini triggers the
    export release from native code while ctypes has dropped the GIL.

Run from the Nexus repo root (no hardcoded paths — both roots via env):
  FLASHRT_DIR=/path/to/FlashRT \
  NEXUS_LIB=./build/libcapsule_nexus_flashrt.so \
  python tests/gate_python_producer.py

Requires: a CUDA GPU, FlashRT's exec/ and runtime/ built, Nexus built with
-DCAPSULE_BUILD_FLASHRT_BACKEND=ON.
"""

import ctypes
import gc
import os
import sys
import weakref

FLASHRT_DIR = os.environ.get("FLASHRT_DIR")
if not FLASHRT_DIR:
    raise SystemExit("Set FLASHRT_DIR=<path to the FlashRT repo root>")
NEXUS_LIB = os.environ.get(
    "NEXUS_LIB", os.path.join("build", "libcapsule_nexus_flashrt.so"))

for sub in ("", "exec/build", "runtime/build"):
    p = os.path.join(FLASHRT_DIR, sub)
    if p not in sys.path:
        sys.path.insert(0, p)

import _flashrt_exec as ex  # noqa: E402
from flash_rt.runtime.export import (  # noqa: E402
    BufferSpec, GraphSpec, RegionSpec, StreamSpec, build_export)

CHECKS = []


def check(name, ok):
    CHECKS.append((name, bool(ok)))
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}")


# ---- ctypes mirrors of the capsule C ABI (capsule/capsule.h) ----
class CapRegion(ctypes.Structure):
    _fields_ = [("buf", ctypes.c_void_p), ("off", ctypes.c_size_t),
                ("bytes", ctypes.c_size_t)]


class CapBoundary(ctypes.Structure):
    _fields_ = [("regions", ctypes.POINTER(CapRegion)), ("n_regions", ctypes.c_int),
                ("meta", ctypes.c_void_p), ("meta_len", ctypes.c_size_t)]


class CapRegionView(ctypes.Structure):
    _fields_ = [("ptr", ctypes.c_void_p), ("bytes", ctypes.c_size_t)]


class CapStage(ctypes.Structure):
    _fields_ = [("graph", ctypes.c_void_p), ("key", ctypes.c_uint64),
                ("stream", ctypes.c_int), ("priority", ctypes.c_int),
                ("cadence_num", ctypes.c_int), ("cadence_den", ctypes.c_int),
                ("trigger", ctypes.c_int)]


# cap_backend: abi_version/struct_size, self, then 19 function pointers
# (8 buffer + 3 graph + 7 stream/event + fingerprint). Verified after adopt
# against the struct_size the backend itself reports.
class CapBackend(ctypes.Structure):
    _fields_ = ([("abi_version", ctypes.c_uint32),
                 ("struct_size", ctypes.c_uint32),
                 ("self_", ctypes.c_void_p)]
                + [(f"fn{i}", ctypes.c_void_p) for i in range(19)])


class Binding(ctypes.Structure):
    _fields_ = [("exp", ctypes.c_void_p), ("backend", CapBackend),
                ("streams", ctypes.POINTER(ctypes.c_int)),
                ("graphs", ctypes.POINTER(ctypes.c_void_p)),
                ("buffers", ctypes.POINTER(ctypes.c_void_p)),
                ("regions", ctypes.POINTER(CapRegion)),
                ("n_streams", ctypes.c_uint64), ("n_graphs", ctypes.c_uint64),
                ("n_buffers", ctypes.c_uint64), ("n_regions", ctypes.c_uint64)]


def bind_symbols(nx):
    nx.flashrt_adopt_runtime_export.restype = ctypes.c_int
    nx.flashrt_adopt_runtime_export.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    nx.flashrt_runtime_binding_fini.argtypes = [ctypes.c_void_p]
    nx.flashrt_runtime_graph.restype = ctypes.c_void_p
    nx.flashrt_runtime_graph.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    nx.flashrt_runtime_stream.restype = ctypes.c_int
    nx.flashrt_runtime_stream.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    nx.cap_ctx_create.restype = ctypes.c_void_p
    nx.cap_ctx_create.argtypes = [ctypes.c_void_p]
    nx.cap_ctx_destroy.argtypes = [ctypes.c_void_p]
    nx.cap_ctx_fingerprint.restype = ctypes.c_uint64
    nx.cap_ctx_fingerprint.argtypes = [ctypes.c_void_p]
    nx.cap_fire.restype = ctypes.c_int
    nx.cap_fire.argtypes = [ctypes.c_void_p, ctypes.POINTER(CapStage)]
    nx.cap_sync.restype = ctypes.c_int
    nx.cap_sync.argtypes = [ctypes.c_void_p, ctypes.c_int]
    nx.cap_snapshot.restype = ctypes.c_void_p
    nx.cap_snapshot.argtypes = [ctypes.c_void_p, ctypes.POINTER(CapBoundary),
                                ctypes.c_int, ctypes.c_int]
    nx.cap_restore.restype = ctypes.c_int
    nx.cap_restore.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int]
    nx.cap_regions.restype = ctypes.c_int
    nx.cap_regions.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                               ctypes.POINTER(CapRegionView),
                               ctypes.POINTER(ctypes.c_int)]
    nx.cap_capsule_drop.argtypes = [ctypes.c_void_p, ctypes.c_void_p]


def main():
    nx = ctypes.CDLL(NEXUS_LIB)
    bind_symbols(nx)
    cudart = ctypes.CDLL("libcudart.so")
    cudart.cudaMemcpy.restype = ctypes.c_int
    cudart.cudaMemcpy.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                  ctypes.c_size_t, ctypes.c_int]
    H2D, D2H = 1, 2

    # ---- producer: seed src, capture src->dst, package with the real builder ----
    N = 4096
    ctx = ex.Ctx()
    sid = ctx.stream(0)
    src = ctx.buffer("src", N)
    dst = ctx.buffer("dst", N)
    g = ctx.graph("copy", 1)
    g.capture(0, lambda stream: ex.memcpy_async(dst.dptr(), src.dptr(), N, stream))

    pattern = bytes((i * 7 + 3) % 256 for i in range(N))
    seed = ctypes.create_string_buffer(pattern, N)
    check("seed src pattern (H2D)",
          cudart.cudaMemcpy(ctypes.c_void_p(src.dptr()), seed, N, H2D) == 0)

    export = build_export(
        ctx,
        streams=[StreamSpec("main", sid)],
        graphs=[GraphSpec("infer", g, 0, (0,))],
        buffers=[BufferSpec("src", src, "input"),
                 BufferSpec("dst", dst, "output")],
        regions=[RegionSpec("boundary", dst)],
        identity={"model": "gate-trivial", "quant": "none"},
    )
    anchor_ref = weakref.ref(export._anchor)

    # ---- consumer: Nexus adopts the pointer ----
    rb = Binding()
    rc = nx.flashrt_adopt_runtime_export(ctypes.c_void_p(export.ptr),
                                         ctypes.byref(rb))
    check("nexus adopts the Python-built export", rc == 0)
    if rc != 0:
        raise SystemExit(1)
    check("cap_backend mirror layout matches the ABI",
          rb.backend.struct_size == ctypes.sizeof(CapBackend))

    cap_ctx = nx.cap_ctx_create(ctypes.byref(rb.backend))
    check("cap_ctx_create over adopted backend", bool(cap_ctx))
    check("builder fingerprint reaches the core",
          nx.cap_ctx_fingerprint(cap_ctx) == export.fingerprint)

    g_infer = nx.flashrt_runtime_graph(ctypes.byref(rb), b"infer")
    s_main = nx.flashrt_runtime_stream(ctypes.byref(rb), b"main")
    check("name lookups across the seam", bool(g_infer) and s_main >= 0)

    st = CapStage(g_infer, 0, s_main, 0, 1, 1, 0)
    check("cap_fire (native replay of the Python-captured graph)",
          nx.cap_fire(cap_ctx, ctypes.byref(st)) == 0)
    nx.cap_sync(cap_ctx, s_main)

    bnd = CapBoundary(rb.regions, int(rb.n_regions), None, 0)
    cap = nx.cap_snapshot(cap_ctx, ctypes.byref(bnd), 1, s_main)  # HOST tier
    check("snapshot over the exported region", bool(cap))
    nx.cap_sync(cap_ctx, s_main)
    view = CapRegionView()
    nv = ctypes.c_int(1)
    nx.cap_regions(cap_ctx, cap, ctypes.byref(view), ctypes.byref(nv))
    got = ctypes.string_at(view.ptr, N)
    check("replayed output round-trips bit-exact through the capsule",
          got == pattern)

    # dirty the live buffer, restore, read it back
    zero = ctypes.create_string_buffer(N)
    cudart.cudaMemcpy(ctypes.c_void_p(dst.dptr()), zero, N, H2D)
    check("cap_restore", nx.cap_restore(cap_ctx, cap, s_main) == 0)
    nx.cap_sync(cap_ctx, s_main)
    back = ctypes.create_string_buffer(N)
    cudart.cudaMemcpy(back, ctypes.c_void_p(dst.dptr()), N, D2H)
    check("restore is bit-exact on the live buffer", back.raw == pattern)

    # ---- lifetime across the seam (GIL-safe release from native fini) ----
    nx.cap_capsule_drop(cap_ctx, cap)
    nx.cap_ctx_destroy(cap_ctx)
    export._anchor = None
    export.release()                      # producer's reference
    gc.collect()
    check("native retain keeps the anchor alive", anchor_ref() is not None)
    nx.flashrt_runtime_binding_fini(ctypes.byref(rb))  # release from C, no GIL held
    gc.collect()
    check("binding fini releases the anchor (GIL-safe)", anchor_ref() is None)

    failed = [n for n, ok in CHECKS if not ok]
    print(f"\n{len(CHECKS) - len(failed)}/{len(CHECKS)} checks passed")
    if failed:
        raise SystemExit("FAILED: " + ", ".join(failed))
    print("PASS — Python producer -> Nexus consumer, one struct across the seam")


if __name__ == "__main__":
    main()
