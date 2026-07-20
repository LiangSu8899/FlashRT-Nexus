/* capsule/model_runtime.h — the standard face of one adopted, tickable model.
 *
 * The first L2 (host/framework) surface: schedulers, sessions, and runtime
 * loops code against THIS, in capsule types only — never against the FlashRT
 * export or a model's pipeline. The FlashRT adapter constructs it by adopting
 * frt_model_runtime_v1.
 *
 * Data first, verbs as sugar:
 *   ports  — every dynamic input/output with its UPDATE CLASS. SWAP ports are
 *            wired capsule buffers: the host writes them with cap_swap (the
 *            microsecond lane, zero model code). STAGED ports go through the
 *            producer's set_input/get_output. SETUP is illegal in a tick.
 *   stages — the subgraph DAG as fire-ready cap_stage entries plus a prepared
 *            cap_schedule; a scheduling host fires/overlaps stages itself,
 *            cap_model_tick is the one-call sequential sugar.
 *   regions— the restorable boundary, ready for cap_snapshot/cap_restore.
 *
 * Hot contract (pinned by the adoption test): updating SWAP or STAGED ports
 * between replays never recaptures, never allocates in the model, never
 * rebinds graph pointers — replay output tracks buffer contents. Shape-bucket
 * misses are handled by `prepare` in the warm phase, never inside a tick.
 *
 * This header stays mechanism-only: no session, no cadence policy, no
 * modality processing. It is data + pass-through verbs + two lookups.
 */
#ifndef CAPSULE_MODEL_RUNTIME_H
#define CAPSULE_MODEL_RUNTIME_H

#include "capsule/capsule.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Producer-v1 schema values mirrored in capsule names. These are routing
 * constants for L2 hosts/modes; capsule core does not interpret them. */
enum cap_model_modality {
    CAP_MODEL_MOD_TENSOR = 0,
    CAP_MODEL_MOD_IMAGE = 1,
    CAP_MODEL_MOD_TEXT = 2,
    CAP_MODEL_MOD_STATE = 3,
    CAP_MODEL_MOD_ACTION = 4,
    CAP_MODEL_MOD_AUDIO = 5,
    CAP_MODEL_MOD_DEPTH = 6,
    CAP_MODEL_MOD_FORCE = 7
};

enum cap_model_dtype {
    CAP_MODEL_DTYPE_U8 = 0,
    CAP_MODEL_DTYPE_F32 = 1,
    CAP_MODEL_DTYPE_F16 = 2,
    CAP_MODEL_DTYPE_BF16 = 3,
    CAP_MODEL_DTYPE_I32 = 4,
    CAP_MODEL_DTYPE_I64 = 5
};

enum cap_model_port_direction {
    CAP_MODEL_PORT_IN = 0,
    CAP_MODEL_PORT_OUT = 1
};

enum cap_model_port_update {
    CAP_MODEL_PORT_SWAP = 0,
    CAP_MODEL_PORT_STAGED = 1,
    CAP_MODEL_PORT_SETUP = 2
};

enum cap_model_executor_kind {
    CAP_MODEL_EXECUTOR_GRAPH = 0,
    CAP_MODEL_EXECUTOR_OPAQUE = 1
};

/* Modality / dtype / layout / direction / update values mirror the producer
 * ABI (append-only after v1). The core never interprets them; they are
 * host-side routing data. */
typedef struct cap_model_port {
    const char* name;
    uint32_t modality, dtype, layout;
    uint32_t direction;          /* enum cap_model_port_direction          */
    uint32_t update;             /* enum cap_model_port_update             */
    uint32_t required;
    const int64_t* shape; uint32_t rank;
    uint32_t cadence_hint_hz;
    cap_buffer buffer;           /* SWAP window as a capsule handle (null = */
    uint64_t offset, bytes;      /* staged-only port)                       */
} cap_model_port;

typedef struct cap_model_stage {
    const char* name;            /* the underlying graph's name             */
    cap_graph graph;
    cap_shape_key key;
    int stream;                  /* capsule stream index, fire-ready        */
    const uint32_t* after; uint32_t n_after;   /* stage-index dependencies  */
} cap_model_stage;

