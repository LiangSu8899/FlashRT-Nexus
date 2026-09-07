"""Producer adapter for a native FlashRT model-runtime provider DSO."""

from __future__ import annotations

import ctypes
import json
from pathlib import Path
from typing import Any

from serve.ffi import (
    FRT_PI05_DTYPE_BFLOAT16,
    FRT_PI05_DTYPE_FLOAT16,
    FRT_PI05_DTYPE_FLOAT32,
    FrtModelRuntimeV1,
)
from serve.manifest import ManifestError, optional_int, optional_str
from serve.producers import ProducerHandle


_DTYPES = {
    "bf16": FRT_PI05_DTYPE_BFLOAT16,
    "f16": FRT_PI05_DTYPE_FLOAT16,
    "fp16": FRT_PI05_DTYPE_FLOAT16,
    "f32": FRT_PI05_DTYPE_FLOAT32,
    "fp32": FRT_PI05_DTYPE_FLOAT32,
}


def build(manifest: dict[str, Any]) -> ProducerHandle:
    model_cfg = _section(manifest, "model")
    producer_cfg = _section(manifest, "producer")
    provider_path = Path(
        optional_str(producer_cfg, "provider_dso", "")).expanduser()
    if not provider_path.is_file():
        raise ManifestError(f"native provider not found: {provider_path}")
    provider_config = producer_cfg.get("config", {})
    if not isinstance(provider_config, dict):
        raise ManifestError("producer.config must be a mapping")

    library = ctypes.CDLL(str(provider_path.resolve()))
    open_v1 = library.frt_model_runtime_open_v1
    open_v1.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p)]
    open_v1.restype = ctypes.c_int
    out = ctypes.c_void_p()
    payload = json.dumps(provider_config, separators=(",", ":")).encode()
    rc = open_v1(payload, ctypes.byref(out))
    if rc != 0 or not out:
        raise RuntimeError(f"native provider open failed rc={rc}")
    runtime = ctypes.cast(out, ctypes.POINTER(FrtModelRuntimeV1)).contents

    chunk = optional_int(model_cfg, "chunk", 0)
    action_dim = optional_int(model_cfg, "action_dim", 0)
    num_views = optional_int(model_cfg, "num_views", 0)
    if chunk <= 0 or action_dim <= 0 or num_views <= 0:
        runtime.release(runtime.owner)
        raise ManifestError(
            "native model requires positive model.chunk, model.action_dim, "
            "and model.num_views")
    dtype_name = optional_str(model_cfg, "noise_dtype", "f32").lower()
    if dtype_name not in _DTYPES:
        runtime.release(runtime.owner)
        raise ManifestError(f"unsupported model.noise_dtype {dtype_name!r}")

    def release() -> None:
        runtime.release(runtime.owner)

    return ProducerHandle(
        model=library,
        frontend=None,
        pipeline=None,
        model_runtime=runtime,
        runtime_ptr=out,
        runtime_view=runtime,
        release=release,
        action_shape=(chunk, action_dim),
        num_views=num_views,
        prompt=optional_str(model_cfg, "prompt", ""),
        noise_dtype=_DTYPES[dtype_name],
    )


def _section(manifest: dict[str, Any], name: str) -> dict[str, Any]:
    value = manifest.get(name, {})
    if not isinstance(value, dict):
        raise ManifestError(f"{name} must be a mapping")
    return value
