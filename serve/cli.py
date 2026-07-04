"""Command-line entry for the Nexus serving shell."""

from __future__ import annotations

import argparse
import os
import signal
import sys

from .manifest import get_section, load_manifest, optional_int, optional_str
from .producers import build_producer
from .session import ModelSession
from .transports.act_http import serve_act_http


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="nexus")
    sub = ap.add_subparsers(dest="cmd", required=True)
    serve = sub.add_parser("serve", help="serve a deployment manifest")
    serve.add_argument("manifest")
    args = ap.parse_args(argv)
    if args.cmd == "serve":
        return _serve(args.manifest)
    return 2


def _serve(path: str) -> int:
    manifest = load_manifest(path)
    serve_cfg = get_section(manifest, "serve")
    state_cfg = get_section(manifest, "state")
    transport = optional_str(serve_cfg, "transport", "act_http")
    host = optional_str(serve_cfg, "host", "127.0.0.1")
    port = optional_int(serve_cfg, "port", 8080)
    nexus_lib = optional_str(
        get_section(manifest, "producer"),
        "nexus_lib",
        os.environ.get("NEXUS_LIB", "build/libcapsule_nexus_flashrt.so"),
    )
    producer = build_producer(manifest)
    session = ModelSession(
        nexus_lib=nexus_lib,
        producer=producer,
        capsule_dir=optional_str(state_cfg, "capsule_dir", ""),
    )
    print(f"phase={session.phase} fingerprint=0x{session.fingerprint:016x}")
    print(f"serving {transport} on {host}:{port}")

    def _drain(signum, frame):  # noqa: ARG001
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, _drain)
    try:
        if transport == "act_http":
            serve_act_http(session, host, port)
        else:
            raise ValueError(f"unsupported transport {transport!r}")
    except KeyboardInterrupt:
        print("draining")
    finally:
        session.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
