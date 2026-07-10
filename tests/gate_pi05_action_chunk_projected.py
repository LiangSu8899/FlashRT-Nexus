"""Gate — Pi0.5 through the projected-state prepare policy (host transport).

Drives the two-phase request on the real model: begin_request computes the
projected state from the executing chunk; the host injects it through the
Pi0.5 state-prompt path (state_prompt_mode="fixed": tokenize + embed upload,
no recapture); commit_request fires. Checks:

  1. The projected state equals the reference implementation
     (flash_rt.runtime.vlash project_state) fed the same chunk, measured
     state, and start index (f32, <= 1 ulp: the reference sums with numpy's
     pairwise reduction).
  2. Given the identical injected state, the chunk the mode ingests equals
     a cap_model_tick baseline bit-exactly — transport and seating add
     nothing.
  3. Seating: landing at d == k consumes from index 0; landing at d > k
     skips the stale prefix.

This gate retains the Python-producer host-transport lane. The native C++
producer's direct STATE/STAGED lane is gated separately by
gate_pi05_native_projected_port.py.
"""

from __future__ import annotations

import argparse
import ctypes
import importlib.util
import os
import sys
import types

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nexus_action_chunk_abi import (  # noqa: E402
    CAP_OK, NEXUS_AC_DTYPE_F32, NEXUS_AC_PREPARE_PROJECTED_STATE,
    NEXUS_AC_READY, ActionChunkAbi, NexusActionChunkConfig, bind_core)
from gate_pi05_action_chunk import (  # noqa: E402
    FLASHRT_DIR, NEXUS_LIB, PI05_LIB, FrtModelRuntimeV1, Pi05RuntimeConfig,
    action_affine, bind_pi05, dtype_from_model_runtime, make_images,
    make_noise_bytes, make_views, model_norm_stats)

import flash_rt  # noqa: E402
from flash_rt.core.utils.actions import LIBERO_ACTION_DIM  # noqa: E402
from flash_rt.subgraphs.pi05.context_action import (  # noqa: E402
    enable as enable_context_action)

REPR_DELTA_CUMULATIVE = 1


