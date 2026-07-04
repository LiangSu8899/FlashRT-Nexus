"""Producer adapters used by `nexus serve`.

Model-specific setup lives here, outside Nexus core. After construction,
sessions drive only the adopted cap_model_runtime face.
"""

from __future__ import annotations

import ctypes
import os
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import numpy as np

from .ffi import (
    CAP_OK,
    FrtModelRuntimeV1,
    Pi05RuntimeConfig,
    bind_pi05,
    dtype_to_pi05,
)
from .manifest import ManifestError, optional_int, optional_str


@dataclass
class ProducerHandle:
    model: Any
    frontend: Any
    pipeline: Any
    model_runtime: Any
    runtime_ptr: ctypes.c_void_p
    runtime_view: FrtModelRuntimeV1 | None
    release: Any
    action_shape: tuple[int, int]
    num_views: int
    prompt: str
    noise_dtype: int


def build_producer(manifest: dict[str, Any]) -> ProducerHandle:
    model_cfg = _section(manifest, "model")
    producer_cfg = _section(manifest, "producer")
    config = optional_str(model_cfg, "config", "pi05")
    if config != "pi05":
        raise ManifestError(
            f"serve P1 supports config=pi05; got {config!r}")
    return _build_pi05(model_cfg, producer_cfg)


def _build_pi05(model_cfg: dict[str, Any],
                producer_cfg: dict[str, Any]) -> ProducerHandle:
    flashrt_dir = optional_str(producer_cfg, "flashrt_dir",
                               os.environ.get("FLASHRT_DIR", ""))
    if not flashrt_dir:
        raise ManifestError("producer.flashrt_dir or FLASHRT_DIR is required")
    _add_flashrt_paths(Path(flashrt_dir))

    import flash_rt  # noqa: WPS433
    from flash_rt.core.utils.actions import LIBERO_ACTION_DIM  # noqa: WPS433

    checkpoint = optional_str(model_cfg, "checkpoint")
    if not checkpoint:
        raise ManifestError("model.checkpoint is required")
    precision = optional_str(model_cfg, "precision", "fp16")
    if precision not in ("fp16", "fp8"):
        raise ManifestError("model.precision must be fp16 or fp8")
    stage_plan = optional_str(model_cfg, "stage_plan", "full")
    io_face = optional_str(model_cfg, "io", "native")
    if io_face != "native":
        raise ManifestError("serve P1 requires model.io=native")
    num_views = optional_int(model_cfg, "num_views", 3)
    steps = optional_int(model_cfg, "steps", 10)
    seed = optional_int(model_cfg, "seed", 0)
    prompt = optional_str(model_cfg, "prompt", "pick up the red block")

    if stage_plan in ("context_action", "context_rtc_prefix_action"):
        from flash_rt.subgraphs.pi05.context_action import enable as enable_context_action  # noqa: E501, WPS433
    else:
        enable_context_action = None

    model = flash_rt.load_model(
        checkpoint,
        framework=optional_str(model_cfg, "framework", "torch"),
        config="pi05",
        hardware=optional_str(model_cfg, "hardware", "auto"),
        num_views=num_views,
        num_steps=steps,
        cache_frames=1,
        use_fp8=(precision == "fp8"),
        use_fp16=(precision == "fp16"),
    )
    if enable_context_action is not None:
        enable_context_action(model)

    warmup_images = _warmup_images(num_views, seed)
    model.predict(warmup_images, prompt=prompt)
    frontend = model._pipe
    pipeline = frontend.pipeline

    mr = pipeline.export_model_runtime(
        identity={"serve": "nexus", "checkpoint": Path(checkpoint).name},
        stage_plan=stage_plan,
        io="native",
    )
    runtime_ptr: ctypes.c_void_p | int = mr.ptr
    runtime_view: FrtModelRuntimeV1 | None = None
    release = mr.release
    native_verbs = optional_str(producer_cfg, "native_verbs",
                                os.environ.get("PI05_LIB", ""))
    dtype_id = dtype_to_pi05(_port_dtype(mr, "noise"))
    if native_verbs:
        lib = ctypes.CDLL(native_verbs)
        bind_pi05(lib)
        mean, stddev = _action_affine(_norm_stats(pipeline, frontend))
        cfg = Pi05RuntimeConfig()
        cfg.struct_size = ctypes.sizeof(cfg)
        cfg.num_views = num_views
        cfg.chunk = int(getattr(pipeline, "chunk_size"))
        cfg.model_action_dim = 32
        cfg.robot_action_dim = int(LIBERO_ACTION_DIM)
        cfg.action_mean = mean.ctypes.data_as(ctypes.POINTER(ctypes.c_float))
        cfg.n_action_mean = mean.size
        cfg.action_stddev = stddev.ctypes.data_as(ctypes.POINTER(ctypes.c_float))
        cfg.n_action_stddev = stddev.size
        cfg.image_buffer_name = b"observation_images_normalized"
        cfg.action_buffer_name = b"diffusion_noise"
        cfg.image_dtype = dtype_id
        cfg.action_dtype = dtype_id
        out = ctypes.c_void_p()
        rc = lib.frt_pi05_model_runtime_create_over(
            ctypes.c_void_p(mr.ptr), ctypes.byref(cfg), ctypes.byref(out))
        if rc != CAP_OK:
            raise RuntimeError(f"frt_pi05_model_runtime_create_over rc={rc}")
        runtime_ptr = out
        runtime_view = ctypes.cast(out, ctypes.POINTER(FrtModelRuntimeV1)).contents

        def release() -> None:
            runtime_view.release(runtime_view.owner)
            mr.release()

    return ProducerHandle(
        model=model,
        frontend=frontend,
        pipeline=pipeline,
        model_runtime=mr,
        runtime_ptr=_as_void_p(runtime_ptr),
        runtime_view=runtime_view,
        release=release,
        action_shape=_action_shape(mr),
        num_views=num_views,
        prompt=prompt,
        noise_dtype=dtype_id,
    )


