/* nexus/modes/action_chunk/action_chunk_c.h — C ABI for Nexus action chunks.
 *
 * Versioning: the config is struct_size-gated. A v1-sized config (through
 * `reserved1`, 48 bytes) is accepted and every v2 field takes its default,
 * reproducing v1 behavior exactly. A struct_size larger than this header's
 * sizeof is a version mismatch (CAP_ERR_VERSION). Unknown enum values and
 * unsupported policy combinations are rejected at create (CAP_ERR_ARG),
 * never at tick time.
 */
#ifndef NEXUS_MODES_ACTION_CHUNK_C_H
#define NEXUS_MODES_ACTION_CHUNK_C_H

#include "nexus/schedulers/stage_dag_c.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NEXUS_AC_NO_OUTPUT_PORT 0xffffffffu

enum nexus_action_chunk_state {
    NEXUS_AC_IDLE = 0,
    NEXUS_AC_PENDING = 1,
    NEXUS_AC_READY = 2,
    NEXUS_AC_FALLBACK = 3,
    NEXUS_AC_ERROR = 4
};

enum nexus_ac_prepare_policy {
    NEXUS_AC_PREPARE_NONE = 0,
    NEXUS_AC_PREPARE_PROJECTED_STATE = 1,
    NEXUS_AC_PREPARE_PREV_CHUNK_PREFIX = 2
};

enum nexus_ac_consume_policy {
    NEXUS_AC_CONSUME_PLAIN = 0,
    NEXUS_AC_CONSUME_SWITCH = 1,
    NEXUS_AC_CONSUME_TEMPORAL_FUSION = 2
};

enum nexus_ac_switch_mode {
    NEXUS_AC_SWITCH_LATENCY = 0,
    NEXUS_AC_SWITCH_STATE = 1
};

enum nexus_ac_miss_policy {
    NEXUS_AC_MISS_REPORT_ONLY = 0,
    NEXUS_AC_MISS_HOLD_LAST = 1
};

enum nexus_ac_dtype {
    NEXUS_AC_DTYPE_RAW = 0,
    NEXUS_AC_DTYPE_F32 = 1,
    NEXUS_AC_DTYPE_BF16 = 2,
    NEXUS_AC_DTYPE_F16 = 3
};

enum nexus_ac_action_repr {
    NEXUS_AC_REPR_ABSOLUTE = 0,
    NEXUS_AC_REPR_DELTA_CUMULATIVE = 1,
    NEXUS_AC_REPR_DELTA_FROM_START = 2
};

typedef struct nexus_action_chunk_config {
    uint32_t struct_size;       /* = sizeof(nexus_action_chunk_config)      */
    uint32_t reserved;
    uint64_t action_stage;
    uint32_t output_port;       /* NEXUS_AC_NO_OUTPUT_PORT disables copy    */
    uint32_t chunk_length;      /* actions per completed chunk              */
    uint32_t action_bytes;      /* bytes per action emitted by next_action  */
    uint32_t ring_slots;        /* allocated at create                      */
    uint32_t execute_horizon;   /* prefetch when remaining <= this          */
    union {
        int32_t poll_budget;    /* pending-poll watchdog; <0 disables       */
        int32_t deadline_ticks; /* legacy name for the same field           */
    };
    uint32_t reserved1;
    uint32_t reserved2;         /* v1 tail padding made explicit            */
    /* -- v2 fields (defaults reproduce v1 behavior) ---------------------- */
    int32_t  deadline_steps;    /* controller-step deadline; <=0 disables   */
    uint8_t  prepare_policy;    /* nexus_ac_prepare_policy                  */
    uint8_t  consume_policy;    /* nexus_ac_consume_policy                  */
    uint8_t  switch_mode;       /* nexus_ac_switch_mode                     */
    uint8_t  miss_policy;       /* nexus_ac_miss_policy                     */
    uint8_t  scalar_dtype;      /* nexus_ac_dtype                           */
    uint8_t  action_representation;  /* nexus_ac_action_repr                */
    uint8_t  distance_metric;   /* 0 = l1, 1 = l2                           */
    uint8_t  experimental;      /* 1 unlocks experimental policy pairings   */
    uint32_t state_dim;         /* 0 = no state feed                        */
    uint32_t candidates;        /* 0 or 1 = single fire (reserved)          */
    uint32_t reserved4;
    double   fusion_decay;      /* f64: the reference semantics are f64     */
    uint32_t fusion_max_chunks; /* 0 = default 3                            */
    int32_t  switch_offset;     /* switch(latency): idx = clip(d + offset)  */
    uint32_t lookahead_steps;   /* projected_state: delta steps integrated  */
    uint32_t state_input_port;  /* +1 encoded STATE/F32/STAGED input port   */
    uint32_t prefix_len;        /* prev_chunk_prefix: capture-time length   */
    uint32_t prev_chunk_port;   /* +1 encoded: 0 = host transport           */
    uint32_t raw_out_port;      /* +1 encoded raw-chunk output port         */
    uint32_t raw_action_bytes;  /* bytes per raw action row                 */
} nexus_action_chunk_config;

