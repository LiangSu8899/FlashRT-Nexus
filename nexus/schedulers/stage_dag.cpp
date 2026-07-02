#include "nexus/schedulers/stage_dag.h"

namespace nexus {

StageDagRunner::StageDagRunner(cap_ctx ctx, cap_model_runtime* model)
    : ctx_(ctx), model_(model), backend_(model ? model->backend : nullptr) {
    if (!ctx_ || !model_ || !backend_ || !backend_->event ||
        !backend_->event_record || !backend_->event_query ||
        !backend_->stream_wait || !backend_->sync || !backend_->event_free) {
        last_error_ = CAP_ERR_ARG;
        return;
    }
    done_.assign(model_->n_stages, nullptr);
    in_flight_.assign(model_->n_stages, 0);
    has_event_.assign(model_->n_stages, 0);
    for (uint64_t i = 0; i < model_->n_stages; ++i) {
        done_[i] = backend_->event(backend_->self);
        if (!done_[i]) {
            last_error_ = CAP_ERR_BACKEND;
            return;
        }
    }
    ok_ = true;
}

StageDagRunner::~StageDagRunner() {
    if (!backend_ || !backend_->event_free) return;
    for (cap_event ev : done_)
        if (ev) backend_->event_free(backend_->self, ev);
}

int StageDagRunner::fire(uint64_t stage_index) {
    if (!ok_ || stage_index >= model_->n_stages) return CAP_ERR_ARG;
    if (in_flight_[stage_index]) return CAP_ERR_ARG;
    const cap_model_stage* stage = &model_->stages[stage_index];
    for (uint32_t i = 0; i < stage->n_after; ++i) {
        const uint32_t dep = stage->after[i];
        if (dep >= stage_index) return CAP_ERR_ARG;
        if (!has_event_[dep]) return CAP_ERR_ARG;
        if (model_->stages[dep].stream == stage->stream) continue;
        if (!done_[dep] ||
            backend_->stream_wait(backend_->self, stage->stream,
                                  done_[dep]) != 0) {
            last_error_ = CAP_ERR_BACKEND;
            return last_error_;
        }
    }
    int rc = cap_model_fire(ctx_, model_, stage_index);
    if (rc != CAP_OK) {
        last_error_ = rc;
        return rc;
    }
    if (backend_->event_record(backend_->self, done_[stage_index],
                               stage->stream) != 0) {
        last_error_ = CAP_ERR_BACKEND;
        return last_error_;
    }
    in_flight_[stage_index] = 1;
    has_event_[stage_index] = 1;
    int q = backend_->event_query(backend_->self, done_[stage_index]);
    if (q == 0) in_flight_[stage_index] = 0;
    else if (q < 0) {
        in_flight_[stage_index] = 0;
        last_error_ = q;
        return q;
    }
    last_error_ = CAP_OK;
    return CAP_OK;
}

int StageDagRunner::run_once() {
    if (!ok_) return CAP_ERR_ARG;
    for (uint64_t i = 0; i < model_->n_stages; ++i) {
        int rc = fire(i);
        if (rc != CAP_OK) return rc;
    }
    return CAP_OK;
}

int StageDagRunner::run_mask(uint64_t stage_mask) {
    if (!ok_) return CAP_ERR_ARG;
    if (model_->n_stages > 64) return CAP_ERR_ARG;
    for (uint64_t i = 0; i < model_->n_stages; ++i) {
        if ((stage_mask & (1ull << i)) == 0) continue;
        int rc = fire(i);
        if (rc != CAP_OK) return rc;
    }
    return CAP_OK;
}

int StageDagRunner::run_due(uint64_t tick, const uint32_t* periods,
                            const uint32_t* phases, uint64_t n_periods) {
    if (!ok_ || !periods || n_periods < model_->n_stages ||
        model_->n_stages > 64) {
        return CAP_ERR_ARG;
    }
    uint64_t mask = 0;
    for (uint64_t i = 0; i < model_->n_stages; ++i) {
        const uint32_t period = periods[i];
        if (!period) continue;
        const uint32_t phase = phases ? phases[i] : 0;
        if ((tick + period - (phase % period)) % period == 0)
            mask |= (1ull << i);
    }
    return run_mask(mask);
}

int StageDagRunner::query(uint64_t stage_index) {
    if (!ok_ || stage_index >= done_.size() || !done_[stage_index])
        return CAP_ERR_ARG;
    int rc = backend_->event_query(backend_->self, done_[stage_index]);
    if (rc == 0) in_flight_[stage_index] = 0;
    return rc;
}

int StageDagRunner::sync(uint64_t stage_index) {
    if (!ok_ || stage_index >= model_->n_stages) return CAP_ERR_ARG;
    int rc = backend_->sync(backend_->self, model_->stages[stage_index].stream);
    if (rc == CAP_OK) in_flight_[stage_index] = 0;
    return rc;
}

bool StageDagRunner::in_flight(uint64_t stage_index) const {
    return stage_index < in_flight_.size() && in_flight_[stage_index] != 0;
}

bool StageDagRunner::has_event(uint64_t stage_index) const {
    return stage_index < has_event_.size() && has_event_[stage_index] != 0;
}

}  // namespace nexus