def _add_flashrt_paths(root: Path) -> None:
    for sub in ("", "exec/build-container", "runtime/build-container",
                "exec/build", "runtime/build", "cpp/build-container",
                "cpp/build"):
        p = str(root / sub) if sub else str(root)
        if p not in sys.path:
            sys.path.insert(0, p)


def _warmup_images(num_views: int, seed: int) -> list[np.ndarray]:
    rng = np.random.default_rng(seed)
    return [
        np.ascontiguousarray(
            rng.integers(0, 256, size=(224, 224, 3), dtype=np.uint8))
        for _ in range(num_views)
    ]


def _action_affine(norm_stats: dict[str, Any]) -> tuple[np.ndarray, np.ndarray]:
    q01 = np.asarray(norm_stats["actions"]["q01"], dtype=np.float32)
    q99 = np.asarray(norm_stats["actions"]["q99"], dtype=np.float32)
    scale = (q99 - q01 + 1e-6) / 2.0
    mean = q01 + scale
    return np.ascontiguousarray(mean), np.ascontiguousarray(scale)


def _norm_stats(pipeline: Any, frontend: Any) -> dict[str, Any]:
    stats = getattr(pipeline, "norm_stats", None)
    if stats is None:
        stats = getattr(frontend, "norm_stats", None)
    if stats is None:
        raise RuntimeError("Pi05 producer does not expose norm_stats")
    return stats


def _port_dtype(mr: Any, name: str) -> object:
    for port in mr.ports():
        if port.get("name") == name:
            return port.get("dtype")
    raise RuntimeError(f"missing model-runtime port {name!r}")


def _action_shape(mr: Any) -> tuple[int, int]:
    for port in mr.ports():
        if port.get("name") == "actions":
            shape = tuple(int(x) for x in port.get("shape", ()))
            if len(shape) == 2:
                return shape
    raise RuntimeError("model-runtime must declare actions shape")


def _as_void_p(value: ctypes.c_void_p | int) -> ctypes.c_void_p:
    if isinstance(value, ctypes.c_void_p):
        return value
    return ctypes.c_void_p(value)


def _section(manifest: dict[str, Any], name: str) -> dict[str, Any]:
    value = manifest.get(name, {})
    if not isinstance(value, dict):
        raise ManifestError(f"{name} must be a mapping")
    return value
