#include "nexus/modes/action_chunk/action_chunk.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace nexus {

namespace {

constexpr uint32_t kStateModality = 3;
constexpr uint32_t kF32Dtype = 1;
constexpr uint32_t kInputDirection = 0;
constexpr uint32_t kStagedUpdate = 1;

}  // namespace

int ActionChunkMode::config_from_output_port(
        StageDagRunner* runner, uint64_t action_stage, uint32_t output_port,
        uint32_t scalar_bytes, uint32_t ring_slots, uint32_t execute_horizon,
        int poll_budget, ActionChunkConfig* out) {
    if (!runner || !out || !scalar_bytes) return CAP_ERR_ARG;
    cap_model_runtime* model = runner->model();
    if (!model || output_port >= model->n_ports) return CAP_ERR_ARG;
    const cap_model_port& port = model->ports[output_port];
    if (!port.shape || port.rank == 0 || port.shape[0] <= 0)
        return CAP_ERR_ARG;

    uint64_t per_action = scalar_bytes;
    for (uint32_t i = 1; i < port.rank; ++i) {
        if (port.shape[i] <= 0) return CAP_ERR_ARG;
        const uint64_t dim = static_cast<uint64_t>(port.shape[i]);
        if (per_action > std::numeric_limits<uint64_t>::max() / dim)
            return CAP_ERR_ARG;
        per_action *= dim;
    }
    if (per_action > std::numeric_limits<uint32_t>::max())
        return CAP_ERR_ARG;

    ActionChunkConfig cfg{};
    cfg.action_stage = action_stage;
    cfg.output_port = output_port;
    cfg.chunk_length = static_cast<uint32_t>(port.shape[0]);
    cfg.action_bytes = static_cast<uint32_t>(per_action);
    cfg.ring_slots = ring_slots;
    cfg.execute_horizon = execute_horizon;
    cfg.poll_budget = poll_budget;
    *out = cfg;
    return CAP_OK;
}

int ActionChunkMode::validate(const ActionChunkConfig& config) {
    if (config.prepare_policy > kActionChunkPreparePrevChunkPrefix)
        return CAP_ERR_ARG;
    if (config.consume_policy > kActionChunkConsumeTemporalFusion)
        return CAP_ERR_ARG;
    if (config.switch_mode > kActionChunkSwitchState) return CAP_ERR_ARG;
    if (config.miss_policy > kActionChunkMissHoldLast) return CAP_ERR_ARG;
    if (config.scalar_dtype > kActionChunkDtypeF16) return CAP_ERR_ARG;
    if (config.action_representation > kActionChunkReprDeltaFromStart)
        return CAP_ERR_ARG;
    if (config.distance_metric > 1) return CAP_ERR_ARG;
    if (config.candidates > 1) return CAP_ERR_ARG;

    const bool chunked = config.output_port != UINT32_MAX &&
                         config.chunk_length && config.action_bytes;
    const bool non_plain = config.consume_policy != kActionChunkConsumePlain;
    const bool state_switch = non_plain &&
                              config.switch_mode == kActionChunkSwitchState;
    const bool fusion =
        config.consume_policy == kActionChunkConsumeTemporalFusion;
    if (non_plain && !chunked) return CAP_ERR_ARG;
    if (state_switch && config.state_dim == 0) return CAP_ERR_ARG;
    /* Policies that do arithmetic on action values need a typed view; only
     * f32 arithmetic is implemented (and oracle-gated) so far. */
    if ((state_switch || fusion) &&
        (config.scalar_dtype != kActionChunkDtypeF32 ||
         config.action_bytes % 4))
        return CAP_ERR_ARG;
    if (fusion) {
        const uint32_t max_chunks =
            config.fusion_max_chunks ? config.fusion_max_chunks : 3;
        if (config.ring_slots < max_chunks + 1) return CAP_ERR_ARG;
        if (!(config.fusion_decay >= 0.0) ||
            config.fusion_decay > 1e6)
            return CAP_ERR_ARG;
    }
    if (config.prepare_policy == kActionChunkPrepareProjectedState) {
        /* Compatibility matrix: the internal d-vs-k seating IS the latency
         * compensation; pairing with switch would compensate twice. The
         * fusion pairing has no reference implementation and is gated as
         * experimental: projection removes the bulk of the seam, fusion
         * smooths the residual; fusion of an early-landed chunk is deferred
         * to its start step so the waiting window keeps its fused chunk. */
        const bool fused_pair = config.experimental &&
            config.consume_policy == kActionChunkConsumeTemporalFusion &&
            config.switch_mode == kActionChunkSwitchLatency;
        if (config.consume_policy != kActionChunkConsumePlain && !fused_pair)
            return CAP_ERR_ARG;
        if (!chunked || config.state_dim == 0) return CAP_ERR_ARG;
        if (config.scalar_dtype != kActionChunkDtypeF32 ||
            config.action_bytes % 4)
            return CAP_ERR_ARG;
        if (config.action_representation != kActionChunkReprDeltaCumulative)
            return CAP_ERR_ARG;
    } else if (config.state_input_port != 0) {
        return CAP_ERR_ARG;
    }
    if (config.prepare_policy == kActionChunkPreparePrevChunkPrefix) {
        /* The in-graph correction owns the seam; consumption pairs with the
         * latency switch (seat at d). State-mode pairing is unvalidated. */
        if (config.consume_policy != kActionChunkConsumeSwitch ||
            config.switch_mode != kActionChunkSwitchLatency)
            return CAP_ERR_ARG;
        if (!chunked) return CAP_ERR_ARG;
        if (config.prefix_len == 0 ||
            config.prefix_len > config.chunk_length)
            return CAP_ERR_ARG;
        if (config.raw_out_port == 0 || config.raw_action_bytes == 0)
            return CAP_ERR_ARG;
        /* +1 encoded; only host transport (0) is implemented. */
        if (config.prev_chunk_port != 0) return CAP_ERR_ARG;
    }
    return CAP_OK;
}

