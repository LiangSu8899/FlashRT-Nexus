/* model_runtime.cpp — lookups, tick sugar, and FFI accessors over the
 * standard model-runtime face (see header). Mechanism-thin: everything here
 * is a direct composition of core verbs and struct reads. */
#include "capsule/model_runtime.h"
#include "model_executor_internal.h"

#include <cstring>

extern "C" int cap_model_find_port(const cap_model_runtime* m,
                                   const char* name) {
    if (!m || !name) return -1;
    for (uint64_t i = 0; i < m->n_ports; ++i)
        if (m->ports[i].name && std::strcmp(m->ports[i].name, name) == 0)
            return (int)i;
    return -1;
}

extern "C" int cap_model_fire(cap_ctx c, const cap_model_runtime* m,
                              uint64_t stage_index) {
    if (!c || !m || stage_index >= m->n_stages) return CAP_ERR_ARG;
    if (!m->schedule.stages) return CAP_ERR_ARG;
    return cap_fire(c, &m->schedule.stages[stage_index]);
}

namespace {

const cap_model_executor_ops_v1* executor_ops(
        const cap_model_runtime* m, int* status) {
    if (status) *status = CAP_ERR_ARG;
    if (!m || !m->impl) return nullptr;
    const auto* ops = static_cast<const cap_model_executor_ops_v1*>(m->impl);
    if (ops->magic != CAP_MODEL_EXECUTOR_OPS_MAGIC ||
        !ops->stage_kind || !ops->execute) {
        if (status) *status = CAP_ERR_FORMAT;
        return nullptr;
    }
    if (ops->version != CAP_MODEL_EXECUTOR_OPS_VERSION ||
        ops->struct_size < CAP_MODEL_EXECUTOR_OPS_V1_SIZE) {
        if (status) *status = CAP_ERR_VERSION;
        return nullptr;
    }
    if (status) *status = CAP_OK;
    return ops;
}

bool all_graph_stages(const cap_model_runtime* m) {
    if (!m || !m->n_stages) return false;
    for (uint64_t i = 0; i < m->n_stages; ++i)
        if (!m->stages[i].graph) return false;
    return true;
}

}  // namespace

extern "C" uint32_t cap_model_stage_executor_kind(
        const cap_model_runtime* m, uint64_t stage_index) {
    if (!m || stage_index >= m->n_stages) return UINT32_MAX;
    if (m->stages[stage_index].graph) return CAP_MODEL_EXECUTOR_GRAPH;
    int status = CAP_OK;
    const auto* ops = executor_ops(m, &status);
    if (!ops) return UINT32_MAX;
    const uint32_t kind = ops->stage_kind(ops->payload, stage_index);
    return kind <= CAP_MODEL_EXECUTOR_OPAQUE ? kind : UINT32_MAX;
}

extern "C" int cap_model_execute_stage(cap_ctx c,
                                        const cap_model_runtime* m,
                                        uint64_t stage_index) {
    if (!m || stage_index >= m->n_stages) return CAP_ERR_ARG;
    if (m->stages[stage_index].graph)
        return c ? cap_model_fire(c, m, stage_index) : CAP_ERR_ARG;
    int status = CAP_OK;
    const auto* ops = executor_ops(m, &status);
    if (!ops) return status;
    if (ops->stage_kind(ops->payload, stage_index) !=
        CAP_MODEL_EXECUTOR_OPAQUE) return CAP_ERR_FORMAT;
    return ops->execute(ops->payload, stage_index);
}

extern "C" int cap_model_tick(cap_ctx c, const cap_model_runtime* m) {
    if (!m) return CAP_ERR_ARG;
    if (!m->n_stages)
        return m->step ? m->step(m->self) : CAP_ERR_ARG;
    if (!all_graph_stages(m)) {
        for (uint64_t i = 0; i < m->n_stages; ++i) {
            const cap_model_stage* s = &m->stages[i];
            for (uint32_t k = 0; k < s->n_after; ++k) {
                const uint32_t dep = s->after[k];
                if (dep >= i) return CAP_ERR_FORMAT;
                if (!s->graph && m->stages[dep].graph) {
                    if (!c) return CAP_ERR_ARG;
                    const int rc = cap_sync(c, m->stages[dep].stream);
                    if (rc != CAP_OK) return rc;
                }
            }
            const int rc = cap_model_execute_stage(c, m, i);
            if (rc != CAP_OK) return rc;
        }
        return CAP_OK;
    }
    if (!c) return CAP_ERR_ARG;
    if (!m->stage_events) {           /* hand-built runtime: allocating fallback */
        int failed = -1;
        return cap_drive_tick(c, &m->schedule, 0, &failed);
    }
    /* alloc-free hot path: pre-created events, core verbs only */
    cap_backend* be = m->backend;
    for (uint64_t i = 0; i < m->n_stages; ++i) {
        const cap_model_stage* s = &m->stages[i];
        for (uint32_t k = 0; k < s->n_after; ++k) {
            const uint32_t dep = s->after[k];
            if (m->stages[dep].stream == s->stream) continue;  /* FIFO order */
            cap_event ev = m->stage_events[dep];
            if (!ev || be->stream_wait(be->self, s->stream, ev) != 0)
                return CAP_ERR_BACKEND;
        }
        int rc = cap_fire(c, &m->schedule.stages[i]);
        if (rc != CAP_OK) return rc;
        if (m->stage_events[i] &&
            be->event_record(be->self, m->stage_events[i], s->stream) != 0)
            return CAP_ERR_BACKEND;
    }
    return CAP_OK;
}

