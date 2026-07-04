#include "nexus/modes/rtc_action_chunk/rtc_action_chunk_c.h"

#include "nexus/modes/rtc_action_chunk/rtc_action_chunk.h"

#include <new>

namespace nexus {
class StageDagRunner;
}

nexus::StageDagRunner* nexus_stage_dag_runner(nexus_stage_dag*);

struct nexus_rtc_action_chunk_s {
    nexus::RtcActionChunkMode mode;
    nexus_rtc_action_chunk_s(nexus::StageDagRunner* runner,
                             const nexus::RtcActionChunkConfig& cfg)
        : mode(runner, cfg) {}
};

namespace {

nexus::RtcActionChunkConfig convert(
        const nexus_rtc_action_chunk_config* in) {
    nexus::RtcActionChunkConfig out{};
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

int state(nexus::RtcChunkState s) {
    switch (s) {
        case nexus::RtcChunkState::kIdle: return NEXUS_RTC_IDLE;
        case nexus::RtcChunkState::kPending: return NEXUS_RTC_PENDING;
        case nexus::RtcChunkState::kReady: return NEXUS_RTC_READY;
        case nexus::RtcChunkState::kFallback: return NEXUS_RTC_FALLBACK;
        case nexus::RtcChunkState::kError: return NEXUS_RTC_ERROR;
    }
    return NEXUS_RTC_ERROR;
}

}  // namespace

extern "C" int nexus_rtc_action_chunk_create(
        nexus_stage_dag* runner,
        const nexus_rtc_action_chunk_config* config,
        nexus_rtc_action_chunk** out) {
    if (!runner || !out) return CAP_ERR_ARG;
    *out = nullptr;
    if (config && config->struct_size < sizeof(nexus_rtc_action_chunk_config))
        return CAP_ERR_VERSION;
    nexus::StageDagRunner* r = nexus_stage_dag_runner(runner);
    if (!r) return CAP_ERR_ARG;
    auto* h = new (std::nothrow) nexus_rtc_action_chunk_s(r, convert(config));
    if (!h) return CAP_ERR_NOMEM;
    *out = h;
    return CAP_OK;
}

extern "C" int nexus_rtc_action_chunk_create_for_output_port(
        nexus_stage_dag* runner, uint64_t action_stage, uint32_t output_port,
        uint32_t scalar_bytes, uint32_t ring_slots, uint32_t execute_horizon,
        int32_t deadline_ticks, nexus_rtc_action_chunk** out) {
    if (!runner || !out) return CAP_ERR_ARG;
    *out = nullptr;
    nexus::StageDagRunner* r = nexus_stage_dag_runner(runner);
    if (!r) return CAP_ERR_ARG;
    nexus::RtcActionChunkConfig cfg{};
    int rc = nexus::RtcActionChunkMode::config_from_output_port(
        r, action_stage, output_port, scalar_bytes, ring_slots,
        execute_horizon, deadline_ticks, &cfg);
    if (rc != CAP_OK) return rc;
    auto* h = new (std::nothrow) nexus_rtc_action_chunk_s(r, cfg);
    if (!h) return CAP_ERR_NOMEM;
    *out = h;
    return CAP_OK;
}

extern "C" void nexus_rtc_action_chunk_destroy(nexus_rtc_action_chunk* h) {
    delete h;
}

extern "C" int nexus_rtc_action_chunk_request(nexus_rtc_action_chunk* h) {
    return h ? h->mode.request() : CAP_ERR_ARG;
}

extern "C" int nexus_rtc_action_chunk_poll(nexus_rtc_action_chunk* h) {
    return h ? state(h->mode.poll()) : NEXUS_RTC_ERROR;
}

extern "C" int nexus_rtc_action_chunk_next_action(nexus_rtc_action_chunk* h,
                                                  void* out, uint64_t capacity,
                                                  uint64_t* written) {
    return h ? state(h->mode.next_action(out, capacity, written))
             : NEXUS_RTC_ERROR;
}

extern "C" void nexus_rtc_action_chunk_reset(nexus_rtc_action_chunk* h) {
    if (h) h->mode.reset();
}

extern "C" int nexus_rtc_action_chunk_in_flight(nexus_rtc_action_chunk* h) {
    return h ? (h->mode.in_flight() ? 1 : 0) : CAP_ERR_ARG;
}

extern "C" int nexus_rtc_action_chunk_has_active(nexus_rtc_action_chunk* h) {
    return h ? (h->mode.has_active_chunk() ? 1 : 0) : CAP_ERR_ARG;
}

extern "C" uint32_t nexus_rtc_action_chunk_remaining(
        nexus_rtc_action_chunk* h) {
    return h ? h->mode.remaining_actions() : 0;
}

extern "C" uint32_t nexus_rtc_action_chunk_active_index(
        nexus_rtc_action_chunk* h) {
    return h ? h->mode.active_index() : 0;
}

extern "C" uint64_t nexus_rtc_action_chunk_completed(
        nexus_rtc_action_chunk* h) {
    return h ? h->mode.completed_chunks() : 0;
}

extern "C" uint64_t nexus_rtc_action_chunk_emitted(
        nexus_rtc_action_chunk* h) {
    return h ? h->mode.emitted_actions() : 0;
}

extern "C" int nexus_rtc_action_chunk_fallbacks(nexus_rtc_action_chunk* h) {
    return h ? h->mode.fallbacks() : CAP_ERR_ARG;
}

extern "C" int nexus_rtc_action_chunk_late_chunks(nexus_rtc_action_chunk* h) {
    return h ? h->mode.late_chunks() : CAP_ERR_ARG;
}

extern "C" uint32_t nexus_rtc_action_chunk_pending_ticks(
        nexus_rtc_action_chunk* h) {
    return h ? h->mode.pending_ticks() : 0;
}

extern "C" uint32_t nexus_rtc_action_chunk_last_ready_ticks(
        nexus_rtc_action_chunk* h) {
    return h ? h->mode.last_ready_ticks() : 0;
}

extern "C" uint32_t nexus_rtc_action_chunk_max_ready_ticks(
        nexus_rtc_action_chunk* h) {
    return h ? h->mode.max_ready_ticks() : 0;
}

extern "C" uint64_t nexus_rtc_action_chunk_total_ready_ticks(
        nexus_rtc_action_chunk* h) {
    return h ? h->mode.total_ready_ticks() : 0;
}

extern "C" int nexus_rtc_action_chunk_last_error(nexus_rtc_action_chunk* h) {
    return h ? h->mode.last_error() : CAP_ERR_ARG;
}