int ActionChunkMode::validate_model_ports(
        StageDagRunner* runner, const ActionChunkConfig& config) {
    if (!config.state_input_port) return CAP_OK;
    if (!runner || !runner->ok()) return CAP_ERR_ARG;
    cap_model_runtime* model = runner->model();
    const uint32_t index = config.state_input_port - 1;
    if (!model || !model->set_input || index >= model->n_ports)
        return CAP_ERR_ARG;
    const cap_model_port& port = model->ports[index];
    if (port.modality != kStateModality || port.dtype != kF32Dtype ||
        port.direction != kInputDirection || port.update != kStagedUpdate ||
        !port.shape || port.rank != 1 || port.shape[0] <= 0 ||
        static_cast<uint64_t>(port.shape[0]) != config.state_dim) {
        return CAP_ERR_ARG;
    }
    return CAP_OK;
}

ActionChunkMode::ActionChunkMode(
        StageDagRunner* runner, const ActionChunkConfig& config)
    : runner_(runner), config_(config) {
    const int port_validation = validate_model_ports(runner_, config_);
    if (port_validation != CAP_OK) {
        last_error_ = port_validation;
        runner_ = nullptr;
        return;
    }
    if (config_.ring_slots == 0) config_.ring_slots = 1;
    if (config_.output_port != UINT32_MAX &&
        config_.chunk_length && config_.action_bytes) {
        chunk_bytes_ = static_cast<uint64_t>(config_.chunk_length) *
                       static_cast<uint64_t>(config_.action_bytes);
        storage_.resize(chunk_bytes_ * config_.ring_slots);
        slot_fire_step_.resize(config_.ring_slots, 0);
        slot_ready_step_.resize(config_.ring_slots, 0);
        slot_start_step_.resize(config_.ring_slots, 0);
        if (config_.miss_policy == kActionChunkMissHoldLast)
            held_.resize(config_.action_bytes);
        slot_valid_.resize(config_.ring_slots, 0);
        if (config_.consume_policy == kActionChunkConsumeTemporalFusion) {
            if (config_.fusion_max_chunks == 0) config_.fusion_max_chunks = 3;
            weight_table_.resize(config_.chunk_length);
            for (uint32_t j = 0; j < config_.chunk_length; ++j)
                weight_table_[j] =
                    std::exp(-config_.fusion_decay * static_cast<double>(j));
            fused_.resize(chunk_bytes_);
            fusion_acc_.resize(config_.action_bytes / 4, 0.0);
            retained_.reserve(config_.ring_slots);
        }
    }
    if (config_.state_dim) {
        state_latest_.resize(config_.state_dim, 0.0f);
        state_fire_.resize(config_.state_dim, 0.0f);
        state_cum_.resize(config_.state_dim, 0.0);
        if (config_.prepare_policy == kActionChunkPrepareProjectedState)
            projected_.resize(config_.state_dim, 0.0f);
    }
    if (config_.prepare_policy == kActionChunkPreparePrevChunkPrefix &&
        config_.chunk_length && config_.raw_action_bytes) {
        const uint64_t raw_bytes =
            static_cast<uint64_t>(config_.chunk_length) *
            static_cast<uint64_t>(config_.raw_action_bytes);
        prev_raw_.resize(raw_bytes, 0);
        prev_staged_.resize(raw_bytes, 0);
    }
}