typedef struct nexus_action_chunk_s nexus_action_chunk;

int  nexus_action_chunk_create(nexus_stage_dag*,
                               const nexus_action_chunk_config*,
                               nexus_action_chunk** out);
/* Infer chunk_length/action_bytes from output_port.shape:
 * shape[0] = chunk length, shape[1:] = one action, scalar_bytes = one
 * postprocessed scalar emitted by cap_model_get_output. */
int  nexus_action_chunk_create_for_output_port(
                               nexus_stage_dag*,
                               uint64_t action_stage,
                               uint32_t output_port,
                               uint32_t scalar_bytes,
                               uint32_t ring_slots,
                               uint32_t execute_horizon,
                               int32_t poll_budget,
                               nexus_action_chunk** out);
void nexus_action_chunk_destroy(nexus_action_chunk*);

/* Two-phase request: begin runs the prepare step (idempotent), commit
 * fires. request = begin + commit. Hosts that inject staged inputs call
 * them separately. */
int  nexus_action_chunk_begin_request(nexus_action_chunk*);
int  nexus_action_chunk_commit_request(nexus_action_chunk*);
int  nexus_action_chunk_request(nexus_action_chunk*);
int  nexus_action_chunk_poll(nexus_action_chunk*);
int  nexus_action_chunk_next_action(nexus_action_chunk*,
                                    void* out, uint64_t capacity,
                                    uint64_t* written);
/* Advance the controller-step grid without consuming. */
int  nexus_action_chunk_advance_step(nexus_action_chunk*);
/* Explicit blocking verb: sync the action stage, then consume; returns a
 * nexus_action_chunk_state like poll. */
int  nexus_action_chunk_sync_next_chunk(nexus_action_chunk*);
void nexus_action_chunk_reset(nexus_action_chunk*);

/* State feed (dim must equal config.state_dim; values must be finite). */
int  nexus_action_chunk_set_state(nexus_action_chunk*,
                                  const float* state, uint32_t dim);
/* One entry per state dimension. UINT32_MAX preserves that dimension instead
 * of integrating an action column. Frozen after the first request. */
int  nexus_action_chunk_set_state_action_indices(nexus_action_chunk*,
                                                 const uint32_t* indices,
                                                 uint32_t n);
/* projected_state prepare: read the value computed by the last begin_request.
 * Hosts may use it when state_input_port selects host transport. */
int  nexus_action_chunk_projected_state(nexus_action_chunk*,
                                        float* out, uint32_t capacity_dims,
                                        uint32_t* written_dims);
/* prev_chunk_prefix prepare: read the re-indexed previous raw chunk staged
 * by the last begin_request for host-transport injection. */
int  nexus_action_chunk_prev_chunk_staged(nexus_action_chunk*,
                                          void* out, uint64_t capacity,
                                          uint64_t* written);

int      nexus_action_chunk_in_flight(nexus_action_chunk*);
int      nexus_action_chunk_has_active(nexus_action_chunk*);
uint32_t nexus_action_chunk_remaining(nexus_action_chunk*);
uint32_t nexus_action_chunk_active_index(nexus_action_chunk*);
uint64_t nexus_action_chunk_completed(nexus_action_chunk*);
uint64_t nexus_action_chunk_emitted(nexus_action_chunk*);
int      nexus_action_chunk_fallbacks(nexus_action_chunk*);
int      nexus_action_chunk_late_chunks(nexus_action_chunk*);
uint32_t nexus_action_chunk_pending_ticks(nexus_action_chunk*);
uint32_t nexus_action_chunk_last_ready_ticks(nexus_action_chunk*);
uint32_t nexus_action_chunk_max_ready_ticks(nexus_action_chunk*);
uint64_t nexus_action_chunk_total_ready_ticks(nexus_action_chunk*);
uint64_t nexus_action_chunk_action_step(nexus_action_chunk*);
uint64_t nexus_action_chunk_held_actions(nexus_action_chunk*);
uint64_t nexus_action_chunk_prepared_requests(nexus_action_chunk*);
uint64_t nexus_action_chunk_state_updates(nexus_action_chunk*);
uint32_t nexus_action_chunk_last_d_steps(nexus_action_chunk*);
int      nexus_action_chunk_seated_waiting(nexus_action_chunk*);
uint64_t nexus_action_chunk_active_start_step(nexus_action_chunk*);
uint32_t nexus_action_chunk_projected_count(nexus_action_chunk*);
int      nexus_action_chunk_last_error(nexus_action_chunk*);

#ifdef __cplusplus
}
#endif

#endif  /* NEXUS_MODES_ACTION_CHUNK_C_H */
