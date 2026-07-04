"""In-process Nexus serving API.

This is the first no-HTTP entry: an application owns the process/thread and
calls the adopted model session directly with image/state buffers.
"""

from __future__ import annotations

from types import TracebackType
from typing import Any

import numpy as np

from .deployment import Deployment, open_deployment
from .session import ActResult


class EmbeddedSession:
    def __init__(self, deployment: Deployment):
        self._deployment = deployment
        self.session = deployment.session

    @classmethod
    def open(cls, manifest_path: str) -> "EmbeddedSession":
        return cls(open_deployment(manifest_path))

    def close(self) -> None:
        self._deployment.close()

    def __enter__(self) -> "EmbeddedSession":
        return self

    def __exit__(self, exc_type: type[BaseException] | None,
                 exc: BaseException | None,
                 tb: TracebackType | None) -> None:
        self.close()

    def health(self) -> dict[str, Any]:
        return self.session.health()

    def state(self) -> dict[str, Any]:
        return self.session.state()

    def act(self, images: list[np.ndarray], *, state: Any = None,
            prompt: str | None = None, seed: int | None = None) -> ActResult:
        return self.session.act_arrays(
            images, state=state, prompt=prompt, seed=seed)

    def snapshot(self, capsule: str | None = None) -> str:
        return self.session.snapshot(capsule)

    def reset(self, capsule: str) -> None:
        self.session.reset(capsule)