ActionChunkMode::ActionChunkMode(StageDagRunner* runner,
                                 uint64_t action_stage,
                                 int max_pending_polls)
    : ActionChunkMode(
          runner,
          ActionChunkConfig{action_stage, UINT32_MAX, 0, 0, 1, 1,
                            max_pending_polls}) {}

int ActionChunkMode::begin_request() {
    if (!runner_ || !runner_->ok()) return CAP_ERR_ARG;
    if (in_flight_ || waiting_slot_ >= 0) return CAP_ERR_ARG;
    if (begun_) return CAP_OK;
    if (config_.state_dim && has_state_)
        std::copy(state_latest_.begin(), state_latest_.end(),
                  state_fire_.begin());
    if (config_.prepare_policy == kActionChunkPrepareProjectedState) {
        int rc = prepare_projected();
        if (rc != CAP_OK) return rc;
        if (config_.state_input_port) {
            cap_model_runtime* model = runner_->model();
            rc = cap_model_set_input(
                model, config_.state_input_port - 1, projected_.data(),
                static_cast<uint64_t>(projected_.size()) * sizeof(float), -1);
            if (rc != CAP_OK) {
                last_error_ = rc;
                return rc;
            }
        }
    }
    if (config_.prepare_policy == kActionChunkPreparePrevChunkPrefix) {
        int rc = prepare_prev_chunk();
        if (rc != CAP_OK) return rc;
    }
    ++prepared_requests_;
    begun_ = true;
    return CAP_OK;
}

int ActionChunkMode::commit_request() {
    if (!begun_) {
        int rc = begin_request();
        if (rc != CAP_OK) return rc;
    }
    begun_ = false;
    if (chunking_enabled()) {
        if (config_.consume_policy == kActionChunkConsumeTemporalFusion) {
            /* Retained raw chunks are immutable; fire into a free slot, or
             * evict the oldest retained chunk. Chunks are equal-length with
             * monotonic starts, so anything older than the newest
             * fusion_max_chunks chunks can never enter a fusion window --
             * eviction is exactly equivalent to unbounded retention. */
            int slot = -1;
            for (uint32_t s = 0; s < config_.ring_slots; ++s)
                if (!slot_valid_[s]) { slot = static_cast<int>(s); break; }
            if (slot < 0) {
                slot = retained_.front();
                retained_.erase(retained_.begin());
                slot_valid_[slot] = 0;
                ++pruned_chunks_;
            }
            pending_slot_ = slot;
        } else {
            int next = active_slot_ < 0 ? 0 : (active_slot_ + 1) %
                                               (int)config_.ring_slots;
            pending_slot_ = next;
        }
    }
    int rc = runner_->fire(config_.action_stage);
    if (rc != CAP_OK) {
        last_error_ = rc;
        pending_slot_ = -1;
        return rc;
    }
    in_flight_ = true;
    requested_once_ = true;
    fire_step_ = action_step_;
    pending_lookahead_ =
        config_.prepare_policy == kActionChunkPrepareProjectedState
            ? projected_count_ : 0;
    if (pending_slot_ >= 0) slot_fire_step_[pending_slot_] = action_step_;
    pending_ticks_ = 0;
    deadline_reported_ = false;
    last_error_ = CAP_OK;
    return CAP_OK;
}

int ActionChunkMode::request() {
    return commit_request();
}

