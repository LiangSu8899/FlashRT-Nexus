"""Gate — real Pi0.5 context/action split through the Nexus action-chunk mode.

This is a production-shape host gate:
  1. FlashRT producer captures Pi0.5 full/decode/context graphs.
  2. The producer exports the context_action model-runtime face.
  3. Pi0.5 native C++ verbs override hot image/action processing.
  4. Nexus adopts the model, fires context through StageDAG, then runs the
     action stage through the action-chunk C ABI.

Acceptance: the mode-emitted action chunk is numerically equal to the normal
cap_model_tick baseline for the same image input and noise seed.

--compat-abi drives the same run through the deprecated
``nexus_rtc_action_chunk_*`` alias layer instead of the current ABI prefix.
"""

from __future__ import annotations

import argparse
import ctypes
import os
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nexus_action_chunk_abi import (  # noqa: E402
    CAP_OK, COMPAT_PREFIX, DEFAULT_PREFIX, NEXUS_AC_ERROR, NEXUS_AC_READY,
    ActionChunkAbi, bind_core)


FLASHRT_DIR = os.environ.get("FLASHRT_DIR")
if not FLASHRT_DIR:
    raise SystemExit("Set FLASHRT_DIR=<path to the FlashRT repo root>")
FLASHRT_BUILD_DIR = os.environ.get("FLASHRT_BUILD_DIR")
NEXUS_LIB = os.environ.get(
    "NEXUS_LIB", os.path.join("build", "libcapsule_nexus_flashrt.so"))
PI05_LIB = os.environ.get(
    "PI05_LIB", os.path.join(
        FLASHRT_BUILD_DIR or os.path.join(FLASHRT_DIR, "cpp/build-container"),
        "libflashrt_cpp_pi05_c.so"))

for sub in ("", "exec/build-container", "runtime/build-container",
            "exec/build", "runtime/build"):
    p = os.path.join(FLASHRT_DIR, sub)
    if p not in sys.path:
        sys.path.insert(0, p)
if FLASHRT_BUILD_DIR:
    for sub in ("exec", "runtime"):
        p = os.path.join(FLASHRT_BUILD_DIR, sub)
        if p in sys.path:
            sys.path.remove(p)
        sys.path.insert(0, p)

import flash_rt  # noqa: E402
from flash_rt.core.utils.actions import LIBERO_ACTION_DIM  # noqa: E402
from flash_rt.subgraphs.pi05.context_action import enable as enable_context_action  # noqa: E402


FRT_RT_PIXEL_RGB8 = 0
FRT_RT_DTYPE_F32 = 1
FRT_RT_DTYPE_F16 = 2
FRT_RT_DTYPE_BF16 = 3
FRT_PI05_DTYPE_BFLOAT16 = 1
FRT_PI05_DTYPE_FLOAT16 = 2
FRT_PI05_DTYPE_FLOAT32 = 3


class FrtImageView(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("pixel_format", ctypes.c_uint32),
        ("data", ctypes.c_void_p),
        ("bytes", ctypes.c_uint64),
        ("width", ctypes.c_int32),
        ("height", ctypes.c_int32),
        ("stride_bytes", ctypes.c_int32),
        ("reserved", ctypes.c_uint32),
        ("timestamp_ns", ctypes.c_uint64),
    ]


class Pi05RuntimeConfig(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("num_views", ctypes.c_int),
        ("chunk", ctypes.c_int),
        ("model_action_dim", ctypes.c_int),
        ("robot_action_dim", ctypes.c_int),
        ("action_mean", ctypes.POINTER(ctypes.c_float)),
        ("n_action_mean", ctypes.c_uint64),
        ("action_stddev", ctypes.POINTER(ctypes.c_float)),
        ("n_action_stddev", ctypes.c_uint64),
        ("graph_name", ctypes.c_char_p),
        ("image_buffer_name", ctypes.c_char_p),
        ("action_buffer_name", ctypes.c_char_p),
        ("image_dtype", ctypes.c_int),
        ("action_dtype", ctypes.c_int),
    ]


RetainReleaseFn = ctypes.CFUNCTYPE(None, ctypes.c_void_p)


class FrtModelRuntimeV1(ctypes.Structure):
    _fields_ = [
        ("abi_version", ctypes.c_uint32),
        ("struct_size", ctypes.c_uint32),
        ("exp", ctypes.c_void_p),
        ("ports", ctypes.c_void_p),
        ("n_ports", ctypes.c_uint64),
        ("stages", ctypes.c_void_p),
        ("n_stages", ctypes.c_uint64),
        ("self", ctypes.c_void_p),
        ("verbs", ctypes.c_byte * 48),
        ("owner", ctypes.c_void_p),
        ("retain", RetainReleaseFn),
        ("release", RetainReleaseFn),
    ]


