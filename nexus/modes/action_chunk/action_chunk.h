/* nexus/modes/action_chunk/action_chunk.h — generic action-chunk scheduler mode.
 *
 * This mode lives in Nexus, not in FlashRT runtime. It is model-agnostic:
 * any VLA policy exposing an action-chunk output port drives it with the
 * same verbs; Pi0.5 is the first gated instance. It is an outer-loop state
 * machine over a declared action stage: fire asynchronously, poll completion,
 * and let the robot loop keep executing the previous action chunk until a new
 * one is ready or fallback is required.
 *
 * Two logical clocks, never converted into each other:
 *  - action_step — the controller-step grid. Advances only on next_action()
 *    emission or advance_step(). Chunk stamps (fire/ready) and the
 *    deadline_steps latency deadline live on this grid.
 *  - pending polls — the query-effort counter behind poll_budget, a liveness
 *    watchdog for embedders with no grid (poll-only mode) and for the wait
 *    before the first chunk. poll() never advances the grid.
 * Overrun of either clock reports kFallback once (from poll); the in-flight
 * fire is never cancelled and a late chunk is still accepted (and counted).
 *
 * The request verb is two-phase: begin_request() runs the prepare step (state
 * snapshot; later: policy port writes) and is idempotent; commit_request()
 * fires. request() is both, and commit_request() without begin_request()
 * begins implicitly — hosts that must inject staged inputs between the two
 * call them separately.
 *
 * Determinism contract: no wall clock, no RNG; identical verb/input sequences
 * produce identical outputs, states, and counters.
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

/* Policy slots. Only the defaults are implemented so far; every other value
 * is rejected by validate(). The slots are orthogonal: prepare acts before a
 * fire (model-input side), consume acts on a ready chunk (output side). */
enum : uint8_t {
    kActionChunkPrepareNone = 0,
    kActionChunkConsumePlain = 0,
    kActionChunkSwitchLatency = 0,
    kActionChunkSwitchState = 1,
    kActionChunkMissReportOnly = 0,
    kActionChunkMissHoldLast = 1,
    kActionChunkDtypeRaw = 0,
    kActionChunkDtypeF32 = 1,
    kActionChunkDtypeBf16 = 2,
    kActionChunkDtypeF16 = 3,
    kActionChunkReprAbsolute = 0,
    kActionChunkReprDeltaCumulative = 1,
    kActionChunkReprDeltaFromStart = 2,
};

struct ActionChunkConfig {
    uint64_t action_stage = 0;
    uint32_t output_port = UINT32_MAX;  /* UINT32_MAX: no output copy */
    uint32_t chunk_length = 0;          /* actions per chunk          */
    uint32_t action_bytes = 0;          /* bytes per single action    */
    uint32_t ring_slots = 2;            /* allocated at construction  */
    uint32_t execute_horizon = 1;       /* prefetch when remaining <= */
    int poll_budget = -1;               /* pending-poll watchdog; <0 off */
    /* -- v2 ---------------------------------------------------------- */
    int32_t deadline_steps = -1;        /* grid latency deadline; <=0 off */
    uint8_t prepare_policy = kActionChunkPrepareNone;
    uint8_t consume_policy = kActionChunkConsumePlain;
    uint8_t switch_mode = kActionChunkSwitchLatency;
    uint8_t miss_policy = kActionChunkMissReportOnly;
    uint8_t scalar_dtype = kActionChunkDtypeRaw;
    uint8_t action_representation = kActionChunkReprAbsolute;
    uint8_t distance_metric = 0;        /* 0 = l1, 1 = l2 */
    uint32_t state_dim = 0;             /* 0 = no state feed */
    uint32_t candidates = 0;            /* 0 or 1 = single fire (reserved) */
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
                                       int poll_budget,
                                       ActionChunkConfig* out);

    /* Reject configs the mode cannot honor (unknown enum values, policies
     * not implemented yet, candidates > 1). Fail at setup, never at tick. */
    static int validate(const ActionChunkConfig& config);

    ActionChunkMode(StageDagRunner* runner,
                    const ActionChunkConfig& config);
    ActionChunkMode(StageDagRunner* runner, uint64_t action_stage,
                    int max_pending_polls);

    /* Two-phase request; request() = begin + commit. */
    int begin_request();
    int commit_request();
    int request();

    ActionChunkState poll();
    ActionChunkState next_action(void* out, uint64_t capacity,
                                 uint64_t* written);
    /* Advance the controller-step grid without consuming — for control
     * ticks where the embedder actuates something of its own. */
    int advance_step();
    /* Explicit blocking verb: sync the action stage, then consume. */
    ActionChunkState sync_next_chunk();
    void reset();

    /* State feed for policies that need proprioception (validated finite;
     * snapshotted at begin_request). set_state_action_indices maps state
     * dims into wider action vectors and must be called before the first
     * request. */
    int set_state(const float* state, uint32_t dim);
    int set_state_action_indices(const uint32_t* indices, uint32_t n);

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
    /* Emitted actions include held re-emissions; held_actions counts them. */
    uint64_t emitted_actions() const { return emitted_actions_; }
    uint64_t action_step() const { return action_step_; }
    uint64_t held_actions() const { return held_actions_; }
    uint64_t prepared_requests() const { return prepared_requests_; }
    uint64_t state_updates() const { return state_updates_; }
    uint32_t last_d_steps() const { return last_d_steps_; }

private:
    bool chunking_enabled() const;
    unsigned char* slot_ptr(int slot);
    int copy_output_to_pending_slot();

    StageDagRunner* runner_ = nullptr;
    ActionChunkConfig config_{};
    uint64_t chunk_bytes_ = 0;
    std::vector<unsigned char> storage_;
    std::vector<uint64_t> slot_fire_step_;
    std::vector<uint64_t> slot_ready_step_;
    std::vector<uint64_t> slot_start_step_;
    std::vector<unsigned char> held_;      /* sized only for HOLD_LAST */
    std::vector<float> state_latest_;
    std::vector<float> state_fire_;
    std::vector<uint32_t> state_action_indices_;
    int active_slot_ = -1;
    int pending_slot_ = -1;
    uint32_t active_index_ = 0;
    int pending_ticks_ = 0;
    bool deadline_reported_ = false;
    bool in_flight_ = false;
    bool begun_ = false;
    bool requested_once_ = false;
    bool has_state_ = false;
    bool has_held_ = false;
    uint64_t action_step_ = 0;
    uint64_t fire_step_ = 0;
    int last_error_ = CAP_OK;
    int fallbacks_ = 0;
    int late_chunks_ = 0;
    uint32_t last_ready_ticks_ = 0;
    uint32_t max_ready_ticks_ = 0;
    uint32_t last_d_steps_ = 0;
    uint64_t total_ready_ticks_ = 0;
    uint64_t completed_chunks_ = 0;
    uint64_t emitted_actions_ = 0;
    uint64_t held_actions_ = 0;
    uint64_t prepared_requests_ = 0;
    uint64_t state_updates_ = 0;
};

}  // namespace nexus

#endif  /* NEXUS_MODES_ACTION_CHUNK_H */