ActionChunkState ActionChunkMode::poll() {
    if (!runner_ || !runner_->ok()) return ActionChunkState::kError;
    if (!in_flight_) return ActionChunkState::kIdle;
    int q = runner_->query(config_.action_stage);
    if (q == 0) {
        last_ready_ticks_ = static_cast<uint32_t>(pending_ticks_);
        total_ready_ticks_ += last_ready_ticks_;
        if (last_ready_ticks_ > max_ready_ticks_)
            max_ready_ticks_ = last_ready_ticks_;
        last_d_steps_ = static_cast<uint32_t>(action_step_ - fire_step_);
        if (deadline_reported_) ++late_chunks_;
        if (chunking_enabled()) {
            if (config_.prepare_policy == kActionChunkPreparePrevChunkPrefix) {
                /* Retain the raw-space chunk for the next fire's prefix.
                 * Read it before the postprocessed copy: both views window
                 * the same producer buffer. SWAP ports are served from
                 * their device window, STAGED ports through the verb. */
                uint64_t raw_written = 0;
                int rraw = runner_->read_output(
                    config_.raw_out_port - 1, prev_raw_.data(),
                    prev_raw_.size(), &raw_written);
                if (rraw != CAP_OK || raw_written != prev_raw_.size()) {
                    last_error_ = rraw != CAP_OK ? rraw : CAP_ERR_ARG;
                    in_flight_ = false;
                    pending_slot_ = -1;
                    return ActionChunkState::kError;
                }
                has_raw_prev_ = true;
            }
            int rc = copy_output_to_pending_slot();
            if (rc != CAP_OK) {
                last_error_ = rc;
                in_flight_ = false;
                pending_slot_ = -1;
                return ActionChunkState::kError;
            }
            slot_ready_step_[pending_slot_] = action_step_;
            /* Chunks anchor at their fire step; projected_state anchors at
             * fire_step + k (index 0 is pinned to the projected step). */
            slot_start_step_[pending_slot_] =
                slot_fire_step_[pending_slot_] + pending_lookahead_;
            if (active_slot_ >= 0) ++chunk_switches_;
            int seat_rc = CAP_OK;
            if (config_.prepare_policy == kActionChunkPrepareProjectedState) {
                if (config_.consume_policy ==
                    kActionChunkConsumeTemporalFusion) {
                    /* Composed seating: at or past the start step, fuse and
                     * seat now (latency index = d - k, clipped); earlier,
                     * park as waiting WITHOUT fusing -- the previous fused
                     * chunk keeps serving until the start step. */
                    if (action_step_ >= slot_start_step_[pending_slot_]) {
                        seat_rc = seat_fusion();
                    } else {
                        waiting_slot_ = pending_slot_;
                        pending_slot_ = -1;
                    }
                } else {
                    seat_rc = seat_projected();
                }
            } else switch (config_.consume_policy) {
                case kActionChunkConsumeSwitch:
                    seat_rc = seat_switch();
                    break;
                case kActionChunkConsumeTemporalFusion:
                    seat_rc = seat_fusion();
                    break;
                default:  /* plain: seat immediately, consume from index 0 */
                    active_slot_ = pending_slot_;
                    pending_slot_ = -1;
                    active_index_ = 0;
                    break;
            }
            if (seat_rc != CAP_OK) {
                last_error_ = seat_rc;
                in_flight_ = false;
                pending_slot_ = -1;
                return ActionChunkState::kError;
            }
        }
        in_flight_ = false;
        pending_ticks_ = 0;
        deadline_reported_ = false;
        ++completed_chunks_;
        return ActionChunkState::kReady;
    }
    if (q < 0) {
        last_error_ = q;
        in_flight_ = false;
        return ActionChunkState::kError;
    }
    ++pending_ticks_;
    const bool over_polls = config_.poll_budget >= 0 &&
                            pending_ticks_ > config_.poll_budget;
    const bool over_steps =
        config_.deadline_steps > 0 &&
        action_step_ - fire_step_ >
            static_cast<uint64_t>(config_.deadline_steps);
    if ((over_polls || over_steps) && !deadline_reported_) {
        deadline_reported_ = true;
        ++fallbacks_;
        return ActionChunkState::kFallback;
    }
    return ActionChunkState::kPending;
}

