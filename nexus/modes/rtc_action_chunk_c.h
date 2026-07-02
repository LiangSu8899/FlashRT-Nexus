/* nexus/modes/rtc_action_chunk_c.h — C ABI for Nexus RTC action chunks. */
#ifndef NEXUS_MODES_RTC_ACTION_CHUNK_C_H
#define NEXUS_MODES_RTC_ACTION_CHUNK_C_H

#include "nexus/schedulers/stage_dag_c.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NEXUS_RTC_NO_OUTPUT_PORT 0xffffffffu

enum nexus_rtc_chunk_state {
    NEXUS_RTC_IDLE = 0,
    NEXUS_RTC_PENDING = 1,
    NEXUS_RTC_READY = 2,
    NEXUS_RTC_FALLBACK = 3,
    NEXUS_RTC_ERROR = 4
};

typedef struct nexus_rtc_action_chunk_config {
    uint32_t struct_size;       /* = sizeof(nexus_rtc_action_chunk_config) */
    uint32_t reserved;
    uint64_t action_stage;
    uint32_t output_port;       /* NEXUS_RTC_NO_OUTPUT_PORT disables copy   */
    uint32_t chunk_length;      /* actions per completed chunk              */
    uint32_t action_bytes;      /* bytes per action emitted by next_action  */
    uint32_t ring_slots;        /* allocated at create                      */
    uint32_t execute_horizon;   /* prefetch when remaining <= this          */
    int32_t  deadline_ticks;    /* <0 disables fallback mark                */
    uint32_t reserved1;
} nexus_rtc_action_chunk_config;

typedef struct nexus_rtc_action_chunk_s nexus_rtc_action_chunk;

int  nexus_rtc_action_chunk_create(nexus_stage_dag*,
                                   const nexus_rtc_action_chunk_config*,
                                   nexus_rtc_action_chunk** out);
/* Infer chunk_length/action_bytes from output_port.shape:
 * shape[0] = chunk length, shape[1:] = one action, scalar_bytes = one
 * postprocessed scalar emitted by cap_model_get_output. */
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

#endif  /* NEXUS_MODES_RTC_ACTION_CHUNK_C_H */
