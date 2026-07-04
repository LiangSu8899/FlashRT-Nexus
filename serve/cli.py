"""Command-line entry for the Nexus serving shell."""

from __future__ import annotations

import argparse
import signal
import sys

from .deployment import open_deployment
from .manifest import get_section, load_manifest, optional_int, optional_str
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
    transport = optional_str(serve_cfg, "transport", "act_http")
    host = optional_str(serve_cfg, "host", "127.0.0.1")
    port = optional_int(serve_cfg, "port", 8080)
    deployment = open_deployment(path)
    session = deployment.session
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
        deployment.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