ActionChunkState ActionChunkMode::next_action(void* out, uint64_t capacity,
                                              uint64_t* written) {
    if (written) *written = config_.action_bytes;
    if (!chunking_enabled() || !out) return ActionChunkState::kError;
    promote_waiting();
    if (active_slot_ < 0) {
        if (waiting_slot_ >= 0) {
            /* A seated chunk waits for its start step and nothing else is
             * consumable: each gap step is served by the miss policy. The
             * gap is bounded by k - d. Never switch early: index 0 is
             * pinned to a future controller step by construction. */
            if (config_.miss_policy == kActionChunkMissHoldLast &&
                has_held_) {
                if (capacity < config_.action_bytes) {
                    last_error_ = CAP_ERR_ARG;
                    return ActionChunkState::kError;
                }
                std::memcpy(out, held_.data(), config_.action_bytes);
                ++action_step_;
                ++emitted_actions_;
                ++held_actions_;
                promote_waiting();
                return ActionChunkState::kFallback;
            }
            return ActionChunkState::kFallback;
        }
        if (!in_flight_) {
            int rc = request();
            if (rc != CAP_OK) {
                last_error_ = rc;
                return ActionChunkState::kError;
            }
        }
        ActionChunkState s = poll();
        if (s == ActionChunkState::kReady) return s;
        if (config_.miss_policy == kActionChunkMissHoldLast && has_held_) {
            if (capacity < config_.action_bytes) {
                last_error_ = CAP_ERR_ARG;
                return ActionChunkState::kError;
            }
            std::memcpy(out, held_.data(), config_.action_bytes);
            ++action_step_;
            ++emitted_actions_;
            ++held_actions_;
            return ActionChunkState::kFallback;
        }
        return s;
    }
    if (capacity < config_.action_bytes) {
        last_error_ = CAP_ERR_ARG;
        return ActionChunkState::kError;
    }
    const unsigned char* base =
        consume_fused_ ? fused_.data() : slot_ptr(active_slot_);
    const unsigned char* src = base +
        static_cast<uint64_t>(active_index_) * config_.action_bytes;
    std::memcpy(out, src, config_.action_bytes);
    if (!held_.empty()) {
        std::memcpy(held_.data(), src, config_.action_bytes);
        has_held_ = true;
    }
    ++active_index_;
    ++emitted_actions_;
    ++action_step_;
    if (active_index_ >= config_.chunk_length) {
        active_slot_ = -1;
        active_index_ = 0;
    }
    if (active_slot_ >= 0 && !in_flight_ && waiting_slot_ < 0 &&
        remaining_actions() <= config_.execute_horizon) {
        (void)request();  /* prefetch best-effort; explicit poll reports errs */
    }
    return ActionChunkState::kReady;
}

int ActionChunkMode::advance_step() {
    ++action_step_;
    return CAP_OK;
}

ActionChunkState ActionChunkMode::sync_next_chunk() {
    if (!runner_ || !runner_->ok()) return ActionChunkState::kError;
    if (!in_flight_) {
        int rc = request();
        if (rc != CAP_OK) {
            last_error_ = rc;
            return ActionChunkState::kError;
        }
    }
    int rc = runner_->sync(config_.action_stage);
    if (rc != CAP_OK) {
        last_error_ = rc;
        in_flight_ = false;
        return ActionChunkState::kError;
    }
    return poll();
}

int ActionChunkMode::set_state(const float* state, uint32_t dim) {
    if (!state || !config_.state_dim || dim != config_.state_dim)
        return CAP_ERR_ARG;
    for (uint32_t i = 0; i < dim; ++i)
        if (!std::isfinite(state[i])) return CAP_ERR_ARG;
    std::copy(state, state + dim, state_latest_.begin());
    has_state_ = true;
    ++state_updates_;
    return CAP_OK;
}

int ActionChunkMode::set_state_action_indices(const uint32_t* indices,
                                              uint32_t n) {
    if (requested_once_) return CAP_ERR_ARG;
    if (!indices || !n || n != config_.state_dim) return CAP_ERR_ARG;
    const uint32_t action_dim = config_.action_bytes / sizeof(float);
    bool has_mapped_dimension = false;
    for (uint32_t i = 0; i < n; ++i) {
        if (indices[i] == UINT32_MAX) continue;
        if (indices[i] >= action_dim) return CAP_ERR_ARG;
        has_mapped_dimension = true;
        for (uint32_t j = i + 1; j < n; ++j)
            if (indices[j] != UINT32_MAX && indices[i] == indices[j])
                return CAP_ERR_ARG;
    }
    if (!has_mapped_dimension) return CAP_ERR_ARG;
    state_action_indices_.assign(indices, indices + n);
    return CAP_OK;
}

void ActionChunkMode::reset() {
    in_flight_ = false;
    begun_ = false;
    has_held_ = false;
    consume_fused_ = false;
    waiting_slot_ = -1;
    has_raw_prev_ = false;
    retained_.clear();
    std::fill(slot_valid_.begin(), slot_valid_.end(), 0);
    pending_ticks_ = 0;
    pending_slot_ = -1;
    active_slot_ = -1;
    active_index_ = 0;
    deadline_reported_ = false;
    last_error_ = CAP_OK;
}

