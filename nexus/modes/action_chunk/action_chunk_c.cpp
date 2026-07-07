#include "nexus/modes/action_chunk/action_chunk_c.h"

#include "nexus/modes/action_chunk/action_chunk.h"

#include <cstddef>
#include <cstring>
#include <new>

namespace nexus {
class StageDagRunner;
}

nexus::StageDagRunner* nexus_stage_dag_runner(nexus_stage_dag*);

struct nexus_action_chunk_s {
    nexus::ActionChunkMode mode;
    nexus_action_chunk_s(nexus::StageDagRunner* runner,
                         const nexus::ActionChunkConfig& cfg)
        : mode(runner, cfg) {}
};

namespace {

/* A v1 config ends at reserved1; the bytes after it were struct padding in
 * the v1 layout, so nothing above this offset may be read from a v1 caller. */
constexpr uint32_t kConfigSizeV1 =
    offsetof(nexus_action_chunk_config, reserved2);

int check_version(const nexus_action_chunk_config* config) {
    if (!config) return CAP_OK;
    if (config->struct_size < kConfigSizeV1) return CAP_ERR_VERSION;
    if (config->struct_size > sizeof(nexus_action_chunk_config))
        return CAP_ERR_VERSION;
    return CAP_OK;
}

nexus::ActionChunkConfig convert(const nexus_action_chunk_config* in) {
    nexus::ActionChunkConfig out{};
    if (!in) return out;
    out.action_stage = in->action_stage;
    out.output_port = in->output_port;
    out.chunk_length = in->chunk_length;
    out.action_bytes = in->action_bytes;
    out.ring_slots = in->ring_slots;
    out.execute_horizon = in->execute_horizon;
    out.poll_budget = in->poll_budget;
    if (in->struct_size >= sizeof(nexus_action_chunk_config)) {
        out.deadline_steps = in->deadline_steps;
        out.prepare_policy = in->prepare_policy;
        out.consume_policy = in->consume_policy;
        out.switch_mode = in->switch_mode;
        out.miss_policy = in->miss_policy;
        out.scalar_dtype = in->scalar_dtype;
        out.action_representation = in->action_representation;
        out.distance_metric = in->distance_metric;
        out.state_dim = in->state_dim;
        out.candidates = in->candidates;
    }
    return out;
}

int state(nexus::ActionChunkState s) {
    switch (s) {
        case nexus::ActionChunkState::kIdle: return NEXUS_AC_IDLE;
        case nexus::ActionChunkState::kPending: return NEXUS_AC_PENDING;
        case nexus::ActionChunkState::kReady: return NEXUS_AC_READY;
        case nexus::ActionChunkState::kFallback: return NEXUS_AC_FALLBACK;
        case nexus::ActionChunkState::kError: return NEXUS_AC_ERROR;
    }
    return NEXUS_AC_ERROR;
}

}  // namespace

extern "C" int nexus_action_chunk_create(
        nexus_stage_dag* runner,
        const nexus_action_chunk_config* config,
        nexus_action_chunk** out) {
    if (!runner || !out) return CAP_ERR_ARG;
    *out = nullptr;
    int rc = check_version(config);
    if (rc != CAP_OK) return rc;
    nexus::StageDagRunner* r = nexus_stage_dag_runner(runner);
    if (!r) return CAP_ERR_ARG;
    nexus::ActionChunkConfig cfg = convert(config);
    rc = nexus::ActionChunkMode::validate(cfg);
    if (rc != CAP_OK) return rc;
    auto* h = new (std::nothrow) nexus_action_chunk_s(r, cfg);
    if (!h) return CAP_ERR_NOMEM;
    *out = h;
    return CAP_OK;
}

extern "C" int nexus_action_chunk_create_for_output_port(
        nexus_stage_dag* runner, uint64_t action_stage, uint32_t output_port,
        uint32_t scalar_bytes, uint32_t ring_slots, uint32_t execute_horizon,
        int32_t poll_budget, nexus_action_chunk** out) {
    if (!runner || !out) return CAP_ERR_ARG;
    *out = nullptr;
    nexus::StageDagRunner* r = nexus_stage_dag_runner(runner);
    if (!r) return CAP_ERR_ARG;
    nexus::ActionChunkConfig cfg{};
    int rc = nexus::ActionChunkMode::config_from_output_port(
        r, action_stage, output_port, scalar_bytes, ring_slots,
        execute_horizon, poll_budget, &cfg);
    if (rc != CAP_OK) return rc;
    auto* h = new (std::nothrow) nexus_action_chunk_s(r, cfg);
    if (!h) return CAP_ERR_NOMEM;
    *out = h;
    return CAP_OK;
}

extern "C" void nexus_action_chunk_destroy(nexus_action_chunk* h) {
    delete h;
}

