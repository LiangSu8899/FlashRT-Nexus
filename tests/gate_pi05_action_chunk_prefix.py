"""Gate — Pi0.5 through the prev-chunk-prefix prepare policy (host transport).

Drives the in-graph RTC correction on the real model: the producer captures
`decode_rtc_prefix` with a fixed prefix_len; every denoise step freezes the
first prefix_len rows of the new chunk to `rtc_prev_action_chunk`. The mode
retains the raw-space chunk (actions_raw), re-indexes it by the consumption
position at begin_request, and stages it for host injection; the host swaps
the staged bytes into the prev_action_chunk port between begin and commit.

Checks:
  1. The staged bytes equal the expected re-index of the previously retained
     raw chunk (recomputed gate-side from its own actions_raw read).
  2. Given identical port contents (noise, images, prev_action_chunk), the
     chunk the mode ingests equals a cap_model_tick baseline bit-exactly.
  3. Seating follows the latency switch: landing d steps late seats at d.
"""

from __future__ import annotations

import argparse
import ctypes
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nexus_action_chunk_abi import (  # noqa: E402
    CAP_OK, NEXUS_AC_CONSUME_SWITCH, NEXUS_AC_PREPARE_PREV_CHUNK_PREFIX,
    NEXUS_AC_READY, ActionChunkAbi, NexusActionChunkConfig, bind_core)
from gate_pi05_action_chunk import (  # noqa: E402
    NEXUS_LIB, PI05_LIB, FrtModelRuntimeV1, Pi05RuntimeConfig,
    action_affine, bind_pi05, dtype_from_model_runtime, make_images,
    make_noise_bytes, make_views, model_norm_stats)

