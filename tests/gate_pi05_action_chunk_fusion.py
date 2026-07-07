"""Gate — Pi0.5 through the action-chunk temporal-fusion consume policy.

Builds on the plain gate's producer setup (gate_pi05_action_chunk.py) and
checks the fusion policy against the reference semantics on the real model:

  1. Every raw chunk the mode ingests equals the cap_model_tick baseline
     (identical image/noise inputs; raw chunks must stay untouched).
  2. The fused actions the mode emits equal, bit-exactly, what the FlashRT
     reference implementation (rtc_temporal_fusion.TemporalFusionBuffer)
     produces when fed the same raw chunks on the same step grid.
  3. The seated switch index equals the reference switch index.

Fused output is never compared against the model baseline — it is
post-processed by design; the reference is the oracle.
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
    CAP_OK, NEXUS_AC_CONSUME_TEMPORAL_FUSION, NEXUS_AC_DTYPE_F32,
    NEXUS_AC_READY, ActionChunkAbi, NexusActionChunkConfig, bind_core)
from gate_pi05_action_chunk import (  # noqa: E402
    FLASHRT_DIR, NEXUS_LIB, PI05_LIB, FrtModelRuntimeV1, Pi05RuntimeConfig,
    action_affine, bind_pi05, dtype_from_model_runtime, make_images,
    make_noise_bytes, make_views, model_norm_stats)

import flash_rt  # noqa: E402
from flash_rt.core.utils.actions import LIBERO_ACTION_DIM  # noqa: E402
from flash_rt.subgraphs.pi05.context_action import (  # noqa: E402
    enable as enable_context_action)


def load_oracle_module(runtime_dir: str):
    """Load rtc_temporal_fusion.py by path, isolated from whichever
    flash_rt package the producer put on sys.path (the oracle may come
    from a different checkout than the producer)."""
    package = types.ModuleType("frt_oracle")
    package.__path__ = [runtime_dir]
    sys.modules["frt_oracle"] = package
    module = None
    for name in ("rtc", "rtc_temporal_fusion"):
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
    ap.add_argument("--rounds", type=int, default=4)
    ap.add_argument("--elapsed-steps", type=int, default=3,
                    help="controller steps to advance while a fire is pending")
    ap.add_argument("--decay", type=float, default=0.1)
    ap.add_argument("--max-chunks", type=int, default=3)
    ap.add_argument("--oracle-dir",
                    default=os.environ.get(
                        "FRT_ORACLE_DIR",
                        os.path.join(FLASHRT_DIR, "flash_rt", "runtime")),
                    help="directory holding rtc_temporal_fusion.py")
    args = ap.parse_args()

    oracle_mod = load_oracle_module(args.oracle_dir)
    TemporalFusionBuffer = oracle_mod.TemporalFusionBuffer
    TemporalFusionConfig = oracle_mod.TemporalFusionConfig

    nx = ctypes.CDLL(NEXUS_LIB)
    bind_core(nx)
    ac = ActionChunkAbi(nx)
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
        identity={"gate": "nexus_pi05_action_chunk_fusion",
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

        set_inputs()
        if nx.cap_model_tick(ctx, cm) != CAP_OK:
            raise RuntimeError("cap_model_tick baseline failed")
        nx.cap_sync(ctx, nx.cap_model_stage_stream(cm, 1))
        baseline = get_actions()

        if nx.nexus_stage_dag_create(ctx, cm, ctypes.byref(dag)) != CAP_OK:
            raise RuntimeError("nexus_stage_dag_create failed")

        mode_cfg = NexusActionChunkConfig()
        mode_cfg.struct_size = ctypes.sizeof(mode_cfg)
        mode_cfg.action_stage = 1
        mode_cfg.output_port = p_actions
        mode_cfg.chunk_length = horizon
        mode_cfg.action_bytes = action_bytes
        mode_cfg.ring_slots = args.max_chunks + 1
        mode_cfg.execute_horizon = 0
        mode_cfg.poll_budget = -1
        mode_cfg.deadline_steps = -1
        mode_cfg.consume_policy = NEXUS_AC_CONSUME_TEMPORAL_FUSION
        mode_cfg.scalar_dtype = NEXUS_AC_DTYPE_F32
        mode_cfg.fusion_decay = args.decay
        mode_cfg.fusion_max_chunks = args.max_chunks
        if ac.create(dag, ctypes.byref(mode_cfg),
                     ctypes.byref(mode)) != CAP_OK:
            raise RuntimeError("fusion mode create failed")

        oracle = TemporalFusionBuffer(
            TemporalFusionConfig(action_hz=1.0, max_chunks=args.max_chunks,
                                 decay=args.decay, switch_mode="latency",
                                 epoch_s=0.0),
            clock=lambda: 0.0)

        raw_exact = fused_exact = index_exact = True
        for round_i in range(args.rounds):
            set_inputs()
            if round_i == 0:
                if nx.nexus_stage_dag_fire(dag, 0) != CAP_OK:
                    raise RuntimeError("context fire failed")
            else:
                nx.nexus_stage_dag_query(dag, 0)
                if nx.nexus_stage_dag_fire(dag, 0) != CAP_OK:
                    if nx.nexus_stage_dag_sync(dag, 0) != CAP_OK:
                        raise RuntimeError("context sync failed")
                    if nx.nexus_stage_dag_fire(dag, 0) != CAP_OK:
                        raise RuntimeError("context fire failed")

            fire_step = int(ac.action_step(mode))
            if ac.request(mode) != CAP_OK:
                raise RuntimeError("mode request failed")
            for _ in range(args.elapsed_steps):
                if ac.advance_step(mode) != CAP_OK:
                    raise RuntimeError("advance_step failed")
            state = ac.sync_next_chunk(mode)
            if state != NEXUS_AC_READY:
                raise RuntimeError(f"sync_next_chunk state={state}")
            ready_step = int(ac.action_step(mode))

            raw = get_actions()
            raw_exact = raw_exact and bool(np.array_equal(raw, baseline))

            ticket = oracle.begin_prediction(started_at=float(fire_step),
                                             start_step=fire_step)
            fused = oracle.complete_prediction(
                ticket, raw, ready_at=float(ready_step),
                switch_at=float(ready_step))
            expected = np.asarray(fused.actions)
            index = int(ac.active_index(mode))
            index_exact = index_exact and index == int(fused.switch_index)

            emitted = np.empty((horizon - index, LIBERO_ACTION_DIM),
                               dtype=np.float32)
            written = ctypes.c_uint64(0)
            for i in range(horizon - index):
                rc = ac.next_action(
                    mode, ctypes.c_void_p(emitted[i].ctypes.data),
                    action_bytes, ctypes.byref(written))
                if (rc != NEXUS_AC_READY or
                        int(written.value) != action_bytes):
                    raise RuntimeError(f"next_action[{i}] rc={rc}")
            fused_exact = fused_exact and bool(
                np.array_equal(emitted, expected[index:]))

        print("\n===== NEXUS PI0.5 ACTION-CHUNK FUSION GATE =====")
        print(f"fingerprint       : 0x{mr.fingerprint:016x}")
        print(f"chunk/actions     : {horizon} x {LIBERO_ACTION_DIM}")
        print(f"rounds            : {args.rounds} "
              f"(d={args.elapsed_steps} steps each)")
        print(f"decay/max_chunks  : {args.decay} / {args.max_chunks}")
        print(f"completed chunks  : {ac.completed(mode)}")
        print(f"raw == baseline   : {raw_exact}")
        print(f"fused == oracle   : {fused_exact}")
        print(f"index == oracle   : {index_exact}")
        if not (raw_exact and fused_exact and index_exact):
            raise SystemExit("FAILED: fusion gate mismatch")
        print("PASS - fusion policy matches the reference on real "
              "Pi0.5 chunks")
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
