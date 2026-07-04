"""Adopted model session and capsule verbs for the serving shell."""

from __future__ import annotations

import base64
import ctypes
import os
import re
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import numpy as np

from .ffi import (
    CAP_OK,
    CAP_TIER_HOST,
    FRT_PI05_DTYPE_BFLOAT16,
    FRT_PI05_DTYPE_FLOAT16,
    FRT_PI05_DTYPE_FLOAT32,
    FRT_RT_PIXEL_RGB8,
    CapBoundary,
    CapRegion,
    FrtImageView,
    bind_nexus,
)
from .producers import ProducerHandle


@dataclass
class CapsuleRecord:
    capsule: ctypes.c_void_p
    created_at: float
    path: Path | None = None
    origin_restore: bool = True


@dataclass
class ActResult:
    actions: list[list[float]]
    chunk_id: int
    latency_ms: float


@dataclass
class ActArrayResult:
    actions: np.ndarray
    chunk_id: int
    latency_ms: float


class ModelSession:
    def __init__(self, nexus_lib: str, producer: ProducerHandle,
                 capsule_dir: str | None = None):
        self.phase = "ADOPT"
        self.nx = ctypes.CDLL(nexus_lib)
        bind_nexus(self.nx)
        self.producer = producer
        self.model = ctypes.c_void_p()
        rc = self.nx.flashrt_adopt_model_runtime(
            producer.runtime_ptr, ctypes.byref(self.model))
        if rc != CAP_OK:
            raise RuntimeError(f"flashrt_adopt_model_runtime rc={rc}")
        self.ctx = ctypes.c_void_p(self.nx.cap_ctx_create(
            self.nx.cap_model_backend(self.model)))
        if not self.ctx:
            raise RuntimeError("cap_ctx_create failed")
        self.fingerprint = int(self.nx.cap_model_fingerprint(self.model))
        self.identity = _decode(self.nx.cap_model_identity(self.model))
        self.ports = {
            "images": self._port("images"),
            "actions": self._port("actions"),
        }
        self.optional_ports = {
            "prompt": self._optional_port("prompt"),
            "text": self._optional_port("text"),
            "state": self._optional_port("state"),
        }
        noise_port = self.nx.cap_model_find_port(self.model, b"noise")
        self.noise_port = int(noise_port) if noise_port >= 0 else -1
        self.noise_buf = (self.nx.cap_model_port_buffer(self.model, noise_port)
                          if self.noise_port >= 0 else None)
        self.noise_bytes = (int(self.nx.cap_model_port_bytes(
            self.model, noise_port)) if self.noise_port >= 0 else 0)
        self.stream = int(self.nx.cap_model_stage_stream(self.model, 0))
        self.action_shape = producer.action_shape
        self.chunk_id = 0
        # One cap_ctx is driven by one thread of control (the core rule).
        # The transport may be threaded; every mutating verb serializes here.
        self.lock = threading.Lock()
        self.capsules: dict[str, CapsuleRecord] = {}
        self.skipped_capsules: list[str] = []
        self.capsule_dir = Path(capsule_dir) if capsule_dir else None
        if self.capsule_dir:
            self.capsule_dir.mkdir(parents=True, exist_ok=True)
            self._load_persisted_capsules()
        self.phase = "SERVE"

    def close(self) -> None:
        for rec in list(self.capsules.values()):
            self.nx.cap_capsule_drop(self.ctx, rec.capsule)
        self.capsules.clear()
        if getattr(self, "ctx", None):
            self.nx.cap_ctx_destroy(self.ctx)
            self.ctx = None
        if getattr(self, "model", None):
            self.nx.flashrt_model_close(self.model)
            self.model = None
        self.producer.release()
        self.phase = "DRAIN"

    def health(self) -> dict[str, Any]:
        return {
            "phase": self.phase,
            "fingerprint": f"0x{self.fingerprint:016x}",
            "capsules": len(self.capsules),
        }

    def state(self) -> dict[str, Any]:
        return {
            "phase": self.phase,
            "fingerprint": f"0x{self.fingerprint:016x}",
            "identity": self.identity,
            "ports": [
                {"name": name, "index": idx}
                for name, idx in sorted(self.ports.items())
            ],
            "optional_ports": {
                name: idx for name, idx in self.optional_ports.items()
                if idx >= 0
            },
            "action_shape": list(self.action_shape),
            "capsules": sorted(self.capsules.keys()),
            "skipped_capsules": list(self.skipped_capsules),
        }

    def act(self, request: dict[str, Any]) -> ActResult:
        with self.lock:
            return self._act_locked(request)

    def _act_locked(self, request: dict[str, Any]) -> ActResult:
        self._set_prompt(request)
        self._set_state(request)
        images = decode_images(request.get("images"), self.producer.num_views)
        result = self._run_images_array_locked(images, request.get("seed"))
        return ActResult(
            actions=result.actions.tolist(),
            chunk_id=result.chunk_id,
            latency_ms=result.latency_ms,
        )

    def act_arrays(self, images: list[np.ndarray], *, state: Any = None,
                   prompt: str | None = None,
                   seed: int | None = None) -> ActArrayResult:
        request: dict[str, Any] = {}
        if prompt is not None:
            request["prompt"] = prompt
        if state is not None:
            request["state"] = state
        with self.lock:
            self._set_prompt(request)
            self._set_state(request)
            normalized = normalize_image_arrays(images, self.producer.num_views)
            return self._run_images_array_locked(normalized, seed)

    def _run_images_array_locked(self, images: list[np.ndarray],
                                 seed: Any) -> ActArrayResult:
        views = make_image_views(images)
        t0 = time.perf_counter()
        rc = self.nx.cap_model_set_input(
            self.model,
            self.ports["images"],
            ctypes.cast(views, ctypes.c_void_p),
            ctypes.sizeof(views),
            -1,
        )
        if rc != CAP_OK:
            raise RuntimeError(f"cap_model_set_input(images) rc={rc}")
        self._seed_noise(seed)
        rc = self.nx.cap_model_tick(self.ctx, self.model)
        if rc != CAP_OK:
            err = _decode(self.nx.cap_model_last_error(self.model))
            raise RuntimeError(f"cap_model_tick rc={rc}: {err}")
        self.nx.cap_sync(self.ctx, self.stream)
        actions = self._read_actions()
        self.chunk_id += 1
        latency_ms = (time.perf_counter() - t0) * 1000.0
        return ActArrayResult(actions=actions, chunk_id=self.chunk_id,
                              latency_ms=latency_ms)

    def snapshot(self, name: str | None = None) -> str:
        with self.lock:
            return self._snapshot_locked(name)

    def _snapshot_locked(self, name: str | None) -> str:
        cap_id = sanitize_capsule_id(name or f"cap-{len(self.capsules) + 1:04d}")
        boundary = self._boundary()
        cap = self.nx.cap_snapshot(self.ctx, ctypes.byref(boundary),
                                   CAP_TIER_HOST, self.stream)
        if not cap:
            raise RuntimeError("cap_snapshot failed")
        self.nx.cap_sync(self.ctx, self.stream)
        path = self._capsule_path(cap_id)
        if path is not None:
            try:
                self._write_capsule_blob(cap, path)
            except Exception:
                self.nx.cap_capsule_drop(self.ctx, ctypes.c_void_p(cap))
                raise
        old = self.capsules.pop(cap_id, None)
        if old is not None:
            self.nx.cap_capsule_drop(self.ctx, old.capsule)
        self.capsules[cap_id] = CapsuleRecord(
            capsule=ctypes.c_void_p(cap), created_at=time.time(), path=path,
            origin_restore=True)
        return cap_id

    def reset(self, cap_id: str) -> None:
        with self.lock:
            self._reset_locked(cap_id)

    def _reset_locked(self, cap_id: str) -> None:
        rec = self.capsules.get(cap_id)
        if rec is None:
            raise KeyError(cap_id)
        if rec.origin_restore:
            rc = self.nx.cap_restore(self.ctx, rec.capsule, self.stream)
        else:
            boundary = self._boundary()
            rc = self.nx.cap_restore_into(
                self.ctx, rec.capsule, boundary.regions,
                boundary.n_regions, self.stream)
        if rc != CAP_OK:
            raise RuntimeError(f"capsule restore rc={rc}")
        self.nx.cap_sync(self.ctx, self.stream)

    def _port(self, name: str) -> int:
        idx = int(self.nx.cap_model_find_port(self.model, name.encode()))
        if idx < 0:
            raise RuntimeError(f"missing model-runtime port {name!r}")
        return idx

    def _optional_port(self, name: str) -> int:
        return int(self.nx.cap_model_find_port(self.model, name.encode()))

    def _set_prompt(self, request: dict[str, Any]) -> None:
        if "prompt" not in request:
            return
        prompt = str(request["prompt"])
        port = self.optional_ports.get("prompt", -1)
        if port < 0:
            port = self.optional_ports.get("text", -1)
        if port < 0:
            if prompt == self.producer.prompt:
                return
            raise ValueError(
                "this producer did not export a prompt/text port; "
                "dynamic prompt is not available")
        self._set_input_bytes(port, prompt.encode("utf-8"), "prompt")

    def _set_state(self, request: dict[str, Any]) -> None:
        if "state" not in request:
            return
        port = self.optional_ports.get("state", -1)
        if port < 0:
            raise ValueError("this producer did not export a state port")
        state = np.ascontiguousarray(request["state"], dtype=np.float32)
        self._set_input_bytes(port, state.tobytes(), "state")

    def _set_input_bytes(self, port: int, data: bytes, label: str) -> None:
        buf = ctypes.create_string_buffer(data, len(data))
        rc = self.nx.cap_model_set_input(
            self.model, port, ctypes.cast(buf, ctypes.c_void_p), len(data), -1)
        if rc != CAP_OK:
            err = _decode(self.nx.cap_model_last_error(self.model))
            raise RuntimeError(f"cap_model_set_input({label}) rc={rc}: {err}")

    def _seed_noise(self, seed: Any) -> None:
        if self.noise_port < 0:
            return
        rng = np.random.default_rng(None if seed is None else int(seed))
        if self.producer.noise_dtype == FRT_PI05_DTYPE_FLOAT32:
            noise = rng.standard_normal(self.noise_bytes // 4).astype(
                np.float32).tobytes()
        elif self.producer.noise_dtype == FRT_PI05_DTYPE_FLOAT16:
            noise = rng.standard_normal(self.noise_bytes // 2).astype(
                np.float16).tobytes()
        elif self.producer.noise_dtype == FRT_PI05_DTYPE_BFLOAT16:
            noise = _bf16_noise(rng, self.noise_bytes)
        else:
            raise RuntimeError(
                f"unsupported Pi05 noise dtype {self.producer.noise_dtype}")
        buf = ctypes.create_string_buffer(noise, self.noise_bytes)
        rc = self.nx.cap_swap(self.ctx, self.noise_buf, buf,
                              self.noise_bytes, self.stream)
        if rc != CAP_OK:
            raise RuntimeError(f"cap_swap(noise) rc={rc}")

    def _read_actions(self) -> np.ndarray:
        out = np.empty(self.action_shape, dtype=np.float32)
        written = ctypes.c_uint64(0)
        rc = self.nx.cap_model_get_output(
            self.model,
            self.ports["actions"],
            ctypes.c_void_p(out.ctypes.data),
            out.nbytes,
            ctypes.byref(written),
            -1,
        )
        if rc != CAP_OK or int(written.value) != out.nbytes:
            raise RuntimeError(
                f"cap_model_get_output rc={rc} written={written.value}")
        return out

    def _boundary(self) -> CapBoundary:
        ptr = self.nx.cap_model_region_array(self.model)
        n = int(self.nx.cap_model_region_count(self.model))
        return CapBoundary(
            ctypes.cast(ptr, ctypes.POINTER(CapRegion)),
            n,
            None,
            0,
        )

    def _capsule_path(self, cap_id: str) -> Path | None:
        if self.capsule_dir is None:
            return None
        return self.capsule_dir / f"{cap_id}.cap"

    def _write_capsule_blob(self, cap: int, path: Path) -> None:
        n = ctypes.c_size_t(0)
        rc = self.nx.cap_serialize(self.ctx, ctypes.c_void_p(cap), None,
                                   ctypes.byref(n))
        if rc != CAP_OK or n.value == 0:
            raise RuntimeError(f"cap_serialize(size) rc={rc} bytes={n.value}")
        blob = ctypes.create_string_buffer(n.value)
        rc = self.nx.cap_serialize(self.ctx, ctypes.c_void_p(cap), blob,
                                   ctypes.byref(n))
        if rc != CAP_OK:
            raise RuntimeError(f"cap_serialize(blob) rc={rc}")
        tmp = path.with_name(
            f".{path.name}.tmp-{os.getpid()}-{time.time_ns()}")
        tmp.write_bytes(blob.raw[:n.value])
        os.replace(tmp, path)

    def _load_persisted_capsules(self) -> None:
        assert self.capsule_dir is not None
        for path in sorted(self.capsule_dir.glob("*.cap")):
            cap_id = path.name[:-4]
            try:
                cap_id = sanitize_capsule_id(cap_id)
            except ValueError:
                self.skipped_capsules.append(path.name)
                continue
            data = path.read_bytes()
            if not data:
                self.skipped_capsules.append(path.name)
                continue
            buf = ctypes.create_string_buffer(data, len(data))
            cap = self.nx.cap_load(self.ctx, buf, len(data))
            if not cap:
                self.skipped_capsules.append(path.name)
                continue
            self.capsules[cap_id] = CapsuleRecord(
                capsule=ctypes.c_void_p(cap),
                created_at=path.stat().st_mtime,
                path=path,
                origin_restore=False,
            )