def load_oracle_module(runtime_dir: str):
    """Load vlash.py by path, isolated from whichever flash_rt package the
    producer put on sys.path."""
    package = types.ModuleType("frt_oracle")
    package.__path__ = [runtime_dir]
    sys.modules["frt_oracle"] = package
    module = None
    for name in ("rtc", "vlash"):
        spec = importlib.util.spec_from_file_location(
            f"frt_oracle.{name}", os.path.join(runtime_dir, f"{name}.py"))
        module = importlib.util.module_from_spec(spec)
        sys.modules[f"frt_oracle.{name}"] = module
        spec.loader.exec_module(module)
    return module


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--checkpoint", required=True)
    ap.add_argument("--num-views", type=int, default=3)
    ap.add_argument("--steps", type=int, default=10)
    ap.add_argument("--prompt", default="pick up the red block")
    ap.add_argument("--fp8", action="store_true")
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--lookahead", type=int, default=3)
    ap.add_argument("--consume-per-round", type=int, default=5)
    ap.add_argument("--state-dim", type=int, default=LIBERO_ACTION_DIM)
    ap.add_argument("--oracle-dir",
                    default=os.environ.get(
                        "FRT_ORACLE_DIR",
                        os.path.join(FLASHRT_DIR, "flash_rt", "runtime")),
                    help="directory holding vlash.py")
    args = ap.parse_args()

    oracle_mod = load_oracle_module(args.oracle_dir)
    AsyncVLAShRunner = oracle_mod.AsyncVLAShRunner
    VLAShConfig = oracle_mod.VLAShConfig

    class _NoAdapter:
        def infer_actions(self, observation):
            raise RuntimeError("the oracle never runs inference")

    oracle = AsyncVLAShRunner(
        _NoAdapter(),
        VLAShConfig(action_hz=1.0, lookahead_steps=args.lookahead))

    nx = ctypes.CDLL(NEXUS_LIB)
    bind_core(nx)
    ac = ActionChunkAbi(nx)
    pi05 = ctypes.CDLL(PI05_LIB)
    bind_pi05(pi05)

    rng = np.random.default_rng(args.seed + 500)
    state0 = np.zeros(args.state_dim, dtype=np.float32)

    images = make_images(args.num_views, args.seed)
    views = make_views(images)
    model = flash_rt.load_model(
        args.checkpoint, framework="torch", config="pi05", hardware="auto",
        num_views=args.num_views, num_steps=args.steps, cache_frames=1,
        use_fp8=bool(args.fp8), use_fp16=not args.fp8,
        state_prompt_mode="fixed")
    enable_context_action(model)
    model.predict(images, prompt=args.prompt, state=state0)
    pipe = model._pipe
    pl = pipe.pipeline

    mr = pl.export_model_runtime(
        identity={"gate": "nexus_pi05_action_chunk_projected",
                  "plan": "context_action"},
        stage_plan="context_action",
        io="native",
    )
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

    horizon = int(pl.chunk_size)
    action_bytes = LIBERO_ACTION_DIM * 4

    cm = ctypes.c_void_p()
    ctx = ctypes.c_void_p()
    dag = ctypes.c_void_p()
    mode = ctypes.c_void_p()
    try:
        if nx.flashrt_adopt_model_runtime(over_ptr,
                                          ctypes.byref(cm)) != CAP_OK:
            raise RuntimeError("flashrt_adopt_model_runtime failed")
        ctx = ctypes.c_void_p(nx.cap_ctx_create(nx.cap_model_backend(cm)))
        if not ctx:
            raise RuntimeError("cap_ctx_create failed")
        p_images = nx.cap_model_find_port(cm, b"images")
        p_noise = nx.cap_model_find_port(cm, b"noise")
        p_actions = nx.cap_model_find_port(cm, b"actions")
        noise_buf = nx.cap_model_port_buffer(cm, p_noise)
        noise_bytes = int(nx.cap_model_port_bytes(cm, p_noise))
        noise = make_noise_bytes(noise_bytes, dtype_id, args.seed + 1000)
        noise_c = ctypes.create_string_buffer(noise, noise_bytes)

        def set_inputs():
            if nx.cap_model_set_input(
                    cm, p_images, ctypes.cast(views, ctypes.c_void_p),
                    ctypes.sizeof(views), -1) != CAP_OK:
                raise RuntimeError("cap_model_set_input(images) failed")
            stream = nx.cap_model_stage_stream(cm, 0)
            if nx.cap_swap(ctx, noise_buf, noise_c, noise_bytes,
                           stream) != CAP_OK:
                raise RuntimeError("cap_swap(noise) failed")

        def get_actions() -> np.ndarray:
            out = np.empty((horizon, LIBERO_ACTION_DIM), dtype=np.float32)
            written = ctypes.c_uint64(0)
            rc = nx.cap_model_get_output(
                cm, p_actions, ctypes.c_void_p(out.ctypes.data), out.nbytes,
                ctypes.byref(written), -1)
            if rc != CAP_OK or int(written.value) != out.nbytes:
                raise RuntimeError(f"cap_model_get_output rc={rc}")
            return out

        def fire_context():
            nx.nexus_stage_dag_query(dag, 0)
            if nx.nexus_stage_dag_fire(dag, 0) != CAP_OK:
                if nx.nexus_stage_dag_sync(dag, 0) != CAP_OK:
                    raise RuntimeError("context sync failed")
                if nx.nexus_stage_dag_fire(dag, 0) != CAP_OK:
                    raise RuntimeError("context fire failed")

        if nx.nexus_stage_dag_create(ctx, cm, ctypes.byref(dag)) != CAP_OK:
            raise RuntimeError("nexus_stage_dag_create failed")

        mode_cfg = NexusActionChunkConfig()
        mode_cfg.struct_size = ctypes.sizeof(mode_cfg)
        mode_cfg.action_stage = 1
        mode_cfg.output_port = p_actions
        mode_cfg.chunk_length = horizon
        mode_cfg.action_bytes = action_bytes
        mode_cfg.ring_slots = 2
        mode_cfg.execute_horizon = 0
        mode_cfg.poll_budget = -1
        mode_cfg.deadline_steps = -1
        mode_cfg.prepare_policy = NEXUS_AC_PREPARE_PROJECTED_STATE
        mode_cfg.scalar_dtype = NEXUS_AC_DTYPE_F32
        mode_cfg.action_representation = REPR_DELTA_CUMULATIVE
        mode_cfg.state_dim = args.state_dim
        mode_cfg.lookahead_steps = args.lookahead
        if ac.create(dag, ctypes.byref(mode_cfg),
                     ctypes.byref(mode)) != CAP_OK:
            raise RuntimeError("projected mode create failed")

        proj_exact = raw_exact = seat_exact = True
        prev_raw = None
        emitted = np.empty((1, LIBERO_ACTION_DIM), dtype=np.float32)
        written = ctypes.c_uint64(0)
        proj_buf = np.empty(args.state_dim, dtype=np.float32)
        dims = ctypes.c_uint32(0)

        for round_i in range(args.rounds):
            measured = (state0 if round_i == 0 else
                        rng.uniform(-0.2, 0.2,
                                    args.state_dim).astype(np.float32))
            start_index = int(ac.active_index(mode))
            if ac.set_state(
                    mode,
                    measured.ctypes.data_as(
                        ctypes.POINTER(ctypes.c_float)),
                    args.state_dim) != CAP_OK:
                raise RuntimeError("set_state failed")
            if ac.begin_request(mode) != CAP_OK:
                raise RuntimeError("begin_request failed")
            if ac.projected_state(
                    mode,
                    proj_buf.ctypes.data_as(ctypes.POINTER(ctypes.c_float)),
                    args.state_dim, ctypes.byref(dims)) != CAP_OK:
                raise RuntimeError("projected_state failed")
            k_actual = int(ac.projected_count(mode))

            if prev_raw is not None:
                expected, expected_count = oracle.project_state(
                    measured, prev_raw[:, :args.state_dim],
                    start_index=start_index)
                expected = np.asarray(expected, dtype=np.float32)
                ulp = np.maximum(np.abs(np.spacing(expected)), 0)
                proj_exact = proj_exact and bool(
                    np.all(np.abs(proj_buf - expected) <= ulp)) and \
                    k_actual == int(expected_count)
            else:
                proj_exact = proj_exact and bool(
                    np.array_equal(proj_buf, measured)) and k_actual == 0

            # HOST transport: state -> prompt tokens -> embeds (no recapture).
            pipe.set_prompt(args.prompt, state=proj_buf.copy())
            set_inputs()
            fire_context()
            if ac.commit_request(mode) != CAP_OK:
                raise RuntimeError("commit_request failed")
            extra = 2 if round_i == args.rounds - 1 else 0
            for _ in range(k_actual + extra):
                if ac.advance_step(mode) != CAP_OK:
                    raise RuntimeError("advance_step failed")
            state = ac.sync_next_chunk(mode)
            if state != NEXUS_AC_READY:
                raise RuntimeError(f"sync_next_chunk state={state}")
            seat_exact = seat_exact and \
                int(ac.active_index(mode)) == extra and \
                int(ac.seated_waiting(mode)) == 0

            raw = get_actions()
            set_inputs()
            if nx.cap_model_tick(ctx, cm) != CAP_OK:
                raise RuntimeError("cap_model_tick baseline failed")
            nx.cap_sync(ctx, nx.cap_model_stage_stream(cm, 1))
            baseline = get_actions()
            raw_exact = raw_exact and bool(np.array_equal(raw, baseline))
            prev_raw = raw

            for _ in range(args.consume_per_round):
                rc = ac.next_action(
                    mode, ctypes.c_void_p(emitted.ctypes.data), action_bytes,
                    ctypes.byref(written))
                if rc != NEXUS_AC_READY:
                    raise RuntimeError(f"next_action rc={rc}")

        print("\n===== NEXUS PI0.5 ACTION-CHUNK PROJECTED-STATE GATE =====")
        print(f"fingerprint       : 0x{mr.fingerprint:016x}")
        print(f"chunk/actions     : {horizon} x {LIBERO_ACTION_DIM}")
        print(f"rounds            : {args.rounds} "
              f"(lookahead={args.lookahead}, host transport)")
        print(f"completed chunks  : {ac.completed(mode)}")
        print(f"projection==oracle: {proj_exact}")
        print(f"raw == baseline   : {raw_exact}")
        print(f"seating (d vs k)  : {seat_exact}")
        if not (proj_exact and raw_exact and seat_exact):
            raise SystemExit("FAILED: projected-state gate mismatch")
        print("PASS - projected-state prepare matches the reference and "
              "the baseline on real Pi0.5")
    finally:
        oracle.close()
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
