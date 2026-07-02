/* flashrt_runtime_adapter.cpp — consumer of frt_runtime_export_v1 (see header). */
#include "flashrt_runtime_adapter.h"

#include <cstring>
#include <new>

namespace {

void free_arrays(flashrt_runtime_binding* b) {
    delete[] b->streams; delete[] b->graphs;
    delete[] b->buffers; delete[] b->regions;
    b->streams = nullptr; b->graphs = nullptr;
    b->buffers = nullptr; b->regions = nullptr;
}

}  // namespace

extern "C" int flashrt_adopt_runtime_export(const frt_runtime_export_v1* exp,
                                            flashrt_runtime_binding* out) {
    if (!exp || !out) return -1;
    if (exp->abi_version != FRT_RUNTIME_ABI_VERSION ||
        exp->struct_size < sizeof(frt_runtime_export_v1)) return -2;
    if (!exp->ctx || !exp->retain || !exp->release) return -1;
    if ((exp->n_streams && !exp->streams) || (exp->n_graphs && !exp->graphs) ||
        (exp->n_buffers && !exp->buffers) ||
        (exp->n_capsule_regions && !exp->capsule_regions)) return -1;

    std::memset(out, 0, sizeof(*out));
    if (flashrt_backend_init(&out->backend, exp->ctx, exp->fingerprint) != 0)
        return -3;
    exp->retain(exp->owner);
    out->exp = exp;

    out->n_streams = exp->n_streams;   out->n_graphs = exp->n_graphs;
    out->n_buffers = exp->n_buffers;   out->n_regions = exp->n_capsule_regions;
    out->streams = new (std::nothrow) int[exp->n_streams ? exp->n_streams : 1];
    out->graphs  = new (std::nothrow) cap_graph[exp->n_graphs ? exp->n_graphs : 1];
    out->buffers = new (std::nothrow) cap_buffer[exp->n_buffers ? exp->n_buffers : 1];
    out->regions = new (std::nothrow) cap_region[exp->n_capsule_regions ? exp->n_capsule_regions : 1];
    if (!out->streams || !out->graphs || !out->buffers || !out->regions) {
        flashrt_runtime_binding_fini(out);
        return -4;
    }

    for (uint64_t i = 0; i < exp->n_streams; ++i) {
        const frt_runtime_stream_desc& d = exp->streams[i];
        int idx = flashrt_adopt_stream(&out->backend, d.stream_id, d.native_handle);
        if (idx < 0) { flashrt_runtime_binding_fini(out); return -4; }
        out->streams[i] = idx;
    }
    for (uint64_t i = 0; i < exp->n_graphs; ++i) {
        cap_graph g = flashrt_wrap_graph(&out->backend, exp->graphs[i].handle);
        if (!g) { flashrt_runtime_binding_fini(out); return -4; }
        out->graphs[i] = g;
    }
    for (uint64_t i = 0; i < exp->n_buffers; ++i) {
        cap_buffer b = flashrt_wrap_buffer(&out->backend, exp->buffers[i].handle);
        if (!b) { flashrt_runtime_binding_fini(out); return -4; }
        out->buffers[i] = b;
    }
    for (uint64_t i = 0; i < exp->n_capsule_regions; ++i) {
        const frt_runtime_region_desc& r = exp->capsule_regions[i];
        cap_buffer b = flashrt_wrap_buffer(&out->backend, r.buffer);
        if (!b) { flashrt_runtime_binding_fini(out); return -4; }
        out->regions[i].buf = b;
        out->regions[i].off = (size_t)r.offset;
        out->regions[i].bytes = (size_t)r.bytes;
    }
    return 0;
}

extern "C" void flashrt_runtime_binding_fini(flashrt_runtime_binding* b) {
    if (!b) return;
    free_arrays(b);
    if (b->backend.self) flashrt_backend_fini(&b->backend);
    if (b->exp) { b->exp->release(b->exp->owner); b->exp = nullptr; }
    b->n_streams = b->n_graphs = b->n_buffers = b->n_regions = 0;
}

extern "C" cap_graph flashrt_runtime_graph(const flashrt_runtime_binding* b,
                                           const char* name) {
    if (!b || !b->exp || !name) return nullptr;
    for (uint64_t i = 0; i < b->n_graphs; ++i)
        if (std::strcmp(b->exp->graphs[i].name, name) == 0) return b->graphs[i];
    return nullptr;
}

extern "C" cap_buffer flashrt_runtime_buffer(const flashrt_runtime_binding* b,
                                             const char* name) {
    if (!b || !b->exp || !name) return nullptr;
    for (uint64_t i = 0; i < b->n_buffers; ++i)
        if (std::strcmp(b->exp->buffers[i].name, name) == 0) return b->buffers[i];
    return nullptr;
}

extern "C" int flashrt_runtime_stream(const flashrt_runtime_binding* b,
                                      const char* name) {
    if (!b || !b->exp || !name) return -1;
    for (uint64_t i = 0; i < b->n_streams; ++i)
        if (std::strcmp(b->exp->streams[i].name, name) == 0) return b->streams[i];
    return -1;
}
