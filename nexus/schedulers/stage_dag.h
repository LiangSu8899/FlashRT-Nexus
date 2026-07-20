/* nexus/schedulers/stage_dag.h — L2 scheduler helper over cap_model_runtime.
 *
 * This is policy-thin: it does not know models, modalities, or RTC. It turns
 * the adopted stage DAG into fire/query primitives an outer scheduler can use
 * for overlap and interruption. It also provides mask and period/phase helpers
 * for multi-rate loops. Allocation happens at construction; fire/query and the
 * schedule helpers are hot-path compositions of capsule verbs.
 *
 * GRAPH stages retain event-based completion. OPAQUE stages are synchronous:
 * execute returns only after completion and no event is created. Discipline:
 * one stage has at most one in-flight replay. Dependencies may use
 * the last completed/fired event from an earlier stage; this lets a fast action
 * stage reuse a stale context stage, but it does not permit overlapping two
 * writes into the same producer-owned hand-off buffer. A producer that wants
 * cross-iteration overlap must capture/export double-buffered stages.
 */
#ifndef NEXUS_SCHEDULERS_STAGE_DAG_H
#define NEXUS_SCHEDULERS_STAGE_DAG_H

#include "capsule/model_runtime.h"

#include <cstdint>
#include <vector>

namespace nexus {

class StageDagRunner {
public:
    StageDagRunner(cap_ctx ctx, cap_model_runtime* model);
    ~StageDagRunner();

    StageDagRunner(const StageDagRunner&) = delete;
    StageDagRunner& operator=(const StageDagRunner&) = delete;

    bool ok() const { return ok_; }
    int last_error() const { return last_error_; }
    uint64_t stages() const { return model_ ? model_->n_stages : 0; }
    cap_model_runtime* model() const { return model_; }

    int fire(uint64_t stage_index);
    int run_once();
    int run_mask(uint64_t stage_mask);
    int run_due(uint64_t tick, const uint32_t* periods,
                const uint32_t* phases, uint64_t n_periods);
    int query(uint64_t stage_index);  /* 0 ready, >0 pending, <0 error */
    int sync(uint64_t stage_index);
    /* Read an OUT port. SWAP ports are served from their declared device
     * window (the swap fast lane's read side: download + sync); STAGED
     * ports go through the producer's get_output verb. Call only after
     * the producing stage completed (query == 0). */
    int read_output(uint32_t port_index, void* dst, uint64_t capacity,
                    uint64_t* written);
    bool in_flight(uint64_t stage_index) const;
    bool has_event(uint64_t stage_index) const;

private:
    cap_ctx ctx_ = nullptr;
    cap_model_runtime* model_ = nullptr;
    cap_backend* backend_ = nullptr;
    std::vector<cap_event> done_;
    std::vector<unsigned char> in_flight_;
    std::vector<unsigned char> has_event_;
    bool ok_ = false;
    int last_error_ = CAP_OK;
};

}  // namespace nexus

#endif  /* NEXUS_SCHEDULERS_STAGE_DAG_H */
