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


NEXUS_AC_PREPARE_NONE = 0
NEXUS_AC_PREPARE_PROJECTED_STATE = 1
NEXUS_AC_PREPARE_PREV_CHUNK_PREFIX = 2
NEXUS_AC_CONSUME_PLAIN = 0
NEXUS_AC_CONSUME_SWITCH = 1
NEXUS_AC_CONSUME_TEMPORAL_FUSION = 2
NEXUS_AC_SWITCH_LATENCY = 0
NEXUS_AC_SWITCH_STATE = 1
NEXUS_AC_DTYPE_RAW = 0
NEXUS_AC_DTYPE_F32 = 1


class NexusActionChunkConfig(ctypes.Structure):
    """Mirror of nexus_action_chunk_config (v2). A v1 caller may pass
    struct_size = 48 (fields through reserved1 plus tail padding)."""

    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32),
        ("action_stage", ctypes.c_uint64),
        ("output_port", ctypes.c_uint32),
        ("chunk_length", ctypes.c_uint32),
        ("action_bytes", ctypes.c_uint32),
        ("ring_slots", ctypes.c_uint32),
        ("execute_horizon", ctypes.c_uint32),
        ("poll_budget", ctypes.c_int32),
        ("reserved1", ctypes.c_uint32),
        ("reserved2", ctypes.c_uint32),
        ("deadline_steps", ctypes.c_int32),
        ("prepare_policy", ctypes.c_uint8),
        ("consume_policy", ctypes.c_uint8),
        ("switch_mode", ctypes.c_uint8),
        ("miss_policy", ctypes.c_uint8),
        ("scalar_dtype", ctypes.c_uint8),
        ("action_representation", ctypes.c_uint8),
        ("distance_metric", ctypes.c_uint8),
        ("reserved3", ctypes.c_uint8),
        ("state_dim", ctypes.c_uint32),
        ("candidates", ctypes.c_uint32),
        ("reserved4", ctypes.c_uint32),
        ("fusion_decay", ctypes.c_double),
        ("fusion_max_chunks", ctypes.c_uint32),
        ("switch_offset", ctypes.c_int32),
        ("lookahead_steps", ctypes.c_uint32),
        ("state_input_port", ctypes.c_uint32),
        ("prefix_len", ctypes.c_uint32),
        ("prev_chunk_port", ctypes.c_uint32),
        ("raw_out_port", ctypes.c_uint32),
        ("raw_action_bytes", ctypes.c_uint32),
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
        "begin_request": (_I, [_P]),
        "commit_request": (_I, [_P]),
        "request": (_I, [_P]),
        "poll": (_I, [_P]),
        "next_action": (_I, [_P, _P, _U64, ctypes.POINTER(_U64)]),
        "advance_step": (_I, [_P]),
        "sync_next_chunk": (_I, [_P]),
        "reset": (None, [_P]),
        "set_state": (_I, [_P, ctypes.POINTER(ctypes.c_float), _U32]),
        "set_state_action_indices": (_I, [_P, ctypes.POINTER(ctypes.c_uint32),
                                          _U32]),
        "projected_state": (_I, [_P, ctypes.POINTER(ctypes.c_float), _U32,
                                 ctypes.POINTER(_U32)]),
        "prev_chunk_staged": (_I, [_P, _P, _U64, ctypes.POINTER(_U64)]),
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
        "action_step": (_U64, [_P]),
        "held_actions": (_U64, [_P]),
        "prepared_requests": (_U64, [_P]),
        "state_updates": (_U64, [_P]),
        "last_d_steps": (_U32, [_P]),
        "seated_waiting": (_I, [_P]),
        "active_start_step": (_U64, [_P]),
        "projected_count": (_U32, [_P]),
        "last_error": (_I, [_P]),
    }

    def __init__(self, nx, prefix: str = DEFAULT_PREFIX):
        self.prefix = prefix
        for verb, (res, args) in self.VERBS.items():
            try:
                fn = getattr(nx, f"{prefix}_{verb}")
            except AttributeError:
                # The deprecated compat prefix exposes only the v1 verbs.
                setattr(self, verb, None)
                continue
            fn.restype = res
            fn.argtypes = args
            setattr(self, verb, fn)
