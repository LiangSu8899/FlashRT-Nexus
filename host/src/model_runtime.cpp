/* model_runtime.cpp — lookups, tick sugar, and FFI accessors over the
 * standard model-runtime face (see header). Mechanism-thin: everything here
 * is a direct composition of core verbs and struct reads. */
#include "capsule/model_runtime.h"

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

extern "C" int cap_model_tick(cap_ctx c, const cap_model_runtime* m) {
    if (!c || !m) return CAP_ERR_ARG;
    int failed = -1;
    return cap_drive_tick(c, &m->schedule, 0, &failed);
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
