#!/usr/bin/env python3
"""Example runner — Pi0.5 RTC action chunks over Nexus.

Thin presentation wrapper: validates the environment, then delegates to
the acceptance gate (tests/gate_pi05_rtc_action_chunk.py), which is the
single source of truth for the wiring. See README.md in this directory.
"""
from __future__ import annotations

import os
import runpy
import sys


def _fail(msg: str) -> "None":
    raise SystemExit(f"[examples/pi05_rtc] {msg}")


def main() -> None:
    repo_root = os.path.dirname(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

    flashrt_dir = os.environ.get("FLASHRT_DIR")
    if not flashrt_dir or not os.path.isdir(flashrt_dir):
        _fail("set FLASHRT_DIR=<path to the FlashRT repo root>")

    nexus_lib = os.environ.get(
        "NEXUS_LIB",
        os.path.join(repo_root, "build", "libcapsule_nexus_flashrt.so"))
    if not os.path.isfile(nexus_lib):
        _fail(
            f"Nexus backend library not found: {nexus_lib}\n"
            "  build with -DCAPSULE_BUILD_FLASHRT_BACKEND=ON or set "
            "NEXUS_LIB")
    os.environ["NEXUS_LIB"] = nexus_lib

    if "--checkpoint" not in sys.argv:
        _fail("pass --checkpoint /path/to/pi05_checkpoint "
              "(plus any gate options, e.g. --bench-iters 50)")

    gate = os.path.join(repo_root, "tests", "gate_pi05_rtc_action_chunk.py")
    runpy.run_path(gate, run_name="__main__")


if __name__ == "__main__":
    main()
