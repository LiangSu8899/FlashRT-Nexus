#!/usr/bin/env python3
"""Minimal no-HTTP Pi0.5 action call over the Nexus embedded API."""

from __future__ import annotations

import argparse
import pathlib
import sys

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from serve.embedded import EmbeddedSession


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("manifest", nargs="?", default="examples/pi05_libero.yaml")
    args = ap.parse_args()
    with EmbeddedSession.open(args.manifest) as nx:
        images = [np.zeros((224, 224, 3), dtype=np.uint8) for _ in range(3)]
        result = nx.act(images, prompt="pick up the red block", seed=7)
        print({
            "chunk_id": result.chunk_id,
            "shape": list(result.actions.shape),
            "latency_ms": result.latency_ms,
        })
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
