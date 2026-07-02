#include "nexus/schedulers/stage_dag_c.h"

#include "nexus/schedulers/stage_dag.h"

#include <new>

struct nexus_stage_dag_s {
    nexus::StageDagRunner runner;
    nexus_stage_dag_s(cap_ctx ctx, cap_model_runtime* model)
        : runner(ctx, model) {}
};

extern "C" int nexus_stage_dag_create(cap_ctx ctx, cap_model_runtime* model,
                                      nexus_stage_dag** out) {
    if (!ctx || !model || !out) return CAP_ERR_ARG;
    *out = nullptr;
    auto* h = new (std::nothrow) nexus_stage_dag_s(ctx, model);
    if (!h) return CAP_ERR_NOMEM;
    if (!h->runner.ok()) {
        int rc = h->runner.last_error();
        delete h;
        return rc ? rc : CAP_ERR;
    }
    *out = h;
    return CAP_OK;
}

extern "C" void nexus_stage_dag_destroy(nexus_stage_dag* h) {
    delete h;
}

extern "C" int nexus_stage_dag_fire(nexus_stage_dag* h,
                                    uint64_t stage_index) {
    return h ? h->runner.fire(stage_index) : CAP_ERR_ARG;
}

extern "C" int nexus_stage_dag_run_once(nexus_stage_dag* h) {
    return h ? h->runner.run_once() : CAP_ERR_ARG;
}

extern "C" int nexus_stage_dag_run_mask(nexus_stage_dag* h,
                                        uint64_t stage_mask) {
    return h ? h->runner.run_mask(stage_mask) : CAP_ERR_ARG;
}

extern "C" int nexus_stage_dag_run_due(nexus_stage_dag* h, uint64_t tick,
                                       const uint32_t* periods,
                                       const uint32_t* phases,
                                       uint64_t n_periods) {
    return h ? h->runner.run_due(tick, periods, phases, n_periods)
             : CAP_ERR_ARG;
}

extern "C" int nexus_stage_dag_query(nexus_stage_dag* h,
                                     uint64_t stage_index) {
    return h ? h->runner.query(stage_index) : CAP_ERR_ARG;
}

extern "C" int nexus_stage_dag_sync(nexus_stage_dag* h,
                                    uint64_t stage_index) {
    return h ? h->runner.sync(stage_index) : CAP_ERR_ARG;
}

extern "C" int nexus_stage_dag_in_flight(nexus_stage_dag* h,
                                         uint64_t stage_index) {
    return h ? (h->runner.in_flight(stage_index) ? 1 : 0) : CAP_ERR_ARG;
}

extern "C" int nexus_stage_dag_has_event(nexus_stage_dag* h,
                                         uint64_t stage_index) {
    return h ? (h->runner.has_event(stage_index) ? 1 : 0) : CAP_ERR_ARG;
}

extern "C" int nexus_stage_dag_last_error(nexus_stage_dag* h) {
    return h ? h->runner.last_error() : CAP_ERR_ARG;
}

nexus::StageDagRunner* nexus_stage_dag_runner(nexus_stage_dag* h) {
    return h ? &h->runner : nullptr;
}
