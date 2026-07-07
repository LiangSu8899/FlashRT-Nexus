"""Shared ctypes plumbing for Nexus action-chunk gates.

Binds the Nexus library's generic C surfaces (capsule model face, stage DAG,
action-chunk mode) so each real-model gate owns only its producer setup.
The action-chunk verbs can be bound under the current ABI prefix
(``nexus_action_chunk``) or the deprecated compat prefix
(``nexus_rtc_action_chunk``) to exercise the alias layer.
"""

from __future__ import annotations

import ctypes

CAP_OK = 0

NEXUS_AC_IDLE = 0
NEXUS_AC_PENDING = 1
NEXUS_AC_READY = 2
NEXUS_AC_FALLBACK = 3
NEXUS_AC_ERROR = 4

DEFAULT_PREFIX = "nexus_action_chunk"
COMPAT_PREFIX = "nexus_rtc_action_chunk"


class NexusActionChunkConfig(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32),
        ("action_stage", ctypes.c_uint64),
        ("output_port", ctypes.c_uint32),
        ("chunk_length", ctypes.c_uint32),
        ("action_bytes", ctypes.c_uint32),
        ("ring_slots", ctypes.c_uint32),
        ("execute_horizon", ctypes.c_uint32),
        ("deadline_ticks", ctypes.c_int32),
        ("reserved1", ctypes.c_uint32),
    ]


def bind_core(nx) -> None:
    """Type the generic capsule/stage-DAG entry points on the Nexus library."""
    P, U64, U32, I, C = (ctypes.c_void_p, ctypes.c_uint64, ctypes.c_uint32,
                         ctypes.c_int, ctypes.c_char_p)
    sigs = {
        "flashrt_adopt_model_runtime": (I, [P, ctypes.POINTER(P)]),
        "flashrt_model_close": (None, [P]),
        "cap_model_backend": (P, [P]),
        "cap_model_find_port": (I, [P, C]),
        "cap_model_port_buffer": (P, [P, U64]),
        "cap_model_port_bytes": (U64, [P, U64]),
        "cap_model_stage_stream": (I, [P, U64]),
        "cap_model_set_input": (I, [P, U32, P, U64, I]),
        "cap_model_get_output": (I, [P, U32, P, U64, ctypes.POINTER(U64), I]),
        "cap_model_tick": (I, [P, P]),
        "cap_ctx_create": (P, [P]),
        "cap_ctx_destroy": (None, [P]),
        "cap_swap": (I, [P, P, P, ctypes.c_size_t, I]),
        "cap_sync": (I, [P, I]),
        "nexus_stage_dag_create": (I, [P, P, ctypes.POINTER(P)]),
        "nexus_stage_dag_destroy": (None, [P]),
        "nexus_stage_dag_fire": (I, [P, U64]),
        "nexus_stage_dag_query": (I, [P, U64]),
        "nexus_stage_dag_sync": (I, [P, U64]),
    }
    for name, (res, args) in sigs.items():
        fn = getattr(nx, name)
        fn.restype = res
        fn.argtypes = args


class ActionChunkAbi:
    """Prefix-agnostic view of the action-chunk C ABI verbs."""

    _P = ctypes.c_void_p
    _U64 = ctypes.c_uint64
    _U32 = ctypes.c_uint32
    _I = ctypes.c_int

    VERBS = {
        "create": (_I, [_P, ctypes.POINTER(NexusActionChunkConfig),
                        ctypes.POINTER(_P)]),
        "create_for_output_port": (_I, [_P, _U64, _U32, _U32, _U32, _U32, _I,
                                        ctypes.POINTER(_P)]),
        "destroy": (None, [_P]),
        "request": (_I, [_P]),
        "poll": (_I, [_P]),
        "next_action": (_I, [_P, _P, _U64, ctypes.POINTER(_U64)]),
        "reset": (None, [_P]),
        "in_flight": (_I, [_P]),
        "has_active": (_I, [_P]),
        "remaining": (_U32, [_P]),
        "active_index": (_U32, [_P]),
        "completed": (_U64, [_P]),
        "emitted": (_U64, [_P]),
        "fallbacks": (_I, [_P]),
        "late_chunks": (_I, [_P]),
        "pending_ticks": (_U32, [_P]),
        "last_ready_ticks": (_U32, [_P]),
        "max_ready_ticks": (_U32, [_P]),
        "total_ready_ticks": (_U64, [_P]),
        "last_error": (_I, [_P]),
    }

    def __init__(self, nx, prefix: str = DEFAULT_PREFIX):
        self.prefix = prefix
        for verb, (res, args) in self.VERBS.items():
            fn = getattr(nx, f"{prefix}_{verb}")
            fn.restype = res
            fn.argtypes = args
            setattr(self, verb, fn)
