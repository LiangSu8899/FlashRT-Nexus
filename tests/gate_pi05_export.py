"""Gate — a REAL model through the whole seam: Pi0.5 -> export -> Nexus -> capsule.

The full phase-1 path on a real VLA policy:
  1. the FlashRT Pi0.5 pipeline captures its infer graph (unchanged);
  2. `pipeline.export_runtime()` packages it as frt_runtime_export_v1;
  3. Nexus adopts the export and drives snapshot / restore / replay THROUGH
     THE CAPSULE CORE.

Acceptance (same invariant as FlashRT's serving/robot_recap/verify_capsule.py,
which validated this boundary via raw exec verbs): restore + replay is
bit-identical, even after the live boundary buffer was overwritten — i.e.
episode reset == capsule restore, now expressed through Nexus.

Run from the Nexus repo root (GPU + built FlashRT exec/runtime + Nexus lib):
  FLASHRT_DIR=/path/to/FlashRT \
  NEXUS_LIB=./build/libcapsule_nexus_flashrt.so \
  python tests/gate_pi05_export.py --checkpoint /path/to/pi05_checkpoint
"""

import argparse
import ctypes
import os
import sys

import numpy as np

FLASHRT_DIR = os.environ.get("FLASHRT_DIR")
if not FLASHRT_DIR:
    raise SystemExit("Set FLASHRT_DIR=<path to the FlashRT repo root>")
NEXUS_LIB = os.environ.get(
    "NEXUS_LIB", os.path.join("build", "libcapsule_nexus_flashrt.so"))

for sub in ("", "exec/build", "runtime/build"):
    p = os.path.join(FLASHRT_DIR, sub)
    if p not in sys.path:
        sys.path.insert(0, p)

# ctypes ABI mirrors + symbol bindings shared with the trivial-graph gate.
from gate_python_producer import (  # noqa: E402
    Binding, CapBoundary, CapStage, bind_symbols)

import flash_rt  # noqa: E402

CHECKS = []


def check(name, ok):
    CHECKS.append((name, bool(ok)))
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--checkpoint", required=True)
    ap.add_argument("--num-views", type=int, default=3)
    ap.add_argument("--steps", type=int, default=10)
    ap.add_argument("--fp8", action="store_true",
                    help="use the FP8 pipeline (default FP16; the capsule "
                         "invariant under test is precision-independent)")
    args = ap.parse_args()

    nx = ctypes.CDLL(NEXUS_LIB)
    bind_symbols(nx)
    cudart = ctypes.CDLL("libcudart.so")
    cudart.cudaMemcpy.restype = ctypes.c_int
    cudart.cudaMemcpy.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                  ctypes.c_size_t, ctypes.c_int]
    cudart.cudaMemset.restype = ctypes.c_int
    cudart.cudaMemset.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_size_t]
    D2H = 2

    # ---- 1. real model: load, capture (the pipeline's normal path) ----
    rng = np.random.RandomState(0)
    images = [rng.randint(0, 256, (224, 224, 3), dtype=np.uint8)
              for _ in range(args.num_views)]
    model = flash_rt.load_model(
        args.checkpoint, framework="torch", config="pi05", hardware="auto",
        num_views=args.num_views, num_steps=args.steps, cache_frames=1,
        use_fp8=args.fp8, use_fp16=not args.fp8)
    model.predict(images, prompt="pick up the red block")   # captures the graph
    pl = model._pipe.pipeline
    check("policy graph captured", getattr(pl, "_graph", None) is not None)

    # ---- 2. producer: package the pipeline as a runtime export ----
    export = pl.export_runtime(identity={"weights": os.path.basename(
        os.path.normpath(args.checkpoint))})
    check("export_runtime built", export.ptr != 0)
    print(f"  fingerprint : {export.fingerprint:#018x}")
    lines = export.identity.splitlines()
    print("  identity    |\n" + "\n".join("    " + ln for ln in lines))

    # ---- 3. consumer: Nexus adopts and drives the capsule verbs ----
    rb = Binding()
    check("nexus adopts the Pi0.5 export",
          nx.flashrt_adopt_runtime_export(ctypes.c_void_p(export.ptr),
                                          ctypes.byref(rb)) == 0)
    cap_ctx = nx.cap_ctx_create(ctypes.byref(rb.backend))
    check("cap_ctx over the Pi0.5 backend", bool(cap_ctx))
    check("fingerprint reaches the core",
          nx.cap_ctx_fingerprint(cap_ctx) == export.fingerprint)

    g_infer = nx.flashrt_runtime_graph(ctypes.byref(rb), b"infer")
    s_main = nx.flashrt_runtime_stream(ctypes.byref(rb), b"main")
    check("infer graph + main stream resolve", bool(g_infer) and s_main >= 0)

    out_buf = pl.bufs["diffusion_noise"]        # the exported rollout boundary
    n = out_buf.nbytes

    def read_actions():
        buf = ctypes.create_string_buffer(n)
        cudart.cudaMemcpy(buf, ctypes.c_void_p(out_buf.ptr.value), n, D2H)
        return buf.raw

    # snapshot the episode boundary through the core
    bnd = CapBoundary(rb.regions, int(rb.n_regions), None, 0)
    cap = nx.cap_snapshot(cap_ctx, ctypes.byref(bnd), 1, s_main)   # HOST tier
    check("episode boundary snapshot", bool(cap))
    nx.cap_sync(cap_ctx, s_main)

    st = CapStage(g_infer, 0, s_main, 0, 1, 1, 0)

    def restore_and_act():
        assert nx.cap_restore(cap_ctx, cap, s_main) == 0
        assert nx.cap_fire(cap_ctx, ctypes.byref(st)) == 0
        nx.cap_sync(cap_ctx, s_main)
        return read_actions()

    a1 = restore_and_act()
    cudart.cudaMemset(ctypes.c_void_p(out_buf.ptr.value), 0xCD, n)  # dirty episode
    a2 = restore_and_act()
    a3 = restore_and_act()
    check("restore+replay reproduces actions bit-exact after dirty", a1 == a2)
    check("capsule reuse is stable (idempotent restore)", a1 == a3)

    nx.cap_capsule_drop(cap_ctx, cap)
    nx.cap_ctx_destroy(cap_ctx)
    nx.flashrt_runtime_binding_fini(ctypes.byref(rb))

    failed = [name for name, ok in CHECKS if not ok]
    print(f"\n{len(CHECKS) - len(failed)}/{len(CHECKS)} checks passed")
    if failed:
        raise SystemExit("FAILED: " + ", ".join(failed))
    print("PASS — Pi0.5 episode reset == capsule restore, driven through Nexus "
          "over the runtime export (bit-identical actions)")


if __name__ == "__main__":
    main()
