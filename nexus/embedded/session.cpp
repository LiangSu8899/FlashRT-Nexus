#include "nexus/embedded/session.h"

#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <new>
#include <string>
#include <vector>

struct CapsuleRec {
    cap_capsule cap = nullptr;
    bool origin_restore = true;
};

struct nexus_embedded_session_s {
    cap_model_runtime* model = nullptr;
    cap_ctx ctx = nullptr;
    uint64_t chunk_id = 0;
    std::map<std::string, CapsuleRec> capsules;
    std::mutex mu;
    std::string last_error;

    ~nexus_embedded_session_s() {
        if (ctx) {
            for (auto& kv : capsules)
                cap_capsule_drop(ctx, kv.second.cap);
            capsules.clear();
            cap_ctx_destroy(ctx);
        }
    }

    int set_error(const char* msg, int rc) {
        last_error = msg ? msg : "";
        return rc;
    }
};

namespace {

bool valid_name(const char* s) {
    if (!s || !s[0]) return false;
    size_t n = 0;
    for (const unsigned char* p = (const unsigned char*)s; *p; ++p, ++n) {
        const bool ok = (*p >= 'A' && *p <= 'Z') ||
                        (*p >= 'a' && *p <= 'z') ||
                        (*p >= '0' && *p <= '9') ||
                        *p == '_' || *p == '.' || *p == '-';
        if (!ok || n >= 128) return false;
        if (n == 0 && !((*p >= 'A' && *p <= 'Z') ||
                        (*p >= 'a' && *p <= 'z') ||
                        (*p >= '0' && *p <= '9')))
            return false;
    }
    return true;
}

cap_boundary boundary(cap_model_runtime* m) {
    cap_boundary b{};
    b.regions = cap_model_region_array(m);
    b.n_regions = cap_model_region_count(m);
    return b;
}

int sync_all(nexus_embedded_session_s* s) {
    if (!s || !s->model) return CAP_ERR_ARG;
    int rc = CAP_OK;
    for (uint64_t i = 0; i < s->model->n_stages; ++i) {
        int sr = cap_sync(s->ctx, s->model->stages[i].stream);
        if (sr != CAP_OK && rc == CAP_OK) rc = sr;
    }
    return rc;
}

int port_index(nexus_embedded_session_s* s, const char* name) {
    if (!s || !s->model || !name) return CAP_ERR_ARG;
    int p = cap_model_find_port(s->model, name);
    if (p < 0) s->set_error("missing port", CAP_ERR_ARG);
    return p;
}

int apply_input_locked(nexus_embedded_session_s* s,
                       const nexus_embedded_input& in) {
    if (in.struct_size < sizeof(nexus_embedded_input) ||
        !in.port || !in.data)
        return CAP_ERR_ARG;
    int p = port_index(s, in.port);
    if (p < 0) return CAP_ERR_ARG;
    if (in.update == NEXUS_EMBEDDED_SET_INPUT) {
        int rc = cap_model_set_input(s->model, (uint32_t)p, in.data,
                                     in.bytes, in.stream);
        return rc == CAP_OK ? CAP_OK : s->set_error("set_input failed", rc);
    }
    if (in.update == NEXUS_EMBEDDED_SWAP) {
        cap_buffer b = cap_model_port_buffer(s->model, (uint64_t)p);
        if (!b) return s->set_error("port is not a SWAP buffer", CAP_ERR_ARG);
        int rc = cap_swap(s->ctx, b, in.data, (size_t)in.bytes, in.stream);
        return rc == CAP_OK ? CAP_OK : s->set_error("swap failed", rc);
    }
    return s->set_error("unknown input update", CAP_ERR_ARG);
}

int tick_locked(nexus_embedded_session_s* s,
                nexus_embedded_tick_result* result) {
    auto t0 = std::chrono::steady_clock::now();
    int rc = cap_model_tick(s->ctx, s->model);
    if (rc != CAP_OK) return s->set_error("tick failed", rc);
    rc = sync_all(s);
    if (rc != CAP_OK) return s->set_error("sync failed", rc);
    ++s->chunk_id;
    if (result) {
        result->struct_size = sizeof(*result);
        result->chunk_id = s->chunk_id;
        result->latency_ms =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();
    }
    return CAP_OK;
}

int fill_output_locked(nexus_embedded_session_s* s,
                       nexus_embedded_output& out) {
    if (out.struct_size < sizeof(nexus_embedded_output) ||
        !out.port || !out.data)
        return CAP_ERR_ARG;
    int p = port_index(s, out.port);
    if (p < 0) return CAP_ERR_ARG;
    uint64_t written = 0;
    int rc = cap_model_get_output(s->model, (uint32_t)p, out.data,
                                  out.capacity, &written, out.stream);
    out.written = written;
    return rc == CAP_OK ? CAP_OK : s->set_error("get_output failed", rc);
}

}  // namespace