def bind_pi05(lib):
    lib.frt_pi05_model_runtime_create_over.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(Pi05RuntimeConfig),
        ctypes.POINTER(ctypes.c_void_p),
    ]
    lib.frt_pi05_model_runtime_create_over.restype = ctypes.c_int


def make_images(num_views: int, seed: int) -> list[np.ndarray]:
    rng = np.random.default_rng(seed)
    return [
        np.ascontiguousarray(
            rng.integers(0, 256, size=(224, 224, 3), dtype=np.uint8))
        for _ in range(num_views)
    ]


def make_views(images: list[np.ndarray]) -> ctypes.Array:
    views = (FrtImageView * len(images))()
    for i, im in enumerate(images):
        views[i].struct_size = ctypes.sizeof(FrtImageView)
        views[i].pixel_format = FRT_RT_PIXEL_RGB8
        views[i].data = ctypes.c_void_p(im.ctypes.data)
        views[i].bytes = im.nbytes
        views[i].width = int(im.shape[1])
        views[i].height = int(im.shape[0])
        views[i].stride_bytes = int(im.strides[0])
    return views


def action_affine(norm_stats) -> tuple[np.ndarray, np.ndarray]:
    q01 = np.asarray(norm_stats["actions"]["q01"], dtype=np.float32)
    q99 = np.asarray(norm_stats["actions"]["q99"], dtype=np.float32)
    scale = (q99 - q01 + 1e-6) / 2.0
    mean = q01 + scale
    return np.ascontiguousarray(mean), np.ascontiguousarray(scale)


def dtype_from_model_runtime(mr) -> int:
    dtype = None
    for port in mr.ports():
        if port.get("name") == "noise":
            dtype = port.get("dtype")
            break
    if dtype is None:
        raise RuntimeError("model runtime does not declare a noise port dtype")

    text = str(dtype).lower()
    if dtype == FRT_RT_DTYPE_BF16 or text in ("bf16", "bfloat16"):
        return FRT_PI05_DTYPE_BFLOAT16
    if dtype == FRT_RT_DTYPE_F16 or text in ("f16", "float16", "fp16"):
        return FRT_PI05_DTYPE_FLOAT16
    if dtype == FRT_RT_DTYPE_F32 or text in ("f32", "float32", "fp32"):
        return FRT_PI05_DTYPE_FLOAT32
    raise RuntimeError(f"unsupported Pi05 runtime dtype: {dtype!r}")


def model_norm_stats(pl, pipe):
    stats = getattr(pl, "norm_stats", None)
    if stats is None:
        stats = getattr(pipe, "norm_stats", None)
    if stats is None:
        raise RuntimeError("producer does not expose Pi05 action norm_stats")
    return stats


