/* nexus/modes/action_chunk/rtc_action_chunk_compat.h — DEPRECATED aliases.
 *
 * Compatibility layer for the pre-rename C ABI (`nexus_rtc_action_chunk_*`).
 * The mode is now `nexus/modes/action_chunk` with the `nexus_action_chunk_*`
 * ABI; new code must include `action_chunk_c.h` instead. These aliases are
 * thin forwarding wrappers over the renamed entry points, kept for external
 * consumers only, and will be removed at the next declared 0.x breaking
 * window.
 */
#ifndef NEXUS_MODES_RTC_ACTION_CHUNK_COMPAT_H
#define NEXUS_MODES_RTC_ACTION_CHUNK_COMPAT_H

#include "nexus/modes/action_chunk/action_chunk_c.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NEXUS_RTC_NO_OUTPUT_PORT NEXUS_AC_NO_OUTPUT_PORT

#define NEXUS_RTC_IDLE NEXUS_AC_IDLE
#define NEXUS_RTC_PENDING NEXUS_AC_PENDING
#define NEXUS_RTC_READY NEXUS_AC_READY
#define NEXUS_RTC_FALLBACK NEXUS_AC_FALLBACK
#define NEXUS_RTC_ERROR NEXUS_AC_ERROR

/* Identical layout and semantics; only the type name is legacy. */
typedef nexus_action_chunk_config nexus_rtc_action_chunk_config;
typedef nexus_action_chunk nexus_rtc_action_chunk;

int  nexus_rtc_action_chunk_create(nexus_stage_dag*,
                                   const nexus_rtc_action_chunk_config*,
                                   nexus_rtc_action_chunk** out);
int  nexus_rtc_action_chunk_create_for_output_port(
                                   nexus_stage_dag*,
                                   uint64_t action_stage,
                                   uint32_t output_port,
                                   uint32_t scalar_bytes,
                                   uint32_t ring_slots,
                                   uint32_t execute_horizon,
                                   int32_t deadline_ticks,
                                   nexus_rtc_action_chunk** out);
void nexus_rtc_action_chunk_destroy(nexus_rtc_action_chunk*);

int  nexus_rtc_action_chunk_request(nexus_rtc_action_chunk*);
int  nexus_rtc_action_chunk_poll(nexus_rtc_action_chunk*);
int  nexus_rtc_action_chunk_next_action(nexus_rtc_action_chunk*,
                                        void* out, uint64_t capacity,
                                        uint64_t* written);
void nexus_rtc_action_chunk_reset(nexus_rtc_action_chunk*);

int      nexus_rtc_action_chunk_in_flight(nexus_rtc_action_chunk*);
int      nexus_rtc_action_chunk_has_active(nexus_rtc_action_chunk*);
uint32_t nexus_rtc_action_chunk_remaining(nexus_rtc_action_chunk*);
uint32_t nexus_rtc_action_chunk_active_index(nexus_rtc_action_chunk*);
uint64_t nexus_rtc_action_chunk_completed(nexus_rtc_action_chunk*);
uint64_t nexus_rtc_action_chunk_emitted(nexus_rtc_action_chunk*);
int      nexus_rtc_action_chunk_fallbacks(nexus_rtc_action_chunk*);
int      nexus_rtc_action_chunk_late_chunks(nexus_rtc_action_chunk*);
uint32_t nexus_rtc_action_chunk_pending_ticks(nexus_rtc_action_chunk*);
uint32_t nexus_rtc_action_chunk_last_ready_ticks(nexus_rtc_action_chunk*);
uint32_t nexus_rtc_action_chunk_max_ready_ticks(nexus_rtc_action_chunk*);
uint64_t nexus_rtc_action_chunk_total_ready_ticks(nexus_rtc_action_chunk*);
int      nexus_rtc_action_chunk_last_error(nexus_rtc_action_chunk*);

#ifdef __cplusplus
}
#endif

#endif  /* NEXUS_MODES_RTC_ACTION_CHUNK_COMPAT_H */
