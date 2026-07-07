#include "nexus/modes/action_chunk/action_chunk.h"

#include <cstring>
#include <limits>

namespace nexus {

int ActionChunkMode::config_from_output_port(
        StageDagRunner* runner, uint64_t action_stage, uint32_t output_port,
        uint32_t scalar_bytes, uint32_t ring_slots, uint32_t execute_horizon,
        int deadline_ticks, ActionChunkConfig* out) {
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
    cfg.deadline_ticks = deadline_ticks;
    *out = cfg;
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
    }
}

ActionChunkMode::ActionChunkMode(StageDagRunner* runner,
                                       uint64_t action_stage,
                                       int max_pending_polls)
    : ActionChunkMode(
          runner,
          ActionChunkConfig{action_stage, UINT32_MAX, 0, 0, 1, 1,
                               max_pending_polls}) {}

int ActionChunkMode::request() {
    if (!runner_ || !runner_->ok()) return CAP_ERR_ARG;
    if (in_flight_) return CAP_ERR_ARG;
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
    pending_ticks_ = 0;
    deadline_reported_ = false;
    last_error_ = CAP_OK;
    return CAP_OK;
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
        if (deadline_reported_) ++late_chunks_;
        if (chunking_enabled()) {
            int rc = copy_output_to_pending_slot();
            if (rc != CAP_OK) {
                last_error_ = rc;
                in_flight_ = false;
                pending_slot_ = -1;
                return ActionChunkState::kError;
            }
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
    if (config_.deadline_ticks >= 0 &&
        pending_ticks_ > config_.deadline_ticks && !deadline_reported_) {
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
        return poll();
    }
    if (capacity < config_.action_bytes) {
        last_error_ = CAP_ERR_ARG;
        return ActionChunkState::kError;
    }
    const unsigned char* src = slot_ptr(active_slot_) +
        static_cast<uint64_t>(active_index_) * config_.action_bytes;
    std::memcpy(out, src, config_.action_bytes);
    ++active_index_;
    ++emitted_actions_;
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

void ActionChunkMode::reset() {
    in_flight_ = false;
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
