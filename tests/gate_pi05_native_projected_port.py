#!/usr/bin/env python3
"""Gate projected-state action chunking through the native STAGED port."""

from __future__ import annotations

import argparse
import ctypes
import json
from pathlib import Path

import numpy as np

from gate_pi05_native_embedded import (
    FrtModelRuntimeV1,
    _bind_flashrt,
    _frames,
    _noise,
)
from nexus_action_chunk_abi import (
    CAP_OK,
    NEXUS_AC_DTYPE_F32,
    NEXUS_AC_PREPARE_PROJECTED_STATE,
    NEXUS_AC_READY,
    ActionChunkAbi,
    NexusActionChunkConfig,
    bind_core,
)


def check(rc: int, message: str) -> None:
    if rc != CAP_OK:
        raise RuntimeError(f"{message} failed rc={rc}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument("--tokenizer", type=Path, required=True)
    parser.add_argument("--flashrt-lib", type=Path, required=True)
    parser.add_argument("--nexus-lib", type=Path, required=True)
    parser.add_argument("--lookahead", type=int, default=3)
    args = parser.parse_args()
    for name in ("checkpoint", "tokenizer", "flashrt_lib", "nexus_lib"):
        path = getattr(args, name).resolve()
        if not path.exists():
            parser.error(f"--{name.replace('_', '-')} does not exist: {path}")
        setattr(args, name, path)

    flashrt = ctypes.CDLL(str(args.flashrt_lib), mode=ctypes.RTLD_GLOBAL)
    nexus = ctypes.CDLL(str(args.nexus_lib), mode=ctypes.RTLD_GLOBAL)
    _bind_flashrt(flashrt)
    bind_core(nexus)
    action_chunk = ActionChunkAbi(nexus)
    nexus.cap_model_fingerprint.restype = ctypes.c_uint64
    nexus.cap_model_fingerprint.argtypes = [ctypes.c_void_p]

    config = json.dumps({
        "io": "native_v2",
        "checkpoint_path": str(args.checkpoint),
        "tokenizer_model_path": str(args.tokenizer),
        "state_prompt_mode": "fixed",
        "max_prompt_tokens": 200,
        "state_dim": 8,
        "num_views": 2,
        "chunk": 10,
        "num_steps": 10,
        "vision_pool_factor": 1,
    }).encode()

    producer = ctypes.c_void_p()
    rc = flashrt.frt_model_runtime_open_v1(config, ctypes.byref(producer))
    if rc != 0:
        detail = (flashrt.frt_pi05_native_open_last_error() or b"").decode()
        raise RuntimeError(f"native open failed rc={rc}: {detail}")

    model = ctypes.c_void_p()
    ctx = ctypes.c_void_p()
    dag = ctypes.c_void_p()
    mode = ctypes.c_void_p()
    try:
        producer_view = ctypes.cast(
            producer, ctypes.POINTER(FrtModelRuntimeV1)
        ).contents
        check(
            nexus.flashrt_adopt_model_runtime(producer, ctypes.byref(model)),
            "adopt native model",
        )
        producer_view.release(producer_view.owner)
        producer = ctypes.c_void_p()

        ports = {
            name: nexus.cap_model_find_port(model, name)
            for name in (b"prompt", b"state", b"images", b"noise", b"actions")
        }
        if any(index < 0 for index in ports.values()):
            raise RuntimeError(f"missing native port: {ports}")

        ctx = ctypes.c_void_p(nexus.cap_ctx_create(
            nexus.cap_model_backend(model)
        ))
        if not ctx:
            raise RuntimeError("cap_ctx_create failed")
        check(nexus.nexus_stage_dag_create(ctx, model, ctypes.byref(dag)),
              "create stage DAG")

        cfg = NexusActionChunkConfig()
        cfg.struct_size = ctypes.sizeof(cfg)
        cfg.action_stage = 0
        cfg.output_port = ports[b"actions"]
        cfg.chunk_length = 10
        cfg.action_bytes = 7 * ctypes.sizeof(ctypes.c_float)
        cfg.ring_slots = 2
        cfg.execute_horizon = 0
        cfg.poll_budget = -1
        cfg.deadline_steps = -1
        cfg.prepare_policy = NEXUS_AC_PREPARE_PROJECTED_STATE
        cfg.scalar_dtype = NEXUS_AC_DTYPE_F32
        cfg.action_representation = 1
        cfg.state_dim = 8
        cfg.lookahead_steps = args.lookahead
        cfg.state_input_port = ports[b"state"] + 1
        check(action_chunk.create(dag, ctypes.byref(cfg), ctypes.byref(mode)),
              "create projected-state mode")

        mapping = np.asarray(
            [0, 1, 2, 3, 4, 5, 6, np.iinfo(np.uint32).max],
            dtype=np.uint32,
        )
        check(action_chunk.set_state_action_indices(
            mode, mapping.ctypes.data_as(ctypes.POINTER(ctypes.c_uint32)),
            mapping.size), "set state-action map")

        _images, views = _frames()
        prompt = b"pick up the red block"
        prompt_ptr = ctypes.cast(ctypes.c_char_p(prompt), ctypes.c_void_p)
        check(nexus.cap_model_set_input(
            model, ports[b"prompt"], prompt_ptr, len(prompt), -1
        ), "stage prompt")
        check(nexus.cap_model_set_input(
            model, ports[b"images"], ctypes.cast(views, ctypes.c_void_p),
            ctypes.sizeof(views), -1
        ), "stage images")

        noise_bytes = int(nexus.cap_model_port_bytes(model, ports[b"noise"]))
        noise_buffer = nexus.cap_model_port_buffer(model, ports[b"noise"])
        stream = nexus.cap_model_stage_stream(model, 0)

        def stage_noise(seed: int) -> np.ndarray:
            noise = _noise(seed, noise_bytes // 2)
            check(nexus.cap_swap(
                ctx, noise_buffer, ctypes.c_void_p(noise.ctypes.data),
                noise.nbytes, stream
            ), "stage noise")
            return noise

        def read_actions() -> np.ndarray:
            out = np.empty((10, 7), dtype=np.float32)
            written = ctypes.c_uint64()
            check(nexus.cap_model_get_output(
                model, ports[b"actions"], ctypes.c_void_p(out.ctypes.data),
                out.nbytes, ctypes.byref(written), -1
            ), "read actions")
            if written.value != out.nbytes:
                raise RuntimeError(f"unexpected action bytes {written.value}")
            return out

        measured0 = np.linspace(-0.2, 0.2, 8, dtype=np.float32)
        stage_noise(17)
        check(action_chunk.set_state(
            mode, measured0.ctypes.data_as(ctypes.POINTER(ctypes.c_float)), 8
        ), "set first state")
        check(action_chunk.request(mode), "fire first request")
        if action_chunk.sync_next_chunk(mode) != NEXUS_AC_READY:
            raise RuntimeError("first request did not become ready")
        first_actions = read_actions()

        emitted = np.empty(7, dtype=np.float32)
        written = ctypes.c_uint64()
        for _ in range(2):
            rc = action_chunk.next_action(
                mode, ctypes.c_void_p(emitted.ctypes.data), emitted.nbytes,
                ctypes.byref(written)
            )
            if rc != NEXUS_AC_READY or written.value != emitted.nbytes:
                raise RuntimeError("failed to consume first action chunk")

        measured1 = np.linspace(0.3, -0.3, 8, dtype=np.float32)
        stage_noise(23)
        check(action_chunk.set_state(
            mode, measured1.ctypes.data_as(ctypes.POINTER(ctypes.c_float)), 8
        ), "set second state")
        check(action_chunk.begin_request(mode), "prepare projected state")
        projected = np.empty(8, dtype=np.float32)
        projected_dims = ctypes.c_uint32()
        check(action_chunk.projected_state(
            mode, projected.ctypes.data_as(ctypes.POINTER(ctypes.c_float)), 8,
            ctypes.byref(projected_dims)
        ), "read projected state")
        expected = measured1.copy()
        expected[:7] = (
            measured1[:7].astype(np.float64) +
            first_actions[2:2 + args.lookahead].astype(np.float64).sum(axis=0)
        ).astype(np.float32)
        tolerance = np.abs(np.spacing(expected))
        projection_ok = (
            projected_dims.value == 8 and
            np.all(np.abs(projected - expected) <= tolerance) and
            projected[7] == measured1[7]
        )
        if not projection_ok:
            raise RuntimeError(
                f"projected state mismatch max_abs="
                f"{np.max(np.abs(projected - expected))}"
            )

        check(action_chunk.commit_request(mode), "fire projected request")
        if action_chunk.sync_next_chunk(mode) != NEXUS_AC_READY:
            raise RuntimeError("projected request did not become ready")
        port_lane_actions = read_actions()

        check(nexus.cap_model_set_input(
            model, ports[b"state"], ctypes.c_void_p(projected.ctypes.data),
            projected.nbytes, -1
        ), "stage baseline state")
        stage_noise(23)
        check(nexus.cap_model_tick(ctx, model), "run manual baseline")
        check(nexus.cap_sync(ctx, stream), "sync manual baseline")
        baseline_actions = read_actions()
        action_exact = np.array_equal(port_lane_actions, baseline_actions)
        if not action_exact:
            raise RuntimeError(
                f"port-lane action mismatch max_abs="
                f"{np.max(np.abs(port_lane_actions - baseline_actions))}"
            )

        print("\n===== NATIVE PROJECTED-STATE PORT GATE =====")
        print(f"fingerprint       : 0x{nexus.cap_model_fingerprint(model):016x}")
        print(f"state/action dims : 8 / 7 (dimension 7 preserved)")
        print(f"lookahead         : {args.lookahead}")
        print(f"projection <=1ulp: {projection_ok}")
        print(f"port == baseline  : {action_exact}")
        print("PASS - projected state stages through the native model port")
        return 0
    finally:
        if mode:
            action_chunk.destroy(mode)
        if dag:
            nexus.nexus_stage_dag_destroy(dag)
        if ctx:
            nexus.cap_ctx_destroy(ctx)
        if model:
            nexus.flashrt_model_close(model)
        if producer:
            view = ctypes.cast(
                producer, ctypes.POINTER(FrtModelRuntimeV1)
            ).contents
            view.release(view.owner)


if __name__ == "__main__":
    raise SystemExit(main())
