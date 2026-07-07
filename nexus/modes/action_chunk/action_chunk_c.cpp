#include "nexus/modes/action_chunk/action_chunk_c.h"

#include "nexus/modes/action_chunk/action_chunk.h"

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

nexus::ActionChunkConfig convert(
        const nexus_action_chunk_config* in) {
    nexus::ActionChunkConfig out{};
    if (!in) return out;
    out.action_stage = in->action_stage;
    out.output_port = in->output_port;
    out.chunk_length = in->chunk_length;
    out.action_bytes = in->action_bytes;
    out.ring_slots = in->ring_slots;
    out.execute_horizon = in->execute_horizon;
    out.deadline_ticks = in->deadline_ticks;
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
    if (config && config->struct_size < sizeof(nexus_action_chunk_config))
        return CAP_ERR_VERSION;
    nexus::StageDagRunner* r = nexus_stage_dag_runner(runner);
    if (!r) return CAP_ERR_ARG;
    auto* h = new (std::nothrow) nexus_action_chunk_s(r, convert(config));
    if (!h) return CAP_ERR_NOMEM;
    *out = h;
    return CAP_OK;
}

extern "C" int nexus_action_chunk_create_for_output_port(
        nexus_stage_dag* runner, uint64_t action_stage, uint32_t output_port,
        uint32_t scalar_bytes, uint32_t ring_slots, uint32_t execute_horizon,
        int32_t deadline_ticks, nexus_action_chunk** out) {
    if (!runner || !out) return CAP_ERR_ARG;
    *out = nullptr;
    nexus::StageDagRunner* r = nexus_stage_dag_runner(runner);
    if (!r) return CAP_ERR_ARG;
    nexus::ActionChunkConfig cfg{};
    int rc = nexus::ActionChunkMode::config_from_output_port(
        r, action_stage, output_port, scalar_bytes, ring_slots,
        execute_horizon, deadline_ticks, &cfg);
    if (rc != CAP_OK) return rc;
    auto* h = new (std::nothrow) nexus_action_chunk_s(r, cfg);
    if (!h) return CAP_ERR_NOMEM;
    *out = h;
    return CAP_OK;
}

extern "C" void nexus_action_chunk_destroy(nexus_action_chunk* h) {
    delete h;
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

extern "C" void nexus_action_chunk_reset(nexus_action_chunk* h) {
    if (h) h->mode.reset();
}

extern "C" int nexus_action_chunk_in_flight(nexus_action_chunk* h) {
    return h ? (h->mode.in_flight() ? 1 : 0) : CAP_ERR_ARG;
}

extern "C" int nexus_action_chunk_has_active(nexus_action_chunk* h) {
    return h ? (h->mode.has_active_chunk() ? 1 : 0) : CAP_ERR_ARG;
}

extern "C" uint32_t nexus_action_chunk_remaining(
        nexus_action_chunk* h) {
    return h ? h->mode.remaining_actions() : 0;
}

extern "C" uint32_t nexus_action_chunk_active_index(
        nexus_action_chunk* h) {
    return h ? h->mode.active_index() : 0;
}

extern "C" uint64_t nexus_action_chunk_completed(
        nexus_action_chunk* h) {
    return h ? h->mode.completed_chunks() : 0;
}

extern "C" uint64_t nexus_action_chunk_emitted(
        nexus_action_chunk* h) {
    return h ? h->mode.emitted_actions() : 0;
}

extern "C" int nexus_action_chunk_fallbacks(nexus_action_chunk* h) {
    return h ? h->mode.fallbacks() : CAP_ERR_ARG;
}

extern "C" int nexus_action_chunk_late_chunks(nexus_action_chunk* h) {
    return h ? h->mode.late_chunks() : CAP_ERR_ARG;
}

extern "C" uint32_t nexus_action_chunk_pending_ticks(
        nexus_action_chunk* h) {
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

extern "C" int nexus_action_chunk_last_error(nexus_action_chunk* h) {
    return h ? h->mode.last_error() : CAP_ERR_ARG;
}
