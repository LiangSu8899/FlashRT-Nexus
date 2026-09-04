"""Command-line entry for the Nexus serving shell."""

from __future__ import annotations

import argparse
import os
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
        return serve_deployment(args.manifest)
    return 2


def serve_deployment(path: str) -> int:
    manifest = load_manifest(path)
    serve_cfg = get_section(manifest, "serve")
    default_transport = ("execution_http"
                         if manifest.get("producer", {}).get("kind") == "worker"
                         else "act_http")
    transport = optional_str(serve_cfg, "transport", default_transport)
    host = optional_str(serve_cfg, "host", "127.0.0.1")
    port = optional_int(serve_cfg, "port", 8080)
    if transport == "execution_http":
        return _serve_execution(manifest, host, port)
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


def _serve_execution(manifest, host, port):
    from .execution import open_execution_service
    from .transports.execution_http import ExecutionHTTPServer

    config = manifest.get("serve", {})
    token = os.environ.get(config.get("token_env", "NEXUS_API_TOKEN"), "")

    def stop(signum, frame):
        raise KeyboardInterrupt

    previous = signal.signal(signal.SIGTERM, stop)
    service = None
    try:
        service = open_execution_service(manifest)
        with ExecutionHTTPServer((host, port), service, token=token) as server:
            print(f"serving execution_http on {host}:{server.server_port}",
                  file=sys.stderr, flush=True)
            server.serve_forever(poll_interval=0.05)
    except KeyboardInterrupt:
        pass
    finally:
        if service is not None:
            service.close()
        signal.signal(signal.SIGTERM, previous)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
