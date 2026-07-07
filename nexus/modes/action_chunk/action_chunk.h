/* nexus/modes/action_chunk/action_chunk.h — generic action-chunk scheduler mode.
 *
 * This mode lives in Nexus, not in FlashRT runtime. It is model-agnostic:
 * any VLA policy exposing an action-chunk output port drives it with the
 * same verbs; Pi0.5 is the first gated instance. It is an outer-loop state
 * machine over a declared action stage: fire asynchronously, poll completion,
 * and let the robot loop keep executing the previous action chunk until a new
 * one is ready or fallback is required.
 */
#ifndef NEXUS_MODES_ACTION_CHUNK_H
#define NEXUS_MODES_ACTION_CHUNK_H

#include "nexus/schedulers/stage_dag.h"

#include <cstdint>
#include <vector>

namespace nexus {

enum class ActionChunkState {
    kIdle,
    kPending,
    kReady,
    kFallback,
    kError,
};

struct ActionChunkConfig {
    uint64_t action_stage = 0;
    uint32_t output_port = UINT32_MAX;  /* UINT32_MAX: no output copy */
    uint32_t chunk_length = 0;          /* actions per chunk          */
    uint32_t action_bytes = 0;          /* bytes per single action    */
    uint32_t ring_slots = 2;            /* allocated at construction  */
    uint32_t execute_horizon = 1;       /* prefetch when remaining <= */
    int deadline_ticks = -1;            /* <0 disables fallback mark  */
};

class ActionChunkMode {
public:
    /* Build config from an ACTION output port shape:
     *   shape[0] = chunk length, shape[1:] = one action.
     * `scalar_bytes` is the byte size emitted by get_output for one scalar
     * (usually 4 for postprocessed float32 robot actions). */
    static int config_from_output_port(StageDagRunner* runner,
                                       uint64_t action_stage,
                                       uint32_t output_port,
                                       uint32_t scalar_bytes,
                                       uint32_t ring_slots,
                                       uint32_t execute_horizon,
                                       int deadline_ticks,
                                       ActionChunkConfig* out);

    ActionChunkMode(StageDagRunner* runner,
                       const ActionChunkConfig& config);
    ActionChunkMode(StageDagRunner* runner, uint64_t action_stage,
                       int max_pending_polls);

    int request();
    ActionChunkState poll();
    ActionChunkState next_action(void* out, uint64_t capacity,
                              uint64_t* written);
    void reset();

    bool in_flight() const { return in_flight_; }
    bool has_active_chunk() const { return active_slot_ >= 0; }
    uint32_t active_index() const { return active_index_; }
    uint32_t remaining_actions() const;
    int last_error() const { return last_error_; }
    int fallbacks() const { return fallbacks_; }
    int late_chunks() const { return late_chunks_; }
    uint32_t pending_ticks() const { return pending_ticks_; }
    uint32_t last_ready_ticks() const { return last_ready_ticks_; }
    uint32_t max_ready_ticks() const { return max_ready_ticks_; }
    uint64_t total_ready_ticks() const { return total_ready_ticks_; }
    uint64_t completed_chunks() const { return completed_chunks_; }
    uint64_t emitted_actions() const { return emitted_actions_; }

private:
    bool chunking_enabled() const;
    unsigned char* slot_ptr(int slot);
    int copy_output_to_pending_slot();

    StageDagRunner* runner_ = nullptr;
    ActionChunkConfig config_{};
    uint64_t chunk_bytes_ = 0;
    std::vector<unsigned char> storage_;
    int active_slot_ = -1;
    int pending_slot_ = -1;
    uint32_t active_index_ = 0;
    int pending_ticks_ = 0;
    bool deadline_reported_ = false;
    bool in_flight_ = false;
    int last_error_ = CAP_OK;
    int fallbacks_ = 0;
    int late_chunks_ = 0;
    uint32_t last_ready_ticks_ = 0;
    uint32_t max_ready_ticks_ = 0;
    uint64_t total_ready_ticks_ = 0;
    uint64_t completed_chunks_ = 0;
    uint64_t emitted_actions_ = 0;
};

}  // namespace nexus

#endif  /* NEXUS_MODES_ACTION_CHUNK_H */