uint32_t ActionChunkMode::remaining_actions() const {
    if (active_slot_ < 0 || active_index_ >= config_.chunk_length) return 0;
    return config_.chunk_length - active_index_;
}

bool ActionChunkMode::chunking_enabled() const {
    return config_.output_port != UINT32_MAX && chunk_bytes_ &&
           config_.chunk_length && config_.action_bytes &&
           storage_.size() >= chunk_bytes_ * config_.ring_slots;
}

unsigned char* ActionChunkMode::slot_ptr(int slot) {
    return storage_.data() + static_cast<uint64_t>(slot) * chunk_bytes_;
}

uint32_t ActionChunkMode::latency_index(uint64_t start_step) const {
    int64_t idx = static_cast<int64_t>(action_step_ - start_step) +
                  config_.switch_offset;
    if (idx < 0) idx = 0;
    const int64_t last = static_cast<int64_t>(config_.chunk_length) - 1;
    if (idx > last) idx = last;
    return static_cast<uint32_t>(idx);
}

/* Reference semantics: estimate the state each action index implies (f64),
 * pick the index closest to the measured state. Estimates read the f32
 * action values (for fusion: the f32-cast fused chunk, matching the
 * reference, which searches the cast array). Distances accumulate in
 * declaration order; cross-implementation equality is tolerance-graded,
 * the argmin index is exact (ties: lowest index wins). */
int ActionChunkMode::state_index(const unsigned char* chunk,
                                 uint32_t* out) const {
    const uint32_t action_dim = config_.action_bytes / 4;
    const uint32_t state_dim = config_.state_dim;
    if (!has_state_) return CAP_ERR_ARG;
    if (state_action_indices_.empty()) {
        if (state_dim > action_dim) return CAP_ERR_ARG;
    } else {
        if (state_action_indices_.size() != state_dim) return CAP_ERR_ARG;
        for (uint32_t idx : state_action_indices_)
            if (idx != UINT32_MAX && idx >= action_dim) return CAP_ERR_ARG;
    }
    std::vector<double>& cum = const_cast<std::vector<double>&>(state_cum_);
    std::fill(cum.begin(), cum.end(), 0.0);
    double best = 0.0;
    uint32_t best_index = 0;
    for (uint32_t i = 0; i < config_.chunk_length; ++i) {
        const float* row = reinterpret_cast<const float*>(
            chunk + static_cast<uint64_t>(i) * config_.action_bytes);
        double dist = 0.0;
        for (uint32_t k = 0; k < state_dim; ++k) {
            const uint32_t col =
                state_action_indices_.empty() ? k : state_action_indices_[k];
            if (col == UINT32_MAX) continue;
            const double a = static_cast<double>(row[col]);
            double estimate;
            switch (config_.action_representation) {
                case kActionChunkReprDeltaCumulative:
                    cum[k] += a;
                    estimate = static_cast<double>(state_fire_[k]) + cum[k];
                    break;
                case kActionChunkReprDeltaFromStart:
                    estimate = static_cast<double>(state_fire_[k]) + a;
                    break;
                default:
                    estimate = a;
                    break;
            }
            const double diff =
                estimate - static_cast<double>(state_latest_[k]);
            dist += config_.distance_metric ? diff * diff : std::fabs(diff);
        }
        if (config_.distance_metric) dist = std::sqrt(dist);
        if (i == 0 || dist < best) {
            best = dist;
            best_index = i;
        }
    }
    *out = best_index;
    return CAP_OK;
}

int ActionChunkMode::seat_switch() {
    uint32_t index = 0;
    if (config_.switch_mode == kActionChunkSwitchState) {
        int rc = state_index(slot_ptr(pending_slot_), &index);
        if (rc != CAP_OK) return rc;
    } else {
        index = latency_index(slot_start_step_[pending_slot_]);
    }
    active_slot_ = pending_slot_;
    pending_slot_ = -1;
    active_index_ = index;
    consume_fused_ = false;
    return CAP_OK;
}

void ActionChunkMode::prune_expired() {
    size_t keep = 0;
    for (size_t r = 0; r < retained_.size(); ++r) {
        const int slot = retained_[r];
        if (slot_start_step_[slot] + config_.chunk_length <= action_step_) {
            slot_valid_[slot] = 0;
            ++pruned_chunks_;
        } else {
            retained_[keep++] = retained_[r];
        }
    }
    retained_.resize(keep);
}

