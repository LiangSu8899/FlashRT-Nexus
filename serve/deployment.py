"""Deployment lifecycle independent of any transport."""

from __future__ import annotations

import os
from collections.abc import Mapping
from dataclasses import dataclass
from os import PathLike
from typing import Any

from flashrt_nexus.library import find_library

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


def open_deployment(
    source: str | PathLike[str] | Mapping[str, Any],
) -> Deployment:
    manifest = load_manifest(source)
    state_cfg = get_section(manifest, "state")
    producer_cfg = get_section(manifest, "producer")
    configured_lib = optional_str(
        producer_cfg, "nexus_lib", os.environ.get("NEXUS_LIB", ""))
    nexus_lib = find_library(configured_lib or None)
    producer = build_producer(manifest)
    session = ModelSession(
        nexus_lib=nexus_lib,
        producer=producer,
        capsule_dir=optional_str(state_cfg, "capsule_dir", ""),
    )
    return Deployment(manifest=manifest, producer=producer, session=session)
