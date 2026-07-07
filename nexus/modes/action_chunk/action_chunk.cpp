#include "nexus/modes/action_chunk/action_chunk.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace nexus {

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
    if (config.prepare_policy != kActionChunkPrepareNone) return CAP_ERR_ARG;
    if (config.consume_policy != kActionChunkConsumePlain) return CAP_ERR_ARG;
    if (config.switch_mode > kActionChunkSwitchState) return CAP_ERR_ARG;
    if (config.miss_policy > kActionChunkMissHoldLast) return CAP_ERR_ARG;
    if (config.scalar_dtype > kActionChunkDtypeF16) return CAP_ERR_ARG;
    if (config.action_representation > kActionChunkReprDeltaFromStart)
        return CAP_ERR_ARG;
    if (config.distance_metric > 1) return CAP_ERR_ARG;
    if (config.candidates > 1) return CAP_ERR_ARG;
    return CAP_OK;
}

ActionChunkMode::ActionChunkMode(
        StageDagRunner* runner, const ActionChunkConfig& config)
    : runner_(runner), config_(config) {
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
    }
    if (config_.state_dim) {
        state_latest_.resize(config_.state_dim, 0.0f);
        state_fire_.resize(config_.state_dim, 0.0f);
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
    if (in_flight_) return CAP_ERR_ARG;
    if (begun_) return CAP_OK;
    if (config_.state_dim && has_state_)
        std::copy(state_latest_.begin(), state_latest_.end(),
                  state_fire_.begin());
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
        int next = active_slot_ < 0 ? 0 : (active_slot_ + 1) %
                                           (int)config_.ring_slots;
        pending_slot_ = next;
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
            int rc = copy_output_to_pending_slot();
            if (rc != CAP_OK) {
                last_error_ = rc;
                in_flight_ = false;
                pending_slot_ = -1;
                return ActionChunkState::kError;
            }
            slot_ready_step_[pending_slot_] = action_step_;
            /* consume = plain: the ready chunk is seated immediately,
             * addresses the grid from its fire step, consumed from index 0. */
            slot_start_step_[pending_slot_] = slot_fire_step_[pending_slot_];
            active_slot_ = pending_slot_;
            pending_slot_ = -1;
            active_index_ = 0;
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
    if (active_slot_ < 0) {
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
    const unsigned char* src = slot_ptr(active_slot_) +
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
    if (active_slot_ >= 0 && !in_flight_ &&
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
    if (!indices || !n) return CAP_ERR_ARG;
    for (uint32_t i = 0; i < n; ++i)
        for (uint32_t j = i + 1; j < n; ++j)
            if (indices[i] == indices[j]) return CAP_ERR_ARG;
    state_action_indices_.assign(indices, indices + n);
    return CAP_OK;
}

void ActionChunkMode::reset() {
    in_flight_ = false;
    begun_ = false;
    has_held_ = false;
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