int ActionChunkMode::seat_fusion() {
    prune_expired();
    slot_valid_[pending_slot_] = 1;
    retained_.push_back(pending_slot_);

    /* Fuse on the newest chunk's step window: per index, the newest (at most
     * fusion_max_chunks) retained chunks covering that step contribute
     * exp-decay weights from the table; f64 accumulation in newest-first
     * order, one divide, cast to f32. Raw chunks stay immutable. */
    const uint64_t start = slot_start_step_[pending_slot_];
    const uint32_t length = config_.chunk_length;
    const uint32_t action_dim = config_.action_bytes / 4;
    for (uint32_t i = 0; i < length; ++i) {
        const uint64_t step = start + i;
        std::fill(fusion_acc_.begin(), fusion_acc_.end(), 0.0);
        double weight_sum = 0.0;
        uint32_t count = 0;
        for (auto it = retained_.rbegin();
             it != retained_.rend() && count < config_.fusion_max_chunks;
             ++it) {
            const int slot = *it;
            const uint64_t slot_start = slot_start_step_[slot];
            if (step < slot_start || step >= slot_start + length) continue;
            const uint32_t source_i = static_cast<uint32_t>(step - slot_start);
            const double w =
                weight_table_[source_i >= i ? source_i - i : i - source_i];
            const float* row = reinterpret_cast<const float*>(
                slot_ptr(slot) +
                static_cast<uint64_t>(source_i) * config_.action_bytes);
            for (uint32_t k = 0; k < action_dim; ++k)
                fusion_acc_[k] += w * static_cast<double>(row[k]);
            weight_sum += w;
            ++count;
        }
        float* fused_row = reinterpret_cast<float*>(
            fused_.data() + static_cast<uint64_t>(i) * config_.action_bytes);
        for (uint32_t k = 0; k < action_dim; ++k)
            fused_row[k] = static_cast<float>(fusion_acc_[k] / weight_sum);
    }

    uint32_t index = 0;
    if (config_.switch_mode == kActionChunkSwitchState) {
        int rc = state_index(fused_.data(), &index);
        if (rc != CAP_OK) return rc;
    } else {
        index = latency_index(start);
    }
    active_slot_ = pending_slot_;
    pending_slot_ = -1;
    active_index_ = index;
    consume_fused_ = true;
    return CAP_OK;
}

/* Integrate the next k delta actions of the executing chunk into the
 * measured state (sequential f64 accumulation; the reference sums with
 * numpy's pairwise reduction, so cross-implementation equality of the f32
 * result is graded to <= 1 ulp). k clips to the chunk remainder; with no
 * active chunk the projection is the measured state itself (k = 0). */
int ActionChunkMode::prepare_projected() {
    if (!has_state_) return CAP_ERR_ARG;
    const uint32_t action_dim = config_.action_bytes / 4;
    const uint32_t state_dim = config_.state_dim;
    if (state_action_indices_.empty()) {
        if (state_dim > action_dim) return CAP_ERR_ARG;
    } else {
        if (state_action_indices_.size() != state_dim) return CAP_ERR_ARG;
        for (uint32_t idx : state_action_indices_)
            if (idx != UINT32_MAX && idx >= action_dim) return CAP_ERR_ARG;
    }
    uint32_t count = 0;
    std::fill(state_cum_.begin(), state_cum_.end(), 0.0);
    if (active_slot_ >= 0) {
        /* Integrate what the robot will actually execute: the fused values
         * under a fusion consume, the raw chunk otherwise. */
        const unsigned char* source =
            consume_fused_ ? fused_.data() : slot_ptr(active_slot_);
        const uint32_t begin = active_index_;
        const uint32_t end = begin + config_.lookahead_steps >
                                     config_.chunk_length
                                 ? config_.chunk_length
                                 : begin + config_.lookahead_steps;
        for (uint32_t i = begin; i < end; ++i) {
            const float* row = reinterpret_cast<const float*>(
                source + static_cast<uint64_t>(i) * config_.action_bytes);
            for (uint32_t k = 0; k < state_dim; ++k) {
                const uint32_t col = state_action_indices_.empty()
                                         ? k : state_action_indices_[k];
                if (col == UINT32_MAX) continue;
                state_cum_[k] += static_cast<double>(row[col]);
            }
        }
        count = end - begin;
    }
    for (uint32_t k = 0; k < state_dim; ++k)
        projected_[k] = static_cast<float>(
            static_cast<double>(state_latest_[k]) + state_cum_[k]);
    projected_count_ = count;
    return CAP_OK;
}

