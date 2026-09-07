"""In-process Nexus serving API.

This is the first no-HTTP entry: an application owns the process/thread and
calls the adopted model session directly with image/state buffers.
"""

from __future__ import annotations

from collections.abc import Mapping
from os import PathLike
from types import TracebackType
from typing import Any

import numpy as np

from .deployment import Deployment, open_deployment
from .session import ActArrayResult


class EmbeddedSession:
    def __init__(self, deployment: Deployment):
        self._deployment = deployment
        self.session = deployment.session
        self._action_chunk = None

    @classmethod
    def open(
        cls,
        manifest: str | PathLike[str] | Mapping[str, Any],
    ) -> "EmbeddedSession":
        return cls(open_deployment(manifest))

    def close(self) -> None:
        if self._action_chunk is not None:
            self._action_chunk.close()
            self._action_chunk = None
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
            prompt: str | None = None,
            seed: int | None = None) -> ActArrayResult:
        return self.session.act_arrays(
            images, state=state, prompt=prompt, seed=seed)

    def snapshot(self, capsule: str | None = None) -> str:
        return self.session.snapshot(capsule)

    def reset(self, capsule: str) -> None:
        self.session.reset(capsule)

    def action_chunks(self, **kwargs):
        """Open Nexus's asynchronous action-chunk mode for this session."""
        if self._action_chunk is not None:
            return self._action_chunk
        from .action_chunk import ActionChunkOptions, ActionChunkSession

        self._action_chunk = ActionChunkSession(
            self.session, ActionChunkOptions(**kwargs))
        return self._action_chunk