import flash_rt  # noqa: E402
from flash_rt.core.utils.actions import LIBERO_ACTION_DIM  # noqa: E402
from flash_rt.subgraphs.pi05.rtc_prefix import (  # noqa: E402
    enable as enable_rtc_prefix)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--checkpoint", required=True)
    ap.add_argument("--num-views", type=int, default=3)
    ap.add_argument("--steps", type=int, default=10)
    ap.add_argument("--prompt", default="pick up the red block")
    ap.add_argument("--fp8", action="store_true")
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--prefix-len", type=int, default=3)
    ap.add_argument("--consume-per-round", type=int, default=4)
    args = ap.parse_args()

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
    enable_rtc_prefix(model, prefix_len=args.prefix_len)
    model.predict(images, prompt=args.prompt)
    pipe = model._pipe
    pl = pipe.pipeline

    mr = pl.export_model_runtime(
        identity={"gate": "nexus_pi05_action_chunk_prefix",
                  "plan": "context_rtc_prefix_action"},
        stage_plan="context_rtc_prefix_action",
        stage_plan_kwargs={"prefix_len": args.prefix_len},
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
        p_prev = nx.cap_model_find_port(cm, b"prev_action_chunk")
        p_raw = nx.cap_model_find_port(cm, b"actions_raw")
        if p_prev < 0 or p_raw < 0:
            raise RuntimeError(
                f"rtc ports missing: prev={p_prev} raw={p_raw}")
        noise_buf = nx.cap_model_port_buffer(cm, p_noise)
        noise_bytes = int(nx.cap_model_port_bytes(cm, p_noise))
        prev_buf = nx.cap_model_port_buffer(cm, p_prev)
        prev_bytes = int(nx.cap_model_port_bytes(cm, p_prev))
        raw_bytes = int(nx.cap_model_port_bytes(cm, p_raw))
        if prev_bytes != raw_bytes or prev_bytes % horizon:
            raise RuntimeError(
                f"unexpected rtc port sizes: prev={prev_bytes} raw={raw_bytes}")
        raw_row = prev_bytes // horizon
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

        def get_raw() -> bytes:
            # Gate-side independent read of the raw window (the mode reads
            # the same window through the runner's SWAP fast lane).
            arr = pl.bufs["diffusion_noise"].download_new(
                (horizon, raw_row // 2), np.uint16)
            data = arr.tobytes()
            if len(data) != raw_bytes:
                raise RuntimeError(f"raw window read {len(data)} bytes")
            return data

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
        mode_cfg.prepare_policy = NEXUS_AC_PREPARE_PREV_CHUNK_PREFIX
        mode_cfg.consume_policy = NEXUS_AC_CONSUME_SWITCH
        mode_cfg.prefix_len = args.prefix_len
        mode_cfg.raw_out_port = p_raw + 1
        mode_cfg.raw_action_bytes = raw_row
        if ac.create(dag, ctypes.byref(mode_cfg),
                     ctypes.byref(mode)) != CAP_OK:
            raise RuntimeError("prefix mode create failed")

        staged_exact = raw_exact = seat_exact = True
        gate_raw_prev = None
        staged = ctypes.create_string_buffer(prev_bytes)
        staged_written = ctypes.c_uint64(0)
        emitted = np.empty((1, LIBERO_ACTION_DIM), dtype=np.float32)
        written = ctypes.c_uint64(0)

        for round_i in range(args.rounds):
            j = int(ac.active_index(mode)) if int(ac.has_active(mode)) \
                else horizon
            if ac.begin_request(mode) != CAP_OK:
                raise RuntimeError("begin_request failed")
            if ac.prev_chunk_staged(mode, staged, prev_bytes,
                                    ctypes.byref(staged_written)) != CAP_OK:
                raise RuntimeError("prev_chunk_staged failed")

            if gate_raw_prev is None:
                expected = bytes(prev_bytes)
            else:
                rows = []
                for i in range(horizon):
                    src = min(j + i, horizon - 1)
                    rows.append(
                        gate_raw_prev[src * raw_row:(src + 1) * raw_row])
                expected = b"".join(rows)
            staged_exact = staged_exact and staged.raw == expected

            # HOST transport: swap the staged prev chunk into its port.
            stream = nx.cap_model_stage_stream(cm, 1)
            if nx.cap_swap(ctx, prev_buf, staged, prev_bytes,
                           stream) != CAP_OK:
                raise RuntimeError("cap_swap(prev_action_chunk) failed")
            set_inputs()
            fire_context()
            if ac.commit_request(mode) != CAP_OK:
                raise RuntimeError("commit_request failed")
            for _ in range(args.prefix_len):
                if ac.advance_step(mode) != CAP_OK:
                    raise RuntimeError("advance_step failed")
            state = ac.sync_next_chunk(mode)
            if state != NEXUS_AC_READY:
                raise RuntimeError(
                    f"sync_next_chunk state={state} "
                    f"last_error={ac.last_error(mode)} "
                    f"prev_bytes={prev_bytes} raw_row={raw_row}")
            seat_exact = seat_exact and \
                int(ac.active_index(mode)) == args.prefix_len

            gate_raw_prev = get_raw()
            raw = get_actions()
            set_inputs()
            if nx.cap_model_tick(ctx, cm) != CAP_OK:
                raise RuntimeError("cap_model_tick baseline failed")
            nx.cap_sync(ctx, nx.cap_model_stage_stream(cm, 1))
            baseline = get_actions()
            raw_exact = raw_exact and bool(np.array_equal(raw, baseline))

            for _ in range(args.consume_per_round):
                rc = ac.next_action(
                    mode, ctypes.c_void_p(emitted.ctypes.data), action_bytes,
                    ctypes.byref(written))
                if rc != NEXUS_AC_READY:
                    raise RuntimeError(f"next_action rc={rc}")

        print("\n===== NEXUS PI0.5 ACTION-CHUNK PREV-PREFIX GATE =====")
        print(f"fingerprint       : 0x{mr.fingerprint:016x}")
        print(f"chunk/actions     : {horizon} x {LIBERO_ACTION_DIM}")
        print(f"rounds            : {args.rounds} "
              f"(prefix_len={args.prefix_len}, host transport)")
        print(f"raw row bytes     : {raw_row}")
        print(f"completed chunks  : {ac.completed(mode)}")
        print(f"staged==expected  : {staged_exact}")
        print(f"raw == baseline   : {raw_exact}")
        print(f"seating (d)       : {seat_exact}")
        if not (staged_exact and raw_exact and seat_exact):
            raise SystemExit("FAILED: prev-prefix gate mismatch")
        print("PASS - prev-chunk-prefix prepare matches the baseline on "
              "real Pi0.5")
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
