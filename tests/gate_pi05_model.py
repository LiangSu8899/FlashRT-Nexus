"""Gate — a REAL model as a standard model runtime: per-tick dynamic input.

The production tick on a real VLA policy, end to end across the seam:
  1. the FlashRT Pi0.5 pipeline captures its infer graph (unchanged);
  2. ``pipeline.export_model_runtime()`` packages it as frt_model_runtime_v1
     (ports: images/noise/encoder_x SWAP windows, actions readback; one
     infer stage);
  3. Nexus adopts it into the standard face (cap_model_runtime) and runs the
     hot loop THROUGH THE CORE: re-seed the noise SWAP port each tick ->
     cap_model_tick -> read actions.

Acceptance:
  - hot input works: same seed -> bit-identical actions, different seed ->
    different actions, across repeated ticks with NO recapture;
  - capsule composes with the tick: snapshot the boundary, dirty, restore,
    tick -> bit-identical (episode reset == capsule restore);
  - producer fingerprint reaches the standard face.

Run from the Nexus repo root (GPU + built FlashRT exec/runtime + Nexus lib):
  FLASHRT_DIR=/path/to/FlashRT \
  NEXUS_LIB=./build/libcapsule_nexus_flashrt.so \
  python tests/gate_pi05_model.py --checkpoint /path/to/pi05_checkpoint
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

for sub in ("", "exec/build-container", "runtime/build-container",
            "exec/build", "runtime/build"):
    p = os.path.join(FLASHRT_DIR, sub)
    if p not in sys.path:
        sys.path.insert(0, p)

from gate_python_producer import CapBoundary  # noqa: E402  (ctypes mirror)

import flash_rt  # noqa: E402

CHECKS = []


def check(name, ok):
    CHECKS.append((name, bool(ok)))
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}")


def standard_buffer(pl, name):
    bufs = getattr(pl, "bufs", None)
    if bufs is not None and name in bufs:
        return bufs[name]
    raise RuntimeError(f"producer does not expose standard buffer {name!r}")


def bind(nx):
    P, U64, U32, I, C = (ctypes.c_void_p, ctypes.c_uint64, ctypes.c_uint32,
                         ctypes.c_int, ctypes.c_char_p)
    sigs = {
        "flashrt_adopt_model_runtime": (I, [P, ctypes.POINTER(P)]),
        "flashrt_model_close": (None, [P]),
        "cap_model_backend": (P, [P]),
        "cap_model_fingerprint": (U64, [P]),
        "cap_model_find_port": (I, [P, C]),
        "cap_model_port_buffer": (P, [P, U64]),
        "cap_model_port_bytes": (U64, [P, U64]),
        "cap_model_port_update": (U32, [P, U64]),
        "cap_model_stage_stream": (I, [P, U64]),
        "cap_model_region_array": (P, [P]),
        "cap_model_region_count": (I, [P]),
        "cap_model_tick": (I, [P, P]),
        "cap_ctx_create": (P, [P]),
        "cap_ctx_destroy": (None, [P]),
        "cap_ctx_fingerprint": (U64, [P]),
        "cap_swap": (I, [P, P, P, ctypes.c_size_t, I]),
        "cap_sync": (I, [P, I]),
        "cap_snapshot": (P, [P, P, I, I]),
        "cap_restore": (I, [P, P, I]),
        "cap_capsule_drop": (None, [P, P]),
    }
    for name, (res, args) in sigs.items():
        fn = getattr(nx, name)
        fn.restype = res
        fn.argtypes = args


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--checkpoint", required=True)
    ap.add_argument("--num-views", type=int, default=3)
    ap.add_argument("--steps", type=int, default=10)
    ap.add_argument("--fp8", action="store_true")
    args = ap.parse_args()

    nx = ctypes.CDLL(NEXUS_LIB)
    bind(nx)
    cudart = ctypes.CDLL("libcudart.so")
    cudart.cudaMemcpy.restype = ctypes.c_int
    cudart.cudaMemcpy.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                  ctypes.c_size_t, ctypes.c_int]
    D2H = 2

    # ---- real model: load, capture, export the model runtime ----
    rng = np.random.RandomState(0)
    images = [rng.randint(0, 256, (224, 224, 3), dtype=np.uint8)
              for _ in range(args.num_views)]
    model = flash_rt.load_model(
        args.checkpoint, framework="torch", config="pi05", hardware="auto",
        num_views=args.num_views, num_steps=args.steps, cache_frames=1,
        use_fp8=args.fp8, use_fp16=not args.fp8)
    model.predict(images, prompt="pick up the red block")
    pl = model._pipe.pipeline

    mr = pl.export_model_runtime(identity={"weights": os.path.basename(
        os.path.normpath(args.checkpoint))})
    check("export_model_runtime built", mr.ptr != 0)
    names = [p["name"] for p in mr.ports()]
    check("ports declared", names == ["images", "noise", "encoder_x", "actions"])

    # ---- Nexus adopts the standard face ----
    cm = ctypes.c_void_p()
    check("nexus adopts the Pi0.5 model runtime",
          nx.flashrt_adopt_model_runtime(ctypes.c_void_p(mr.ptr),
                                         ctypes.byref(cm)) == 0)
    ctx = nx.cap_ctx_create(nx.cap_model_backend(cm))
    check("cap_ctx over the model backend", bool(ctx))
    check("fingerprint reaches the standard face",
          nx.cap_model_fingerprint(cm) == mr.fingerprint
          and nx.cap_ctx_fingerprint(ctx) == mr.fingerprint)

    p_noise = nx.cap_model_find_port(cm, b"noise")
    check("noise port resolves as SWAP",
          p_noise >= 0 and nx.cap_model_port_update(cm, p_noise) == 0)
    noise_buf = nx.cap_model_port_buffer(cm, p_noise)
    nbytes = int(nx.cap_model_port_bytes(cm, p_noise))
    stream = nx.cap_model_stage_stream(cm, 0)
    check("noise window wired", bool(noise_buf) and nbytes > 0 and stream >= 0)

    out_ptr = standard_buffer(pl, "diffusion_noise").ptr.value

    def read_actions():
        buf = ctypes.create_string_buffer(nbytes)
        cudart.cudaMemcpy(buf, ctypes.c_void_p(out_ptr), nbytes, D2H)
        return buf.raw

    def tick(seed_bytes):
        seed = ctypes.create_string_buffer(seed_bytes, nbytes)
        assert nx.cap_swap(ctx, noise_buf, seed, nbytes, stream) == 0
        assert nx.cap_model_tick(ctx, cm) == 0
        nx.cap_sync(ctx, stream)
        return read_actions()

    # ---- the production hot loop: per-tick dynamic input, no recapture ----
    import ml_dtypes

    def gaussian_seed(seed):
        r = np.random.RandomState(seed)
        return (r.standard_normal(nbytes // 2).astype(np.float32)
                .astype(ml_dtypes.bfloat16).tobytes())

    seed_a = gaussian_seed(1)
    seed_b = gaussian_seed(2)
    a1 = tick(seed_a)
    b1 = tick(seed_b)
    a2 = tick(seed_a)
    check("same seed -> bit-identical actions (hot input works)", a1 == a2)
    check("different seed -> different actions (input actually flows)",
          a1 != b1)

    # ---- capsule composes with the tick ----
    tick(seed_a)
    bnd = CapBoundary(
        ctypes.cast(nx.cap_model_region_array(cm),
                    CapBoundary._fields_[0][1]),
        nx.cap_model_region_count(cm), None, 0)
    cap = nx.cap_snapshot(ctx, ctypes.byref(bnd), 1, stream)   # HOST tier
    check("boundary snapshot", bool(cap))
    nx.cap_sync(ctx, stream)
    tick(seed_b)                                # a different episode
    assert nx.cap_restore(ctx, cap, stream) == 0
    nx.cap_sync(ctx, stream)
    check("episode reset == capsule restore (bit-identical boundary)",
          read_actions() == a1)

    nx.cap_capsule_drop(ctx, cap)
    nx.cap_ctx_destroy(ctx)
    nx.flashrt_model_close(cm)
    mr.release()

    failed = [n for n, ok in CHECKS if not ok]
    print(f"\n{len(CHECKS) - len(failed)}/{len(CHECKS)} checks passed")
    if failed:
        raise SystemExit("FAILED: " + ", ".join(failed))
    print("PASS — Pi0.5 as a standard model runtime: per-tick dynamic input "
          "through Nexus, no recapture, capsule-composable")


if __name__ == "__main__":
    main()
