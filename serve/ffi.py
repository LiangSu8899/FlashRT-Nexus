"""ctypes bindings for the Nexus product shell."""

from __future__ import annotations

import ctypes


CAP_OK = 0
CAP_TIER_HOST = 1
NEXUS_AC_IDLE = 0
NEXUS_AC_PENDING = 1
NEXUS_AC_READY = 2
NEXUS_AC_FALLBACK = 3
NEXUS_AC_ERROR = 4

NEXUS_AC_PREPARE_NONE = 0
NEXUS_AC_CONSUME_PLAIN = 0
NEXUS_AC_CONSUME_SWITCH = 1
NEXUS_AC_CONSUME_TEMPORAL_FUSION = 2
NEXUS_AC_SWITCH_LATENCY = 0
NEXUS_AC_MISS_REPORT_ONLY = 0
NEXUS_AC_MISS_HOLD_LAST = 1
NEXUS_AC_DTYPE_F32 = 1
NEXUS_AC_REPR_ABSOLUTE = 0
FRT_RT_PIXEL_RGB8 = 0
FRT_RT_DTYPE_F32 = 1
FRT_RT_DTYPE_F16 = 2
FRT_RT_DTYPE_BF16 = 3
FRT_RT_PORT_SWAP = 0
FRT_RT_PORT_STAGED = 1
FRT_PI05_DTYPE_BFLOAT16 = 1
FRT_PI05_DTYPE_FLOAT16 = 2
FRT_PI05_DTYPE_FLOAT32 = 3


class FrtImageView(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("pixel_format", ctypes.c_uint32),
        ("data", ctypes.c_void_p),
        ("bytes", ctypes.c_uint64),
        ("width", ctypes.c_int32),
        ("height", ctypes.c_int32),
        ("stride_bytes", ctypes.c_int32),
        ("reserved", ctypes.c_uint32),
        ("timestamp_ns", ctypes.c_uint64),
    ]


class Pi05RuntimeConfig(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("num_views", ctypes.c_int),
        ("chunk", ctypes.c_int),
        ("model_action_dim", ctypes.c_int),
        ("robot_action_dim", ctypes.c_int),
        ("action_mean", ctypes.POINTER(ctypes.c_float)),
        ("n_action_mean", ctypes.c_uint64),
        ("action_stddev", ctypes.POINTER(ctypes.c_float)),
        ("n_action_stddev", ctypes.c_uint64),
        ("graph_name", ctypes.c_char_p),
        ("image_buffer_name", ctypes.c_char_p),
        ("action_buffer_name", ctypes.c_char_p),
        ("image_dtype", ctypes.c_int),
        ("action_dtype", ctypes.c_int),
    ]


RetainReleaseFn = ctypes.CFUNCTYPE(None, ctypes.c_void_p)


class FrtModelRuntimeV1(ctypes.Structure):
    _fields_ = [
        ("abi_version", ctypes.c_uint32),
        ("struct_size", ctypes.c_uint32),
        ("exp", ctypes.c_void_p),
        ("ports", ctypes.c_void_p),
        ("n_ports", ctypes.c_uint64),
        ("stages", ctypes.c_void_p),
        ("n_stages", ctypes.c_uint64),
        ("self", ctypes.c_void_p),
        ("verbs", ctypes.c_byte * 48),
        ("owner", ctypes.c_void_p),
        ("retain", RetainReleaseFn),
        ("release", RetainReleaseFn),
    ]


class CapRegion(ctypes.Structure):
    _fields_ = [
        ("buf", ctypes.c_void_p),
        ("off", ctypes.c_size_t),
        ("bytes", ctypes.c_size_t),
    ]


class CapBoundary(ctypes.Structure):
    _fields_ = [
        ("regions", ctypes.POINTER(CapRegion)),
        ("n_regions", ctypes.c_int),
        ("meta", ctypes.c_void_p),
        ("meta_len", ctypes.c_size_t),
    ]


class NexusActionChunkConfig(ctypes.Structure):
    """Mirror of the versioned ``nexus_action_chunk_config`` C ABI."""

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
        ("experimental", ctypes.c_uint8),
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