extern "C" int nexus_action_chunk_begin_request(nexus_action_chunk* h) {
    return h ? h->mode.begin_request() : CAP_ERR_ARG;
}

extern "C" int nexus_action_chunk_commit_request(nexus_action_chunk* h) {
    return h ? h->mode.commit_request() : CAP_ERR_ARG;
}

extern "C" int nexus_action_chunk_request(nexus_action_chunk* h) {
    return h ? h->mode.request() : CAP_ERR_ARG;
}

extern "C" int nexus_action_chunk_poll(nexus_action_chunk* h) {
    return h ? state(h->mode.poll()) : NEXUS_AC_ERROR;
}

extern "C" int nexus_action_chunk_next_action(nexus_action_chunk* h,
                                              void* out, uint64_t capacity,
                                              uint64_t* written) {
    return h ? state(h->mode.next_action(out, capacity, written))
             : NEXUS_AC_ERROR;
}

extern "C" int nexus_action_chunk_advance_step(nexus_action_chunk* h) {
    return h ? h->mode.advance_step() : CAP_ERR_ARG;
}

extern "C" int nexus_action_chunk_sync_next_chunk(nexus_action_chunk* h) {
    return h ? state(h->mode.sync_next_chunk()) : NEXUS_AC_ERROR;
}

extern "C" void nexus_action_chunk_reset(nexus_action_chunk* h) {
    if (h) h->mode.reset();
}

extern "C" int nexus_action_chunk_set_state(nexus_action_chunk* h,
                                            const float* state_values,
                                            uint32_t dim) {
    return h ? h->mode.set_state(state_values, dim) : CAP_ERR_ARG;
}

extern "C" int nexus_action_chunk_set_state_action_indices(
        nexus_action_chunk* h, const uint32_t* indices, uint32_t n) {
    return h ? h->mode.set_state_action_indices(indices, n) : CAP_ERR_ARG;
}

extern "C" int nexus_action_chunk_in_flight(nexus_action_chunk* h) {
    return h ? (h->mode.in_flight() ? 1 : 0) : CAP_ERR_ARG;
}

extern "C" int nexus_action_chunk_has_active(nexus_action_chunk* h) {
    return h ? (h->mode.has_active_chunk() ? 1 : 0) : CAP_ERR_ARG;
}

extern "C" uint32_t nexus_action_chunk_remaining(nexus_action_chunk* h) {
    return h ? h->mode.remaining_actions() : 0;
}

extern "C" uint32_t nexus_action_chunk_active_index(nexus_action_chunk* h) {
    return h ? h->mode.active_index() : 0;
}

extern "C" uint64_t nexus_action_chunk_completed(nexus_action_chunk* h) {
    return h ? h->mode.completed_chunks() : 0;
}

extern "C" uint64_t nexus_action_chunk_emitted(nexus_action_chunk* h) {
    return h ? h->mode.emitted_actions() : 0;
}

extern "C" int nexus_action_chunk_fallbacks(nexus_action_chunk* h) {
    return h ? h->mode.fallbacks() : CAP_ERR_ARG;
}

extern "C" int nexus_action_chunk_late_chunks(nexus_action_chunk* h) {
    return h ? h->mode.late_chunks() : CAP_ERR_ARG;
}

extern "C" uint32_t nexus_action_chunk_pending_ticks(nexus_action_chunk* h) {
    return h ? h->mode.pending_ticks() : 0;
}

extern "C" uint32_t nexus_action_chunk_last_ready_ticks(
        nexus_action_chunk* h) {
    return h ? h->mode.last_ready_ticks() : 0;
}

extern "C" uint32_t nexus_action_chunk_max_ready_ticks(
        nexus_action_chunk* h) {
    return h ? h->mode.max_ready_ticks() : 0;
}

extern "C" uint64_t nexus_action_chunk_total_ready_ticks(
        nexus_action_chunk* h) {
    return h ? h->mode.total_ready_ticks() : 0;
}

extern "C" uint64_t nexus_action_chunk_action_step(nexus_action_chunk* h) {
    return h ? h->mode.action_step() : 0;
}

extern "C" uint64_t nexus_action_chunk_held_actions(nexus_action_chunk* h) {
    return h ? h->mode.held_actions() : 0;
}

extern "C" uint64_t nexus_action_chunk_prepared_requests(
        nexus_action_chunk* h) {
    return h ? h->mode.prepared_requests() : 0;
}

extern "C" uint64_t nexus_action_chunk_state_updates(nexus_action_chunk* h) {
    return h ? h->mode.state_updates() : 0;
}

extern "C" uint32_t nexus_action_chunk_last_d_steps(nexus_action_chunk* h) {
    return h ? h->mode.last_d_steps() : 0;
}

extern "C" int nexus_action_chunk_last_error(nexus_action_chunk* h) {
    return h ? h->mode.last_error() : CAP_ERR_ARG;
}