def decode_images(payload: Any, expected_views: int) -> list[np.ndarray]:
    if not isinstance(payload, list) or len(payload) != expected_views:
        raise ValueError(f"images must contain {expected_views} frames")
    frames = []
    for item in payload:
        if isinstance(item, str):
            data = base64.b64decode(item)
            width = height = 224
        elif isinstance(item, dict):
            data = base64.b64decode(str(item.get("data", "")))
            width = int(item.get("width", 224))
            height = int(item.get("height", 224))
        else:
            raise ValueError("each image must be a base64 string or object")
        expected = width * height * 3
        if len(data) != expected:
            raise ValueError(
                f"image bytes mismatch: got {len(data)}, expected {expected}")
        frames.append(np.frombuffer(data, dtype=np.uint8).reshape(height, width, 3))
    return normalize_image_arrays(frames, expected_views)


def normalize_image_arrays(images: list[np.ndarray],
                           expected_views: int) -> list[np.ndarray]:
    if not isinstance(images, list) or len(images) != expected_views:
        raise ValueError(f"images must contain {expected_views} frames")
    out = []
    for im in images:
        arr = np.asarray(im)
        if arr.dtype != np.uint8 or arr.ndim != 3 or arr.shape[2] != 3:
            raise ValueError("each image must be uint8 HWC RGB")
        out.append(np.ascontiguousarray(arr))
    return out


def make_image_views(images: list[np.ndarray]) -> ctypes.Array:
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


_CAPSULE_ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]{0,127}$")


def sanitize_capsule_id(value: str | None) -> str:
    if value is None:
        raise ValueError("capsule is required")
    cap_id = str(value)
    if not _CAPSULE_ID.fullmatch(cap_id):
        raise ValueError(
            "capsule must match [A-Za-z0-9][A-Za-z0-9_.-]{0,127}")
    return cap_id


def _bf16_noise(rng: np.random.Generator, nbytes: int) -> bytes:
    values = rng.standard_normal(nbytes // 2).astype(np.float32)
    try:
        import ml_dtypes  # noqa: WPS433

        return values.astype(ml_dtypes.bfloat16).tobytes()
    except Exception:  # noqa: BLE001
        words = values.view(np.uint32) >> 16
        return words.astype(np.uint16).tobytes()


def _decode(value: bytes | None) -> str:
    return value.decode() if value else ""