extern "C" int nexus_embedded_open(const nexus_embedded_config* cfg,
                                   nexus_embedded_session** out) {
    if (!cfg || !out || cfg->struct_size < sizeof(nexus_embedded_config) ||
        !cfg->model || cfg->flags != 0)
        return CAP_ERR_ARG;
    *out = nullptr;
    auto* s = new (std::nothrow) nexus_embedded_session_s();
    if (!s) return CAP_ERR_NOMEM;
    s->model = cfg->model;
    s->ctx = cap_ctx_create(cap_model_backend(cfg->model));
    if (!s->ctx) {
        delete s;
        return CAP_ERR;
    }
    *out = s;
    return CAP_OK;
}

extern "C" void nexus_embedded_close(nexus_embedded_session* s) {
    delete s;
}

extern "C" cap_ctx nexus_embedded_ctx(nexus_embedded_session* s) {
    return s ? s->ctx : nullptr;
}

extern "C" cap_model_runtime* nexus_embedded_model(nexus_embedded_session* s) {
    return s ? s->model : nullptr;
}

extern "C" uint64_t nexus_embedded_fingerprint(nexus_embedded_session* s) {
    return s ? cap_model_fingerprint(s->model) : 0;
}

extern "C" const char* nexus_embedded_identity(nexus_embedded_session* s) {
    return s ? cap_model_identity(s->model) : "";
}

extern "C" const char* nexus_embedded_last_error(nexus_embedded_session* s) {
    return s ? s->last_error.c_str() : "null session";
}

extern "C" int nexus_embedded_find_port(nexus_embedded_session* s,
                                        const char* name) {
    return port_index(s, name);
}

extern "C" int nexus_embedded_set_input(nexus_embedded_session* s,
                                        const char* port,
                                        const void* data,
                                        uint64_t bytes,
                                        int stream) {
    if (!s || !data || !port) return CAP_ERR_ARG;
    std::lock_guard<std::mutex> lock(s->mu);
    nexus_embedded_input in{};
    in.struct_size = sizeof(in);
    in.port = port;
    in.data = data;
    in.bytes = bytes;
    in.update = NEXUS_EMBEDDED_SET_INPUT;
    in.stream = stream;
    return apply_input_locked(s, in);
}

extern "C" int nexus_embedded_swap(nexus_embedded_session* s,
                                   const char* port,
                                   const void* data,
                                   uint64_t bytes,
                                   int stream) {
    if (!s || !data || !port) return CAP_ERR_ARG;
    std::lock_guard<std::mutex> lock(s->mu);
    nexus_embedded_input in{};
    in.struct_size = sizeof(in);
    in.port = port;
    in.data = data;
    in.bytes = bytes;
    in.update = NEXUS_EMBEDDED_SWAP;
    in.stream = stream;
    return apply_input_locked(s, in);
}

extern "C" int nexus_embedded_tick(nexus_embedded_session* s,
                                   nexus_embedded_tick_result* result) {
    if (!s) return CAP_ERR_ARG;
    std::lock_guard<std::mutex> lock(s->mu);
    return tick_locked(s, result);
}

extern "C" int nexus_embedded_sync(nexus_embedded_session* s, int stream) {
    if (!s) return CAP_ERR_ARG;
    std::lock_guard<std::mutex> lock(s->mu);
    return cap_sync(s->ctx, stream);
}

extern "C" int nexus_embedded_get_output(nexus_embedded_session* s,
                                         const char* port,
                                         void* out,
                                         uint64_t capacity,
                                         uint64_t* written,
                                         int stream) {
    if (!s || !port || !out) return CAP_ERR_ARG;
    std::lock_guard<std::mutex> lock(s->mu);
    nexus_embedded_output desc{};
    desc.struct_size = sizeof(desc);
    desc.port = port;
    desc.data = out;
    desc.capacity = capacity;
    desc.stream = stream;
    int rc = fill_output_locked(s, desc);
    if (written) *written = desc.written;
    return rc;
}

extern "C" int nexus_embedded_act(nexus_embedded_session* s,
                                  const char* input_port,
                                  const void* input,
                                  uint64_t input_bytes,
                                  const char* output_port,
                                  void* output,
                                  uint64_t output_capacity,
                                  nexus_embedded_tick_result* result) {
    if (!s) return CAP_ERR_ARG;
    if (input_port) {
        int rc = nexus_embedded_set_input(s, input_port, input, input_bytes, -1);
        if (rc != CAP_OK) return rc;
    }
    int rc = nexus_embedded_tick(s, result);
    if (rc != CAP_OK) return rc;
    if (output_port) {
        uint64_t written = 0;
        rc = nexus_embedded_get_output(s, output_port, output,
                                       output_capacity, &written, -1);
        if (result) result->written = written;
    }
    return rc;
}