extern "C" int cap_model_state_status(const cap_model_runtime* m) {
    return m && all_graph_stages(m) && m->regions && m->n_regions
               ? CAP_OK : CAP_ERR_ARG;
}

extern "C" cap_capsule cap_model_snapshot(cap_ctx c,
                                           const cap_model_runtime* m,
                                           int tier, int stream) {
    if (!c || cap_model_state_status(m) != CAP_OK) return nullptr;
    cap_boundary boundary{m->regions, static_cast<int>(m->n_regions),
                          nullptr, 0};
    return cap_snapshot(c, &boundary, tier, stream);
}

extern "C" int cap_model_restore(cap_ctx c, const cap_model_runtime* m,
                                  cap_capsule capsule, int stream) {
    if (!c || !capsule || cap_model_state_status(m) != CAP_OK)
        return CAP_ERR_ARG;
    return cap_restore(c, capsule, stream);
}

extern "C" int cap_model_restore_into(cap_ctx c,
                                       const cap_model_runtime* m,
                                       cap_capsule capsule, int stream) {
    if (!c || !capsule || cap_model_state_status(m) != CAP_OK)
        return CAP_ERR_ARG;
    return cap_restore_into(c, capsule, m->regions,
                            static_cast<int>(m->n_regions), stream);
}

extern "C" cap_backend* cap_model_backend(cap_model_runtime* m) {
    return m ? m->backend : nullptr;
}
extern "C" uint64_t cap_model_fingerprint(const cap_model_runtime* m) {
    return m ? m->fingerprint : 0;
}
extern "C" const char* cap_model_identity(const cap_model_runtime* m) {
    return m && m->identity ? m->identity : "";
}
extern "C" uint64_t cap_model_n_ports(const cap_model_runtime* m) {
    return m ? m->n_ports : 0;
}
extern "C" cap_buffer cap_model_port_buffer(const cap_model_runtime* m,
                                            uint64_t port) {
    return (m && port < m->n_ports) ? m->ports[port].buffer : nullptr;
}
extern "C" uint64_t cap_model_port_bytes(const cap_model_runtime* m,
                                         uint64_t port) {
    return (m && port < m->n_ports) ? m->ports[port].bytes : 0;
}
extern "C" uint32_t cap_model_port_update(const cap_model_runtime* m,
                                          uint64_t port) {
    return (m && port < m->n_ports) ? m->ports[port].update : ~0u;
}
extern "C" uint64_t cap_model_n_stages(const cap_model_runtime* m) {
    return m ? m->n_stages : 0;
}
extern "C" int cap_model_stage_stream(const cap_model_runtime* m,
                                      uint64_t stage) {
    return (m && stage < m->n_stages) ? m->stages[stage].stream : -1;
}
extern "C" cap_region* cap_model_region_array(const cap_model_runtime* m) {
    return m ? m->regions : nullptr;
}
extern "C" int cap_model_region_count(const cap_model_runtime* m) {
    return m ? (int)m->n_regions : 0;
}
extern "C" int cap_model_set_input(cap_model_runtime* m, uint32_t port,
                                   const void* data, uint64_t bytes,
                                   int stream) {
    if (!m || !m->set_input) return CAP_ERR_ARG;
    return m->set_input(m->self, port, data, bytes, stream);
}
extern "C" int cap_model_get_output(cap_model_runtime* m, uint32_t port,
                                    void* out, uint64_t capacity,
                                    uint64_t* written, int stream) {
    if (!m || !m->get_output) return CAP_ERR_ARG;
    return m->get_output(m->self, port, out, capacity, written, stream);
}
extern "C" const char* cap_model_last_error(cap_model_runtime* m) {
    if (!m || !m->last_error) return "";
    return m->last_error(m->self);
}
