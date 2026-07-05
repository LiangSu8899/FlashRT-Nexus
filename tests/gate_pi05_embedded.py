#!/usr/bin/env python3
"""Real Pi0.5 gate for the no-HTTP embedded Nexus path.

Required environment:
  FLASHRT_DIR       FlashRT checkout containing the Pi0.5 producer
  PI05_CHECKPOINT   Pi0.5 checkpoint directory
  PI05_LIB          native Pi0.5 runtime verb library
  NEXUS_LIB         built libcapsule_nexus_flashrt.so
"""

from __future__ import annotations

import argparse
import os
import pathlib
import statistics
import sys
import tempfile
import time

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from serve.embedded import EmbeddedSession


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--manifest", default="")
    ap.add_argument("--iters", type=int, default=8)
    ap.add_argument("--prompt", default="pick up the red block")
    args = ap.parse_args()

    _require_env("FLASHRT_DIR")
    _require_env("PI05_CHECKPOINT")
    _require_env("PI05_LIB")
    _require_env("NEXUS_LIB")

    manifest_path = args.manifest
    tmpdir: tempfile.TemporaryDirectory[str] | None = None
    if not manifest_path:
        tmpdir = tempfile.TemporaryDirectory(prefix="nexus-pi05-embedded-")
        manifest_path = str(pathlib.Path(tmpdir.name) / "pi05.yaml")
        pathlib.Path(manifest_path).write_text(_manifest(tmpdir.name))

    images = _frames(3)
    try:
        with EmbeddedSession.open(manifest_path) as nx:
            a = nx.act(images, prompt=args.prompt, seed=7)
            b = nx.act(images, prompt=args.prompt, seed=7)
            c = nx.act(images, prompt=args.prompt, seed=8)
            _check(a.actions.shape == (10, 7), f"action shape {a.actions.shape}")
            _check(np.array_equal(a.actions, b.actions), "same seed is deterministic")
            _check(not np.array_equal(a.actions, c.actions), "different seed changes action")

            cap = nx.snapshot("ep-001")
            expected = nx.act(images, prompt=args.prompt, seed=9)
            dirty = nx.act(images, prompt=args.prompt, seed=11)
            _check(not np.array_equal(expected.actions, dirty.actions),
                   "dirty step changes state")
            nx.reset(cap)
            restored = nx.act(images, prompt=args.prompt, seed=9)
            _check(np.array_equal(expected.actions, restored.actions),
                   "reset restores embedded episode state")

            lat = []
            for i in range(max(1, args.iters)):
                t0 = time.perf_counter()
                out = nx.act(images, prompt=args.prompt, seed=100 + i)
                lat.append((time.perf_counter() - t0) * 1000.0)
                _check(out.actions.shape == (10, 7), "loop action shape")
            print({
                "ok": True,
                "iters": len(lat),
                "p50_ms": round(statistics.median(lat), 3),
                "max_ms": round(max(lat), 3),
                "capsules": nx.state()["capsules"],
            })
    finally:
        if tmpdir is not None:
            tmpdir.cleanup()
    return 0


def _manifest(capsule_dir: str) -> str:
    return f"""
model:
  checkpoint: $PI05_CHECKPOINT
  config: pi05
  framework: torch
  hardware: auto
  precision: ${{PI05_PRECISION:-fp8}}
  num_views: 3
  steps: 10
  stage_plan: full
  io: native
  prompt: pick up the red block
producer:
  kind: python
  entry: serve.producer_plugins.pi05:build
  flashrt_dir: $FLASHRT_DIR
  nexus_lib: $NEXUS_LIB
  native_verbs: $PI05_LIB
mode:
  kind: tick
serve:
  transport: embedded
state:
  capsule_dir: {capsule_dir}
"""


def _frames(n: int) -> list[np.ndarray]:
    rng = np.random.default_rng(123)
    return [
        np.ascontiguousarray(
            rng.integers(0, 256, size=(224, 224, 3), dtype=np.uint8))
        for _ in range(n)
    ]


def _require_env(name: str) -> None:
    if not os.environ.get(name):
        raise SystemExit(f"{name} is required")


def _check(cond: bool, msg: str) -> None:
    if not cond:
        raise AssertionError(msg)
    print(f"ok  : {msg}")


if __name__ == "__main__":
    raise SystemExit(main())
