"""Python host for Nexus's asynchronous action-chunk mode."""

from __future__ import annotations

import ctypes
import time
from dataclasses import dataclass
from typing import Any

import numpy as np

from .ffi import (
    CAP_OK,
    NEXUS_AC_CONSUME_PLAIN,
    NEXUS_AC_CONSUME_SWITCH,
    NEXUS_AC_CONSUME_TEMPORAL_FUSION,
    NEXUS_AC_DTYPE_F32,
    NEXUS_AC_ERROR,
    NEXUS_AC_FALLBACK,
    NEXUS_AC_IDLE,
    NEXUS_AC_MISS_HOLD_LAST,
    NEXUS_AC_MISS_REPORT_ONLY,
    NEXUS_AC_PENDING,
    NEXUS_AC_PREPARE_NONE,
    NEXUS_AC_READY,
    NEXUS_AC_REPR_ABSOLUTE,
    NEXUS_AC_SWITCH_LATENCY,
    NexusActionChunkConfig,
)


_STATES = {
    NEXUS_AC_IDLE: "idle",
    NEXUS_AC_PENDING: "pending",
    NEXUS_AC_READY: "ready",
    NEXUS_AC_FALLBACK: "fallback",
    NEXUS_AC_ERROR: "error",
}
_CONSUME = {
    "plain": NEXUS_AC_CONSUME_PLAIN,
    "switch": NEXUS_AC_CONSUME_SWITCH,
    "temporal_fusion": NEXUS_AC_CONSUME_TEMPORAL_FUSION,
}


@dataclass(frozen=True)
class ActionChunkOptions:
    execute_horizon: int = 1
    ring_slots: int = 3
    poll_budget: int = -1
    deadline_steps: int = 0
    consume: str = "plain"
    miss: str = "hold_last"
    fusion_decay: float = 0.1
    fusion_max_chunks: int = 3
    switch_offset: int = 0
    context_stage: int = 0
    action_stage: int | None = None


