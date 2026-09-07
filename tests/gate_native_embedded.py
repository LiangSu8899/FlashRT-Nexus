#!/usr/bin/env python3
"""No-GPU gate for native provider -> Nexus -> Python action chunks."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from flashrt_nexus import EmbeddedSession  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--nexus", required=True)
    parser.add_argument("--provider", required=True)
    args = parser.parse_args()
    manifest = {
        "model": {"chunk": 2, "action_dim": 2, "num_views": 1},
        "producer": {
            "entry": "serve.producer_plugins.native:build",
            "provider_dso": args.provider,
            "nexus_lib": args.nexus,
            "config": {"fixture": True},
        },
    }
    image = np.zeros((4, 4, 3), dtype=np.uint8)
    with EmbeddedSession.open(manifest) as session:
        result = session.act([image])
        if result.actions.shape != (2, 2) or result.actions[1, 1] != 4:
            raise RuntimeError("native blocking action result is incorrect")
        chunks = session.action_chunks(execute_horizon=1)
        chunks.request([image])
        chunks.wait_ready(1.0)
        if chunks.next_action().tolist() != [1.0, 2.0]:
            raise RuntimeError("native action chunk result is incorrect")
    print("PASS - native provider -> Nexus -> Python action chunk")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
