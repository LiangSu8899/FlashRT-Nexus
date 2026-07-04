"""Producer plugin loading for `nexus serve`."""

from __future__ import annotations

import ctypes
import importlib
from dataclasses import dataclass
from typing import Any, Callable

from .ffi import FrtModelRuntimeV1
from .manifest import ManifestError, optional_str


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


ProducerBuilder = Callable[[dict[str, Any]], ProducerHandle]


def build_producer(manifest: dict[str, Any]) -> ProducerHandle:
    entry = _producer_entry(manifest)
    return _load_builder(entry)(manifest)


def _producer_entry(manifest: dict[str, Any]) -> str:
    producer_cfg = _section(manifest, "producer")
    entry = optional_str(producer_cfg, "entry", "")
    if entry:
        return entry
    config = optional_str(_section(manifest, "model"), "config", "")
    if config == "pi05":
        return "serve.producer_plugins.pi05:build"
    raise ManifestError(
        "producer.entry is required when model.config has no bundled plugin")


def _load_builder(entry: str) -> ProducerBuilder:
    if ":" not in entry:
        raise ManifestError("producer.entry must be module:function")
    module_name, fn_name = entry.split(":", 1)
    try:
        module = importlib.import_module(module_name)
        fn = getattr(module, fn_name)
    except (ImportError, AttributeError) as exc:
        raise ManifestError(f"cannot load producer.entry {entry!r}") from exc
    if not callable(fn):
        raise ManifestError(f"producer.entry {entry!r} is not callable")
    return fn


def _section(manifest: dict[str, Any], name: str) -> dict[str, Any]:
    value = manifest.get(name, {})
    if not isinstance(value, dict):
        raise ManifestError(f"{name} must be a mapping")
    return value
