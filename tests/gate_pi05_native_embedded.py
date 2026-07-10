#!/usr/bin/env python3
"""Gate the native Pi0.5 producer through Nexus embedded sessions."""

from __future__ import annotations

import argparse
import ctypes
import json
from pathlib import Path
import statistics

import numpy as np


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


class EmbeddedConfig(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("model", ctypes.c_void_p),
        ("flags", ctypes.c_uint32),
    ]


class TickResult(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("chunk_id", ctypes.c_uint64),
        ("latency_ms", ctypes.c_double),
        ("written", ctypes.c_uint64),
    ]


def _bind_flashrt(lib):
    lib.frt_model_runtime_open_v1.restype = ctypes.c_int
    lib.frt_model_runtime_open_v1.argtypes = [
        ctypes.c_char_p,
        ctypes.POINTER(ctypes.c_void_p),
    ]
    lib.frt_pi05_native_open_last_error.restype = ctypes.c_char_p
    lib.frt_pi05_native_open_last_error.argtypes = []


def _bind_nexus(lib):
    pointer = ctypes.c_void_p
    lib.flashrt_adopt_model_runtime.restype = ctypes.c_int
    lib.flashrt_adopt_model_runtime.argtypes = [
        pointer,
        ctypes.POINTER(pointer),
    ]
    lib.flashrt_model_close.restype = None
    lib.flashrt_model_close.argtypes = [pointer]
    lib.cap_model_find_port.restype = ctypes.c_int
    lib.cap_model_find_port.argtypes = [pointer, ctypes.c_char_p]
    lib.cap_model_port_bytes.restype = ctypes.c_uint64
    lib.cap_model_port_bytes.argtypes = [pointer, ctypes.c_uint64]
    lib.cap_model_port_update.restype = ctypes.c_uint32
    lib.cap_model_port_update.argtypes = [pointer, ctypes.c_uint64]
    lib.cap_model_fingerprint.restype = ctypes.c_uint64
    lib.cap_model_fingerprint.argtypes = [pointer]
    lib.cap_model_identity.restype = ctypes.c_char_p
    lib.cap_model_identity.argtypes = [pointer]
    lib.nexus_embedded_open.restype = ctypes.c_int
    lib.nexus_embedded_open.argtypes = [
        ctypes.POINTER(EmbeddedConfig),
        ctypes.POINTER(pointer),
    ]
    lib.nexus_embedded_close.restype = None
    lib.nexus_embedded_close.argtypes = [pointer]
    lib.nexus_embedded_set_input.restype = ctypes.c_int
    lib.nexus_embedded_set_input.argtypes = [
        pointer, ctypes.c_char_p, pointer, ctypes.c_uint64, ctypes.c_int,
    ]
    lib.nexus_embedded_swap.restype = ctypes.c_int
    lib.nexus_embedded_swap.argtypes = [
        pointer, ctypes.c_char_p, pointer, ctypes.c_uint64, ctypes.c_int,
    ]
    lib.nexus_embedded_tick.restype = ctypes.c_int
    lib.nexus_embedded_tick.argtypes = [pointer, ctypes.POINTER(TickResult)]
    lib.nexus_embedded_get_output.restype = ctypes.c_int
    lib.nexus_embedded_get_output.argtypes = [
        pointer, ctypes.c_char_p, pointer, ctypes.c_uint64,
        ctypes.POINTER(ctypes.c_uint64), ctypes.c_int,
    ]
    lib.nexus_embedded_snapshot.restype = ctypes.c_int
    lib.nexus_embedded_snapshot.argtypes = [pointer, ctypes.c_char_p]
    lib.nexus_embedded_restore.restype = ctypes.c_int
    lib.nexus_embedded_restore.argtypes = [pointer, ctypes.c_char_p]
    lib.nexus_embedded_capsule_count.restype = ctypes.c_uint64
    lib.nexus_embedded_capsule_count.argtypes = [pointer]
    lib.nexus_embedded_last_error.restype = ctypes.c_char_p
    lib.nexus_embedded_last_error.argtypes = [pointer]