class ActionChunkSession:
    """Drive one split model runtime without blocking the control loop."""

    def __init__(self, session: Any, options: ActionChunkOptions):
        if options.consume not in _CONSUME:
            raise ValueError(f"unknown consume policy {options.consume!r}")
        if options.miss not in {"report", "hold_last"}:
            raise ValueError(f"unknown miss policy {options.miss!r}")
        self.session = session
        self.options = options
        self.nx = session.nx
        self._dag = ctypes.c_void_p()
        self._mode = ctypes.c_void_p()
        self._closed = False

        stages = int(self.nx.cap_model_n_stages(session.model))
        action_stage = stages - 1 if options.action_stage is None else options.action_stage
        if stages < 2 or not 0 <= options.context_stage < stages:
            raise ValueError("action chunks require a split model runtime")
        if not 0 <= action_stage < stages or action_stage == options.context_stage:
            raise ValueError("context and action stages must be distinct")
        self.context_stage = options.context_stage
        self.action_stage = action_stage

        rc = self.nx.nexus_stage_dag_create(
            session.ctx, session.model, ctypes.byref(self._dag))
        if rc != CAP_OK:
            raise RuntimeError(f"nexus_stage_dag_create rc={rc}")

        chunk_length, action_dim = session.action_shape
        config = NexusActionChunkConfig()
        config.struct_size = ctypes.sizeof(config)
        config.action_stage = action_stage
        config.output_port = session.ports["actions"]
        config.chunk_length = chunk_length
        config.action_bytes = action_dim * np.dtype(np.float32).itemsize
        config.ring_slots = options.ring_slots
        # The application stages a fresh observation and fires the context
        # stage before every request. Disable the mode's same-context
        # auto-prefetch; ``should_request`` exposes the host-side threshold.
        config.execute_horizon = 0
        config.poll_budget = options.poll_budget
        config.deadline_steps = options.deadline_steps
        config.prepare_policy = NEXUS_AC_PREPARE_NONE
        config.consume_policy = _CONSUME[options.consume]
        config.switch_mode = NEXUS_AC_SWITCH_LATENCY
        config.miss_policy = (
            NEXUS_AC_MISS_HOLD_LAST
            if options.miss == "hold_last"
            else NEXUS_AC_MISS_REPORT_ONLY
        )
        config.scalar_dtype = NEXUS_AC_DTYPE_F32
        config.action_representation = NEXUS_AC_REPR_ABSOLUTE
        config.fusion_decay = options.fusion_decay
        config.fusion_max_chunks = options.fusion_max_chunks
        config.switch_offset = options.switch_offset

        rc = self.nx.nexus_action_chunk_create(
            self._dag, ctypes.byref(config), ctypes.byref(self._mode))
        if rc != CAP_OK:
            self.nx.nexus_stage_dag_destroy(self._dag)
            self._dag = ctypes.c_void_p()
            raise RuntimeError(f"nexus_action_chunk_create rc={rc}")

    def close(self) -> None:
        if self._closed:
            return
        if self._mode:
            self.nx.nexus_action_chunk_destroy(self._mode)
            self._mode = ctypes.c_void_p()
        if self._dag:
            self.nx.nexus_stage_dag_destroy(self._dag)
            self._dag = ctypes.c_void_p()
        self._closed = True

    @property
    def state(self) -> str:
        return _STATES.get(int(self.nx.nexus_action_chunk_poll(self._mode)), "error")

    @property
    def in_flight(self) -> bool:
        return bool(self.nx.nexus_action_chunk_in_flight(self._mode))

    @property
    def has_active(self) -> bool:
        return bool(self.nx.nexus_action_chunk_has_active(self._mode))

    @property
    def remaining(self) -> int:
        return int(self.nx.nexus_action_chunk_remaining(self._mode))

    @property
    def should_request(self) -> bool:
        return (
            self.has_active
            and not self.in_flight
            and self.remaining <= self.options.execute_horizon
        )

    def request(self, images: list[np.ndarray], *, state: Any = None,
                prompt: str | None = None, seed: int | None = None) -> None:
        """Stage the latest observation and request its next action chunk."""
        with self.session.lock:
            self.session._stage_arrays_locked(
                images, state=state, prompt=prompt, seed=seed)
            self.nx.nexus_stage_dag_query(self._dag, self.context_stage)
            rc = self.nx.nexus_stage_dag_fire(self._dag, self.context_stage)
            if rc != CAP_OK:
                self.nx.nexus_stage_dag_sync(self._dag, self.context_stage)
                rc = self.nx.nexus_stage_dag_fire(
                    self._dag, self.context_stage)
            if rc != CAP_OK:
                raise RuntimeError(f"context stage fire rc={rc}")
            rc = self.nx.nexus_action_chunk_request(self._mode)
            if rc != CAP_OK:
                raise RuntimeError(f"action chunk request rc={rc}")

    def poll(self) -> str:
        state = int(self.nx.nexus_action_chunk_poll(self._mode))
        if state == NEXUS_AC_ERROR:
            error = int(self.nx.nexus_action_chunk_last_error(self._mode))
            raise RuntimeError(f"action chunk mode error rc={error}")
        return _STATES.get(state, "error")

    def wait_ready(self, timeout_s: float = 30.0) -> None:
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            if self.poll() == "ready":
                return
            time.sleep(0.0005)
        raise TimeoutError("action chunk did not become ready")

    def next_action(self) -> np.ndarray | None:
        action = np.empty(self.session.action_shape[1], dtype=np.float32)
        written = ctypes.c_uint64()
        held_before = int(
            self.nx.nexus_action_chunk_held_actions(self._mode))
        state = self.nx.nexus_action_chunk_next_action(
            self._mode, ctypes.c_void_p(action.ctypes.data), action.nbytes,
            ctypes.byref(written))
        if state == NEXUS_AC_FALLBACK:
            held_after = int(
                self.nx.nexus_action_chunk_held_actions(self._mode))
            return action if held_after > held_before else None
        if state in {NEXUS_AC_IDLE, NEXUS_AC_PENDING}:
            return None
        if state != NEXUS_AC_READY or int(written.value) != action.nbytes:
            raise RuntimeError(
                f"action chunk next_action state={state} written={written.value}")
        return action

    def reset(self) -> None:
        self.nx.nexus_action_chunk_reset(self._mode)

    def stats(self) -> dict[str, int]:
        return {
            "completed_chunks": int(
                self.nx.nexus_action_chunk_completed(self._mode)),
            "emitted_actions": int(
                self.nx.nexus_action_chunk_emitted(self._mode)),
            "fallbacks": int(
                self.nx.nexus_action_chunk_fallbacks(self._mode)),
            "late_chunks": int(
                self.nx.nexus_action_chunk_late_chunks(self._mode)),
            "held_actions": int(
                self.nx.nexus_action_chunk_held_actions(self._mode)),
        }
