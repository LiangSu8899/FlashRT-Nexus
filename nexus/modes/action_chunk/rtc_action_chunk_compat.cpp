/* DEPRECATED forwarding wrappers for the pre-rename C ABI. See the header. */
#include "nexus/modes/action_chunk/rtc_action_chunk_compat.h"

extern "C" int nexus_rtc_action_chunk_create(
        nexus_stage_dag* runner,
        const nexus_rtc_action_chunk_config* config,
        nexus_rtc_action_chunk** out) {
    return nexus_action_chunk_create(runner, config, out);
}

extern "C" int nexus_rtc_action_chunk_create_for_output_port(
        nexus_stage_dag* runner, uint64_t action_stage, uint32_t output_port,
        uint32_t scalar_bytes, uint32_t ring_slots, uint32_t execute_horizon,
        int32_t deadline_ticks, nexus_rtc_action_chunk** out) {
    return nexus_action_chunk_create_for_output_port(
        runner, action_stage, output_port, scalar_bytes, ring_slots,
        execute_horizon, deadline_ticks, out);
}

extern "C" void nexus_rtc_action_chunk_destroy(nexus_rtc_action_chunk* h) {
    nexus_action_chunk_destroy(h);
}

extern "C" int nexus_rtc_action_chunk_request(nexus_rtc_action_chunk* h) {
    return nexus_action_chunk_request(h);
}

extern "C" int nexus_rtc_action_chunk_poll(nexus_rtc_action_chunk* h) {
    return nexus_action_chunk_poll(h);
}

extern "C" int nexus_rtc_action_chunk_next_action(nexus_rtc_action_chunk* h,
                                                  void* out, uint64_t capacity,
                                                  uint64_t* written) {
    return nexus_action_chunk_next_action(h, out, capacity, written);
}

extern "C" void nexus_rtc_action_chunk_reset(nexus_rtc_action_chunk* h) {
    nexus_action_chunk_reset(h);
}

extern "C" int nexus_rtc_action_chunk_in_flight(nexus_rtc_action_chunk* h) {
    return nexus_action_chunk_in_flight(h);
}

extern "C" int nexus_rtc_action_chunk_has_active(nexus_rtc_action_chunk* h) {
    return nexus_action_chunk_has_active(h);
}

extern "C" uint32_t nexus_rtc_action_chunk_remaining(
        nexus_rtc_action_chunk* h) {
    return nexus_action_chunk_remaining(h);
}

extern "C" uint32_t nexus_rtc_action_chunk_active_index(
        nexus_rtc_action_chunk* h) {
    return nexus_action_chunk_active_index(h);
}

extern "C" uint64_t nexus_rtc_action_chunk_completed(
        nexus_rtc_action_chunk* h) {
    return nexus_action_chunk_completed(h);
}

extern "C" uint64_t nexus_rtc_action_chunk_emitted(
        nexus_rtc_action_chunk* h) {
    return nexus_action_chunk_emitted(h);
}

extern "C" int nexus_rtc_action_chunk_fallbacks(nexus_rtc_action_chunk* h) {
    return nexus_action_chunk_fallbacks(h);
}

extern "C" int nexus_rtc_action_chunk_late_chunks(nexus_rtc_action_chunk* h) {
    return nexus_action_chunk_late_chunks(h);
}

extern "C" uint32_t nexus_rtc_action_chunk_pending_ticks(
        nexus_rtc_action_chunk* h) {
    return nexus_action_chunk_pending_ticks(h);
}

extern "C" uint32_t nexus_rtc_action_chunk_last_ready_ticks(
        nexus_rtc_action_chunk* h) {
    return nexus_action_chunk_last_ready_ticks(h);
}

extern "C" uint32_t nexus_rtc_action_chunk_max_ready_ticks(
        nexus_rtc_action_chunk* h) {
    return nexus_action_chunk_max_ready_ticks(h);
}

extern "C" uint64_t nexus_rtc_action_chunk_total_ready_ticks(
        nexus_rtc_action_chunk* h) {
    return nexus_action_chunk_total_ready_ticks(h);
}

extern "C" int nexus_rtc_action_chunk_last_error(nexus_rtc_action_chunk* h) {
    return nexus_action_chunk_last_error(h);
}