def bind_nexus(nx: ctypes.CDLL) -> None:
    p = ctypes.c_void_p
    u64 = ctypes.c_uint64
    u32 = ctypes.c_uint32
    i = ctypes.c_int
    c = ctypes.c_char_p
    sigs = {
        "cap_abi_version": (u32, []),
        "flashrt_adopt_model_runtime": (i, [p, ctypes.POINTER(p)]),
        "flashrt_model_close": (None, [p]),
        "cap_model_backend": (p, [p]),
        "cap_model_fingerprint": (u64, [p]),
        "cap_model_identity": (c, [p]),
        "cap_model_find_port": (i, [p, c]),
        "cap_model_port_buffer": (p, [p, u64]),
        "cap_model_port_bytes": (u64, [p, u64]),
        "cap_model_port_update": (u32, [p, u64]),
        "cap_model_stage_stream": (i, [p, u64]),
        "cap_model_n_stages": (u64, [p]),
        "cap_model_region_array": (p, [p]),
        "cap_model_region_count": (i, [p]),
        "cap_model_set_input": (i, [p, u32, p, u64, i]),
        "cap_model_get_output": (i, [p, u32, p, u64, ctypes.POINTER(u64), i]),
        "cap_model_tick": (i, [p, p]),
        "cap_model_last_error": (c, [p]),
        "cap_ctx_create": (p, [p]),
        "cap_ctx_destroy": (None, [p]),
        "cap_ctx_fingerprint": (u64, [p]),
        "cap_swap": (i, [p, p, p, ctypes.c_size_t, i]),
        "cap_sync": (i, [p, i]),
        "cap_snapshot": (p, [p, p, i, i]),
        "cap_serialize": (i, [p, p, p, ctypes.POINTER(ctypes.c_size_t)]),
        "cap_load": (p, [p, p, ctypes.c_size_t]),
        "cap_restore": (i, [p, p, i]),
        "cap_restore_into": (i, [p, p, ctypes.POINTER(CapRegion), i, i]),
        "cap_capsule_drop": (None, [p, p]),
        "nexus_stage_dag_create": (i, [p, p, ctypes.POINTER(p)]),
        "nexus_stage_dag_destroy": (None, [p]),
        "nexus_stage_dag_fire": (i, [p, u64]),
        "nexus_stage_dag_query": (i, [p, u64]),
        "nexus_stage_dag_sync": (i, [p, u64]),
        "nexus_action_chunk_create": (
            i, [p, ctypes.POINTER(NexusActionChunkConfig), ctypes.POINTER(p)]),
        "nexus_action_chunk_destroy": (None, [p]),
        "nexus_action_chunk_request": (i, [p]),
        "nexus_action_chunk_poll": (i, [p]),
        "nexus_action_chunk_next_action": (
            i, [p, p, u64, ctypes.POINTER(u64)]),
        "nexus_action_chunk_reset": (None, [p]),
        "nexus_action_chunk_in_flight": (i, [p]),
        "nexus_action_chunk_has_active": (i, [p]),
        "nexus_action_chunk_remaining": (u32, [p]),
        "nexus_action_chunk_completed": (u64, [p]),
        "nexus_action_chunk_emitted": (u64, [p]),
        "nexus_action_chunk_fallbacks": (i, [p]),
        "nexus_action_chunk_late_chunks": (i, [p]),
        "nexus_action_chunk_held_actions": (u64, [p]),
        "nexus_action_chunk_last_error": (i, [p]),
    }
    for name, (restype, argtypes) in sigs.items():
        fn = getattr(nx, name)
        fn.restype = restype
        fn.argtypes = argtypes


def bind_pi05(lib: ctypes.CDLL) -> None:
    lib.frt_pi05_model_runtime_create_over.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(Pi05RuntimeConfig),
        ctypes.POINTER(ctypes.c_void_p),
    ]
    lib.frt_pi05_model_runtime_create_over.restype = ctypes.c_int


def dtype_to_pi05(dtype: object) -> int:
    text = str(dtype).lower()
    if dtype == FRT_RT_DTYPE_BF16 or text in ("bf16", "bfloat16"):
        return FRT_PI05_DTYPE_BFLOAT16
    if dtype == FRT_RT_DTYPE_F16 or text in ("f16", "float16", "fp16"):
        return FRT_PI05_DTYPE_FLOAT16
    if dtype == FRT_RT_DTYPE_F32 or text in ("f32", "float32", "fp32"):
        return FRT_PI05_DTYPE_FLOAT32
    raise RuntimeError(f"unsupported Pi05 runtime dtype: {dtype!r}")