typedef struct cap_model_runtime {
    cap_backend* backend;        /* initialized; pass to cap_ctx_create     */

    const cap_model_port*  ports;  uint64_t n_ports;
    const cap_model_stage* stages; uint64_t n_stages;
    cap_region* regions;           uint64_t n_regions;

    /* Prepared from the stage DAG at adoption: cadence 1/1 CAP_EVERY stages
     * plus the after-edges, ready for cap_drive_tick. */
    cap_schedule schedule;

    /* Pre-created by the adapter, one entry per stage (null where no later
     * stage depends on it). With these, cap_model_tick is ALLOCATION-FREE —
     * a true hot verb (cap_drive_tick remains the allocating fallback for
     * hand-built runtimes that leave this null). */
    cap_event* stage_events;

    uint64_t fingerprint;
    const char* identity;

    /* Producer verbs, passed through verbatim (the hot contract applies).
     * Any entry may be null — probe before calling. */
    void* self;
    int (*set_input)(void* self, uint32_t port, const void* data,
                     uint64_t bytes, int stream);
    int (*get_output)(void* self, uint32_t port, void* out, uint64_t capacity,
                      uint64_t* written, int stream);   /* bytes */
    int (*prepare)(void* self, uint32_t graph, cap_shape_key key); /* WARM */
    int (*step)(void* self);
    const char* (*last_error)(void* self);

    void* impl;                  /* adapter internals; owned by the adapter */
} cap_model_runtime;

/* Port lookup by name: index into `ports`, or negative when absent. */
int cap_model_find_port(const cap_model_runtime*, const char* name);

/* Fire one stage through the core (equivalent cap_fire on the prepared
 * cap_stage) — the building block scheduling hosts use directly. */
int cap_model_fire(cap_ctx, const cap_model_runtime*, uint64_t stage_index);

/* Execute one stage through its declared mechanism. GRAPH preserves the
 * existing cap_fire path. OPAQUE is synchronous and complete on return. */
uint32_t cap_model_stage_executor_kind(const cap_model_runtime*,
                                       uint64_t stage_index);
int cap_model_execute_stage(cap_ctx, const cap_model_runtime*,
                            uint64_t stage_index);

/* Sugar: run the whole declared stage DAG once (cap_drive_tick over the
 * prepared schedule). Hosts that overlap/interrupt fire stages themselves. */
int cap_model_tick(cap_ctx, const cap_model_runtime*);

/* Model-level state is available only when every stage is GRAPH-backed and
 * the adopted runtime declares restorable regions. OPAQUE and step-only
 * runtimes fail closed instead of fabricating snapshot semantics. */
int cap_model_state_status(const cap_model_runtime*);
cap_capsule cap_model_snapshot(cap_ctx, const cap_model_runtime*, int tier,
                               int stream);
int cap_model_restore(cap_ctx, const cap_model_runtime*, cap_capsule,
                      int stream);
int cap_model_restore_into(cap_ctx, const cap_model_runtime*, cap_capsule,
                           int stream);

/* Flat accessors so dynamic-language hosts (ctypes/FFI) can drive an adopted
 * model without mirroring the structs above. */
cap_backend*          cap_model_backend    (cap_model_runtime*);
uint64_t              cap_model_fingerprint(const cap_model_runtime*);
const char*           cap_model_identity   (const cap_model_runtime*);
uint64_t              cap_model_n_ports    (const cap_model_runtime*);
cap_buffer            cap_model_port_buffer(const cap_model_runtime*, uint64_t port);
uint64_t              cap_model_port_bytes (const cap_model_runtime*, uint64_t port);
uint32_t              cap_model_port_update(const cap_model_runtime*, uint64_t port);
uint64_t              cap_model_n_stages   (const cap_model_runtime*);
int                   cap_model_stage_stream(const cap_model_runtime*, uint64_t stage);
cap_region*           cap_model_region_array(const cap_model_runtime*);
int                   cap_model_region_count(const cap_model_runtime*);
int                   cap_model_set_input  (cap_model_runtime*, uint32_t port,
                                            const void* data, uint64_t bytes,
                                            int stream);
int                   cap_model_get_output (cap_model_runtime*, uint32_t port,
                                            void* out, uint64_t capacity,
                                            uint64_t* written, int stream);
const char*           cap_model_last_error (cap_model_runtime*);

#ifdef __cplusplus
}
#endif

#endif /* CAPSULE_MODEL_RUNTIME_H */
