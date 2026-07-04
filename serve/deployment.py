"""Deployment lifecycle independent of any transport."""

from __future__ import annotations

import os
from dataclasses import dataclass
from typing import Any

from .manifest import get_section, load_manifest, optional_str
from .producers import ProducerHandle, build_producer
from .session import ModelSession


@dataclass
class Deployment:
    manifest: dict[str, Any]
    producer: ProducerHandle
    session: ModelSession

    def close(self) -> None:
        self.session.close()


def open_deployment(path: str) -> Deployment:
    manifest = load_manifest(path)
    state_cfg = get_section(manifest, "state")
    producer_cfg = get_section(manifest, "producer")
    nexus_lib = optional_str(
        producer_cfg,
        "nexus_lib",
        os.environ.get("NEXUS_LIB", "build/libcapsule_nexus_flashrt.so"),
    )
    producer = build_producer(manifest)
    session = ModelSession(
        nexus_lib=nexus_lib,
        producer=producer,
        capsule_dir=optional_str(state_cfg, "capsule_dir", ""),
    )
    return Deployment(manifest=manifest, producer=producer, session=session)