extern "C" int nexus_embedded_step(nexus_embedded_session* s,
                                   const nexus_embedded_input* inputs,
                                   uint64_t n_inputs,
                                   nexus_embedded_output* outputs,
                                   uint64_t n_outputs,
                                   nexus_embedded_tick_result* result) {
    if (!s) return CAP_ERR_ARG;
    if (n_inputs && !inputs) return CAP_ERR_ARG;
    if (n_outputs && !outputs) return CAP_ERR_ARG;
    std::lock_guard<std::mutex> lock(s->mu);
    for (uint64_t i = 0; i < n_inputs; ++i) {
        int rc = apply_input_locked(s, inputs[i]);
        if (rc != CAP_OK) return rc;
    }
    int rc = tick_locked(s, result);
    if (rc != CAP_OK) return rc;
    uint64_t total_written = 0;
    for (uint64_t i = 0; i < n_outputs; ++i) {
        rc = fill_output_locked(s, outputs[i]);
        if (rc != CAP_OK) return rc;
        total_written += outputs[i].written;
    }
    if (result) result->written = total_written;
    return CAP_OK;
}

extern "C" int nexus_embedded_snapshot(nexus_embedded_session* s,
                                       const char* name) {
    if (!s || !valid_name(name)) return CAP_ERR_ARG;
    std::lock_guard<std::mutex> lock(s->mu);
    cap_boundary b = boundary(s->model);
    cap_capsule cap = cap_snapshot(s->ctx, &b, CAP_TIER_HOST, 0);
    if (!cap) return s->set_error("snapshot failed", CAP_ERR);
    int rc = sync_all(s);
    if (rc != CAP_OK) {
        cap_capsule_drop(s->ctx, cap);
        return s->set_error("snapshot sync failed", rc);
    }
    auto it = s->capsules.find(name);
    if (it != s->capsules.end()) {
        cap_capsule_drop(s->ctx, it->second.cap);
        s->capsules.erase(it);
    }
    s->capsules[name] = CapsuleRec{cap, true};
    return CAP_OK;
}

extern "C" int nexus_embedded_restore(nexus_embedded_session* s,
                                      const char* name) {
    if (!s || !valid_name(name)) return CAP_ERR_ARG;
    std::lock_guard<std::mutex> lock(s->mu);
    auto it = s->capsules.find(name);
    if (it == s->capsules.end()) return s->set_error("missing capsule", CAP_ERR_ARG);
    int rc = CAP_OK;
    if (it->second.origin_restore) {
        rc = cap_restore(s->ctx, it->second.cap, 0);
    } else {
        cap_boundary b = boundary(s->model);
        rc = cap_restore_into(s->ctx, it->second.cap, b.regions, b.n_regions, 0);
    }
    if (rc != CAP_OK) return s->set_error("restore failed", rc);
    rc = sync_all(s);
    return rc == CAP_OK ? CAP_OK : s->set_error("restore sync failed", rc);
}

extern "C" int nexus_embedded_serialize(nexus_embedded_session* s,
                                        const char* name,
                                        void* out,
                                        size_t* len) {
    if (!s || !valid_name(name) || !len) return CAP_ERR_ARG;
    std::lock_guard<std::mutex> lock(s->mu);
    auto it = s->capsules.find(name);
    if (it == s->capsules.end()) return s->set_error("missing capsule", CAP_ERR_ARG);
    return cap_serialize(s->ctx, it->second.cap, out, len);
}

extern "C" int nexus_embedded_load(nexus_embedded_session* s,
                                   const char* name,
                                   const void* blob,
                                   size_t len) {
    if (!s || !valid_name(name) || !blob || !len) return CAP_ERR_ARG;
    std::lock_guard<std::mutex> lock(s->mu);
    cap_capsule cap = cap_load(s->ctx, blob, len);
    if (!cap) return s->set_error("load failed", CAP_ERR_FORMAT);
    auto it = s->capsules.find(name);
    if (it != s->capsules.end()) {
        cap_capsule_drop(s->ctx, it->second.cap);
        s->capsules.erase(it);
    }
    s->capsules[name] = CapsuleRec{cap, false};
    return CAP_OK;
}

extern "C" uint64_t nexus_embedded_capsule_count(nexus_embedded_session* s) {
    if (!s) return 0;
    std::lock_guard<std::mutex> lock(s->mu);
    return (uint64_t)s->capsules.size();
}