def make_noise_bytes(nbytes: int, dtype_id: int, seed: int) -> bytes:
    rng = np.random.default_rng(seed)
    values = rng.standard_normal(nbytes // 2).astype(np.float32)
    if dtype_id == FRT_PI05_DTYPE_FLOAT16:
        return values.astype(np.float16).tobytes()
    if dtype_id == FRT_PI05_DTYPE_BFLOAT16:
        import ml_dtypes
        return values.astype(ml_dtypes.bfloat16).tobytes()
    return rng.standard_normal(nbytes // 4).astype(np.float32).tobytes()


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--checkpoint", required=True)
    ap.add_argument("--num-views", type=int, default=3)
    ap.add_argument("--steps", type=int, default=10)
    ap.add_argument("--prompt", default="pick up the red block")
    ap.add_argument("--fp8", action="store_true")
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--bench-iters", type=int, default=0)
    ap.add_argument("--compat-abi", action="store_true",
                    help="drive the deprecated nexus_rtc_action_chunk_* aliases")
    args = ap.parse_args()

    nx = ctypes.CDLL(NEXUS_LIB)
    bind_core(nx)
    prefix = COMPAT_PREFIX if args.compat_abi else DEFAULT_PREFIX
    ac = ActionChunkAbi(nx, prefix)
    pi05 = ctypes.CDLL(PI05_LIB)
    bind_pi05(pi05)

    images = make_images(args.num_views, args.seed)
    views = make_views(images)
    model = flash_rt.load_model(
        args.checkpoint, framework="torch", config="pi05", hardware="auto",
        num_views=args.num_views, num_steps=args.steps, cache_frames=1,
        use_fp8=bool(args.fp8), use_fp16=not args.fp8)
    enable_context_action(model)
    model.predict(images, prompt=args.prompt)
    pipe = model._pipe
    pl = pipe.pipeline

    mr = pl.export_model_runtime(
        identity={"gate": "nexus_pi05_action_chunk", "plan": "context_action"},
        stage_plan="context_action",
        io="native",
    )
    if len(mr.stages()) != 2:
        raise RuntimeError(f"expected 2 stages, got {mr.stages()}")

    dtype_id = dtype_from_model_runtime(mr)
    mean, stddev = action_affine(model_norm_stats(pl, pipe))
    cfg = Pi05RuntimeConfig()
    cfg.struct_size = ctypes.sizeof(cfg)
    cfg.num_views = args.num_views
    cfg.chunk = int(pl.chunk_size)
    cfg.model_action_dim = 32
    cfg.robot_action_dim = LIBERO_ACTION_DIM
    cfg.action_mean = mean.ctypes.data_as(ctypes.POINTER(ctypes.c_float))
    cfg.n_action_mean = mean.size
    cfg.action_stddev = stddev.ctypes.data_as(ctypes.POINTER(ctypes.c_float))
    cfg.n_action_stddev = stddev.size
    cfg.image_buffer_name = b"observation_images_normalized"
    cfg.action_buffer_name = b"diffusion_noise"
    cfg.image_dtype = dtype_id
    cfg.action_dtype = dtype_id

    over_ptr = ctypes.c_void_p()
    rc = pi05.frt_pi05_model_runtime_create_over(
        ctypes.c_void_p(mr.ptr), ctypes.byref(cfg), ctypes.byref(over_ptr))
    if rc != 0:
        raise RuntimeError(f"frt_pi05_model_runtime_create_over rc={rc}")
    over = ctypes.cast(over_ptr, ctypes.POINTER(FrtModelRuntimeV1)).contents

    cm = ctypes.c_void_p()
    ctx = ctypes.c_void_p()
    dag = ctypes.c_void_p()
    mode = ctypes.c_void_p()
    try:
        if nx.flashrt_adopt_model_runtime(over_ptr, ctypes.byref(cm)) != CAP_OK:
            raise RuntimeError("flashrt_adopt_model_runtime failed")
        ctx = ctypes.c_void_p(nx.cap_ctx_create(nx.cap_model_backend(cm)))
        if not ctx:
            raise RuntimeError("cap_ctx_create failed")

        p_images = nx.cap_model_find_port(cm, b"images")
        p_noise = nx.cap_model_find_port(cm, b"noise")
        p_actions = nx.cap_model_find_port(cm, b"actions")
        if (p_images, p_noise, p_actions) != (0, 1, 2):
            raise RuntimeError(f"unexpected ports: {p_images},{p_noise},{p_actions}")
        noise_buf = nx.cap_model_port_buffer(cm, p_noise)
        noise_bytes = int(nx.cap_model_port_bytes(cm, p_noise))
        noise = make_noise_bytes(noise_bytes, dtype_id, args.seed + 1000)
        noise_c = ctypes.create_string_buffer(noise, noise_bytes)
        action_bytes = LIBERO_ACTION_DIM * 4

        def set_inputs():
            if nx.cap_model_set_input(
                    cm, p_images, ctypes.cast(views, ctypes.c_void_p),
                    ctypes.sizeof(views), -1) != CAP_OK:
                raise RuntimeError("cap_model_set_input(images) failed")
            stream = nx.cap_model_stage_stream(cm, 0)
            if nx.cap_swap(ctx, noise_buf, noise_c, noise_bytes, stream) != CAP_OK:
                raise RuntimeError("cap_swap(noise) failed")

        def get_actions() -> np.ndarray:
            out = np.empty((int(pl.chunk_size), LIBERO_ACTION_DIM),
                           dtype=np.float32)
            written = ctypes.c_uint64(0)
            rc = nx.cap_model_get_output(
                cm, p_actions, ctypes.c_void_p(out.ctypes.data), out.nbytes,
                ctypes.byref(written), -1)
            if rc != CAP_OK or int(written.value) != out.nbytes:
                raise RuntimeError(f"cap_model_get_output rc={rc} written={written.value}")
            return out

        def wait_chunk_ready() -> tuple[int, float, int]:
            last_state = None
            polls = 0
            t0 = time.perf_counter()
            deadline = t0 + 10.0
            while time.perf_counter() < deadline:
                state = ac.poll(mode)
                polls += 1
                last_state = state
                if state == NEXUS_AC_READY:
                    return polls, (time.perf_counter() - t0) * 1000.0, state
                if state == NEXUS_AC_ERROR:
                    raise RuntimeError("action-chunk poll returned error")
                time.sleep(0.0001)
            raise RuntimeError(
                f"action chunk did not become ready, last_state={last_state}")

        def emit_chunk() -> np.ndarray:
            emitted = np.empty_like(baseline)
            written = ctypes.c_uint64(0)
            for i in range(int(pl.chunk_size)):
                rc = ac.next_action(
                    mode, ctypes.c_void_p(emitted[i].ctypes.data), action_bytes,
                    ctypes.byref(written))
                if rc != NEXUS_AC_READY or int(written.value) != action_bytes:
                    raise RuntimeError(
                        f"next_action[{i}] rc={rc} written={written.value}")
            return emitted

        set_inputs()
        if nx.cap_model_tick(ctx, cm) != CAP_OK:
            raise RuntimeError("cap_model_tick baseline failed")
        nx.cap_sync(ctx, nx.cap_model_stage_stream(cm, 1))
        baseline = get_actions()

        set_inputs()
        if nx.nexus_stage_dag_create(ctx, cm, ctypes.byref(dag)) != CAP_OK:
            raise RuntimeError("nexus_stage_dag_create failed")
        if nx.nexus_stage_dag_fire(dag, 0) != CAP_OK:
            raise RuntimeError("context fire failed")
        if ac.create_for_output_port(
                dag, 1, p_actions, 4, 2, 0, 100000,
                ctypes.byref(mode)) != CAP_OK:
            raise RuntimeError(f"{ac.prefix}_create_for_output_port failed")
        if ac.request(mode) != CAP_OK:
            raise RuntimeError("action-chunk request failed")
        polls, ready_ms, _ = wait_chunk_ready()
        emitted = emit_chunk()

        max_abs = float(np.max(np.abs(baseline - emitted)))
        ok = bool(np.allclose(baseline, emitted, rtol=1e-4, atol=1e-3))
        print("\n===== NEXUS PI0.5 ACTION-CHUNK GATE =====")
        print(f"abi prefix        : {ac.prefix}")
        print(f"fingerprint       : 0x{mr.fingerprint:016x}")
        print(f"stages            : {mr.stages()}")
        print(f"chunk/actions     : {pl.chunk_size} x {LIBERO_ACTION_DIM}")
        print(f"completed chunks  : {ac.completed(mode)}")
        print(f"emitted actions   : {ac.emitted(mode)}")
        print(f"first ready       : polls={polls} wall_ms={ready_ms:.3f} "
              f"ready_ticks={ac.last_ready_ticks(mode)}")
        print(f"baseline vs mode  : {ok} max_abs={max_abs:.6g}")
        if not ok:
            raise SystemExit("FAILED: mode-emitted actions differ")

        if args.bench_iters > 0:
            sync_ms = []
            request_ms = []
            ready_lat_ms = []
            poll_counts = []
            for _ in range(args.bench_iters):
                t0 = time.perf_counter()
                set_inputs()
                if nx.cap_model_tick(ctx, cm) != CAP_OK:
                    raise RuntimeError("cap_model_tick bench failed")
                nx.cap_sync(ctx, nx.cap_model_stage_stream(cm, 1))
                _ = get_actions()
                sync_ms.append((time.perf_counter() - t0) * 1000.0)

                set_inputs()
                nx.nexus_stage_dag_query(dag, 0)
                if nx.nexus_stage_dag_fire(dag, 0) != CAP_OK:
                    if nx.nexus_stage_dag_sync(dag, 0) != CAP_OK:
                        raise RuntimeError("context sync bench failed")
                    if nx.nexus_stage_dag_fire(dag, 0) != CAP_OK:
                        raise RuntimeError("context fire bench failed")
                t_req = time.perf_counter()
                if ac.request(mode) != CAP_OK:
                    raise RuntimeError("action-chunk request bench failed")
                request_ms.append((time.perf_counter() - t_req) * 1000.0)
                polls, wall_ms, _ = wait_chunk_ready()
                ready_lat_ms.append(wall_ms)
                poll_counts.append(polls)
                _ = emit_chunk()

            def p50(xs):
                return float(np.percentile(np.asarray(xs, dtype=np.float64), 50))

            total_ready = int(ac.total_ready_ticks(mode))
            completed = int(ac.completed(mode))
            avg_ready_ticks = total_ready / completed if completed else 0.0
            print("\n----- ASYNC BENCH -----")
            print(f"sync tick p50 ms : {p50(sync_ms):.3f}")
            print(f"request p50 ms   : {p50(request_ms):.3f}")
            print(f"ready p50 ms     : {p50(ready_lat_ms):.3f}")
            print(f"polls p50        : {p50(poll_counts):.1f}")
            print(f"ready ticks      : last={ac.last_ready_ticks(mode)} "
                  f"max={ac.max_ready_ticks(mode)} "
                  f"avg={avg_ready_ticks:.2f}")
            print(f"fallbacks/late   : {ac.fallbacks(mode)} / "
                  f"{ac.late_chunks(mode)}")
        print("PASS - Nexus action-chunk mode matches cap_model_tick baseline")
    finally:
        if mode:
            ac.destroy(mode)
        if dag:
            nx.nexus_stage_dag_destroy(dag)
        if ctx:
            nx.cap_ctx_destroy(ctx)
        if cm:
            nx.flashrt_model_close(cm)
        over.release(over.owner)
        mr.release()


if __name__ == "__main__":
    main()