def _check(rc: int, message: str, nexus=None, session=None,
           quiet: bool = False) -> None:
    if rc == 0:
        if not quiet:
            print(f"ok  : {message}")
        return
    detail = ""
    if nexus is not None and session:
        raw = nexus.nexus_embedded_last_error(session)
        detail = (raw or b"").decode(errors="replace")
    raise RuntimeError(f"{message} failed rc={rc}: {detail}")


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)
    print(f"ok  : {message}")


def _frames(count: int = 2):
    rng = np.random.default_rng(123)
    images = [
        np.ascontiguousarray(
            rng.integers(0, 256, size=(224, 224, 3), dtype=np.uint8)
        )
        for _ in range(count)
    ]
    views = (FrtImageView * count)()
    for index, image in enumerate(images):
        views[index].struct_size = ctypes.sizeof(FrtImageView)
        views[index].pixel_format = 0
        views[index].data = ctypes.c_void_p(image.ctypes.data)
        views[index].bytes = image.nbytes
        views[index].width = image.shape[1]
        views[index].height = image.shape[0]
        views[index].stride_bytes = image.strides[0]
    return images, views


def _noise(seed: int, elements: int) -> np.ndarray:
    values = np.asarray(
        np.random.default_rng(seed).standard_normal(elements),
        dtype=np.float32,
    )
    bits = values.view(np.uint32)
    rounding = np.uint32(0x7FFF) + ((bits >> 16) & 1)
    return ((bits + rounding) >> 16).astype(np.uint16)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument("--tokenizer", type=Path, required=True)
    parser.add_argument("--flashrt-lib", type=Path, required=True)
    parser.add_argument("--nexus-lib", type=Path, required=True)
    parser.add_argument("--iters", type=int, default=8)
    args = parser.parse_args()
    for name in ("checkpoint", "tokenizer", "flashrt_lib", "nexus_lib"):
        path = getattr(args, name).resolve()
        if not path.exists():
            parser.error(f"--{name.replace('_', '-')} does not exist: {path}")
        setattr(args, name, path)

    flashrt = ctypes.CDLL(str(args.flashrt_lib), mode=ctypes.RTLD_GLOBAL)
    nexus = ctypes.CDLL(str(args.nexus_lib), mode=ctypes.RTLD_GLOBAL)
    _bind_flashrt(flashrt)
    _bind_nexus(nexus)
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
    session = ctypes.c_void_p()
    try:
        producer_view = ctypes.cast(
            producer, ctypes.POINTER(FrtModelRuntimeV1)
        ).contents
        _require(producer_view.abi_version == 1, "producer ABI version")
        _require(
            producer_view.struct_size >= ctypes.sizeof(FrtModelRuntimeV1),
            "producer struct size",
        )
        _check(
            nexus.flashrt_adopt_model_runtime(producer, ctypes.byref(model)),
            "Nexus adopts native Pi0.5 model runtime",
        )
        producer_view.release(producer_view.owner)
        producer = ctypes.c_void_p()
        _require(
            bool(nexus.cap_model_fingerprint(model)),
            "fingerprint survives adoption",
        )
        identity = (nexus.cap_model_identity(model) or b"").decode()
        _require(
            "producer=native" in identity,
            "native identity survives adoption",
        )
        expected_ports = {
            b"prompt": 1,
            b"state": 1,
            b"images": 1,
            b"noise": 0,
            b"actions": 1,
            b"actions_raw": 0,
        }
        indices = {}
        for name, update in expected_ports.items():
            port = nexus.cap_model_find_port(model, name)
            _require(port >= 0, f"port {name.decode()} resolves")
            _require(
                int(nexus.cap_model_port_update(model, port)) == update,
                f"port {name.decode()} update class",
            )
            indices[name] = port
        noise_bytes = int(nexus.cap_model_port_bytes(model, indices[b"noise"]))
        _require(noise_bytes == 10 * 32 * 2, "noise window size")

        embedded = EmbeddedConfig()
        embedded.struct_size = ctypes.sizeof(EmbeddedConfig)
        embedded.model = model
        _check(
            nexus.nexus_embedded_open(ctypes.byref(embedded),
                                      ctypes.byref(session)),
            "embedded session opens",
        )
        _images, views = _frames()
        prompt = b"pick up the red block"
        state = np.linspace(-0.25, 0.25, 8, dtype=np.float32)
        for name, payload, size in (
            (b"prompt", prompt, len(prompt)),
            (b"state", state, state.nbytes),
            (b"images", views, ctypes.sizeof(views)),
        ):
            if isinstance(payload, bytes):
                pointer = ctypes.cast(ctypes.c_char_p(payload), ctypes.c_void_p)
            elif isinstance(payload, np.ndarray):
                pointer = ctypes.c_void_p(payload.ctypes.data)
            else:
                pointer = ctypes.cast(payload, ctypes.c_void_p)
            _check(
                nexus.nexus_embedded_set_input(
                    session, name, pointer, size, -1
                ),
                f"stages {name.decode()}", nexus, session,
            )

        actions = np.empty((10, 7), dtype=np.float32)

        def tick(seed: int, quiet: bool = False):
            noise = _noise(seed, noise_bytes // 2)
            _check(
                nexus.nexus_embedded_swap(
                    session, b"noise", ctypes.c_void_p(noise.ctypes.data),
                    noise.nbytes, -1,
                ),
                "swaps noise", nexus, session, quiet,
            )
            result = TickResult()
            result.struct_size = ctypes.sizeof(TickResult)
            _check(
                nexus.nexus_embedded_tick(session, ctypes.byref(result)),
                "embedded tick", nexus, session, quiet,
            )
            written = ctypes.c_uint64()
            _check(
                nexus.nexus_embedded_get_output(
                    session, b"actions", ctypes.c_void_p(actions.ctypes.data),
                    actions.nbytes, ctypes.byref(written), -1,
                ),
                "reads actions", nexus, session, quiet,
            )
            if written.value != actions.nbytes:
                raise RuntimeError(f"unexpected action bytes {written.value}")
            return actions.copy(), result.latency_ms

        same_a, _ = tick(7)
        same_b, _ = tick(7)
        different, _ = tick(8)
        _require(np.array_equal(same_a, same_b), "same seed is deterministic")
        _require(
            not np.array_equal(same_a, different),
            "different seed changes action",
        )
        _require(nexus.nexus_embedded_snapshot(session, b"episode") == 0,
                 "snapshot rollout boundary")
        dirty_actions, _ = tick(9)
        _require(nexus.nexus_embedded_restore(session, b"episode") == 0,
                 "restore rollout boundary")
        written = ctypes.c_uint64()
        restored = np.empty_like(actions)
        _check(
            nexus.nexus_embedded_get_output(
                session, b"actions", ctypes.c_void_p(restored.ctypes.data),
                restored.nbytes, ctypes.byref(written), -1,
            ),
            "reads restored action", nexus, session,
        )
        _require(np.array_equal(restored, different),
                 "restore recovers snapshotted action bytes")
        _require(not np.array_equal(restored, dirty_actions),
                 "restore discards dirty rollout bytes")

        latencies = []
        for iteration in range(max(1, args.iters)):
            _, latency = tick(1000 + iteration, quiet=True)
            latencies.append(latency)
        print({
            "ok": True,
            "iters": len(latencies),
            "p50_ms": round(statistics.median(latencies), 3),
            "max_ms": round(max(latencies), 3),
            "capsules": int(nexus.nexus_embedded_capsule_count(session)),
        })
    finally:
        if session:
            nexus.nexus_embedded_close(session)
        if model:
            nexus.flashrt_model_close(model)
        if producer:
            producer_view = ctypes.cast(
                producer, ctypes.POINTER(FrtModelRuntimeV1)
            ).contents
            producer_view.release(producer_view.owner)
    print("PASS native Pi0.5 through Nexus embedded session")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