int ActionChunkMode::projected_state(float* out, uint32_t capacity_dims,
                                     uint32_t* written_dims) const {
    if (written_dims) *written_dims = config_.state_dim;
    if (!out || projected_.empty() || capacity_dims < config_.state_dim)
        return CAP_ERR_ARG;
    std::copy(projected_.begin(), projected_.end(), out);
    return CAP_OK;
}

/* projected_state seating: index 0 is pinned to fire_step + k. Landing at
 * or past the start step seats immediately, skipping the stale prefix;
 * landing early parks the chunk as waiting -- the old chunk keeps serving
 * and consumption switches exactly at the start step. */
int ActionChunkMode::seat_projected() {
    const uint64_t start = slot_start_step_[pending_slot_];
    if (action_step_ >= start) {
        uint64_t idx = action_step_ - start;
        const uint64_t last = config_.chunk_length - 1;
        if (idx > last) idx = last;
        active_slot_ = pending_slot_;
        active_index_ = static_cast<uint32_t>(idx);
        consume_fused_ = false;
    } else {
        waiting_slot_ = pending_slot_;
    }
    pending_slot_ = -1;
    return CAP_OK;
}

/* Stage the previous raw chunk re-indexed onto the new frame: new row i
 * takes the raw action the robot will be executing i steps after this fire,
 * i.e. old row (consumption index + i), clipped to the last row. Before any
 * chunk exists the stage image is zero -- a documented cold start; real
 * deployments warm up through the plain plan. */
int ActionChunkMode::prepare_prev_chunk() {
    if (prev_staged_.empty()) return CAP_ERR_ARG;
    if (!has_raw_prev_) {
        std::fill(prev_staged_.begin(), prev_staged_.end(), 0);
        return CAP_OK;
    }
    const uint32_t length = config_.chunk_length;
    const uint32_t row = config_.raw_action_bytes;
    const uint32_t base = active_slot_ >= 0 ? active_index_ : length;
    for (uint32_t i = 0; i < length; ++i) {
        uint32_t src = base + i;
        if (src > length - 1) src = length - 1;
        std::memcpy(prev_staged_.data() + static_cast<uint64_t>(i) * row,
                    prev_raw_.data() + static_cast<uint64_t>(src) * row,
                    row);
    }
    return CAP_OK;
}

int ActionChunkMode::prev_chunk_staged(void* out, uint64_t capacity,
                                       uint64_t* written) const {
    if (written) *written = prev_staged_.size();
    if (!out || prev_staged_.empty() || capacity < prev_staged_.size())
        return CAP_ERR_ARG;
    std::memcpy(out, prev_staged_.data(), prev_staged_.size());
    return CAP_OK;
}

void ActionChunkMode::promote_waiting() {
    if (waiting_slot_ < 0 ||
        action_step_ < slot_start_step_[waiting_slot_])
        return;
    if (config_.consume_policy == kActionChunkConsumeTemporalFusion) {
        /* Deferred fusion: the chunk becomes consumable now; prune at the
         * current step, fuse, and seat (latency index, cannot fail). */
        pending_slot_ = waiting_slot_;
        waiting_slot_ = -1;
        (void)seat_fusion();
        return;
    }
    uint64_t idx = action_step_ - slot_start_step_[waiting_slot_];
    const uint64_t last = config_.chunk_length - 1;
    if (idx > last) idx = last;
    active_slot_ = waiting_slot_;
    waiting_slot_ = -1;
    active_index_ = static_cast<uint32_t>(idx);
    consume_fused_ = false;
}

int ActionChunkMode::copy_output_to_pending_slot() {
    if (!chunking_enabled() || pending_slot_ < 0) return CAP_OK;
    cap_model_runtime* model = runner_->model();
    if (!model) return CAP_ERR_ARG;
    uint64_t written = 0;
    int rc = cap_model_get_output(model, config_.output_port,
                                  slot_ptr(pending_slot_), chunk_bytes_,
                                  &written, -1);
    if (rc != CAP_OK) return rc;
    if (written != chunk_bytes_) return CAP_ERR_ARG;
    return CAP_OK;
}

}  // namespace nexus
