/* stub_backend.cpp — host-memory reference backend (see stub_backend.h). */
#include "stub_backend.h"

#include <cstdlib>
#include <cstring>
#include <cstdint>

namespace {

struct StubCtx { uint64_t fp; };

struct StubBuffer {
    unsigned char* ptr;
    size_t bytes;
    int space;
    bool owned;
};

struct StubGraph {
    void (*fn)(void* user);
    void* user;
};

/* ---- buffers ---- */
cap_buffer s_buffer_alloc(void*, const char*, size_t bytes, int space) {
    StubBuffer* b = new StubBuffer();
    b->ptr = (unsigned char*)std::malloc(bytes ? bytes : 1);
    b->bytes = bytes; b->space = space; b->owned = true;
    return (cap_buffer)b;
}
cap_buffer s_buffer_wrap(void*, const char*, void* ptr, size_t bytes, int space) {
    StubBuffer* b = new StubBuffer();
    b->ptr = (unsigned char*)ptr; b->bytes = bytes; b->space = space; b->owned = false;
    return (cap_buffer)b;
}
void*  s_buffer_ptr(void*, cap_buffer h)   { return ((StubBuffer*)h)->ptr; }
size_t s_buffer_bytes(void*, cap_buffer h) { return ((StubBuffer*)h)->bytes; }
int s_buffer_copy(void*, cap_buffer dst, size_t doff, cap_buffer src, size_t soff, size_t n, int) {
    std::memcpy(((StubBuffer*)dst)->ptr + doff, ((StubBuffer*)src)->ptr + soff, n);
    return 0;
}
int s_buffer_upload(void*, cap_buffer dst, size_t off, const void* src, size_t n, int) {
    std::memcpy(((StubBuffer*)dst)->ptr + off, src, n);
    return 0;
}
int s_buffer_download(void*, cap_buffer src, size_t off, void* dst, size_t n, int) {
    std::memcpy(dst, ((StubBuffer*)src)->ptr + off, n);
    return 0;
}
void s_buffer_free(void*, cap_buffer h) {
    StubBuffer* b = (StubBuffer*)h;
    if (b->owned) std::free(b->ptr);
    delete b;
}

/* ---- graphs ---- */
int s_graph_replay(void*, cap_graph g, cap_shape_key, int) {
    StubGraph* sg = (StubGraph*)g;
    if (sg && sg->fn) sg->fn(sg->user);
    return 0;
}
int s_graph_has(void*, cap_graph, cap_shape_key) { return 1; }
int s_graph_bind(void*, cap_graph, const char*, cap_buffer) { return 0; }

/* ---- streams + events (synchronous host model) ---- */
int       s_stream(void*, int priority) { return priority; }
cap_event s_event(void*) { return (cap_event)new int(0); }
int  s_event_record(void*, cap_event, int) { return 0; }
int  s_event_query(void*, cap_event) { return 0; }   /* always ready */
int  s_stream_wait(void*, int, cap_event) { return 0; }
int  s_sync(void*, int) { return 0; }
void s_event_free(void*, cap_event e) { delete (int*)e; }

uint64_t s_fingerprint(void* self) { return ((StubCtx*)self)->fp; }

} /* namespace */

extern "C" void stub_backend_init(cap_backend* be, uint64_t fingerprint) {
    std::memset(be, 0, sizeof(*be));
    be->abi_version = CAP_ABI_VERSION;
    be->struct_size = (uint32_t)sizeof(cap_backend);
    be->self        = new StubCtx{fingerprint};

    be->buffer_alloc    = s_buffer_alloc;
    be->buffer_wrap     = s_buffer_wrap;
    be->buffer_ptr      = s_buffer_ptr;
    be->buffer_bytes    = s_buffer_bytes;
    be->buffer_copy     = s_buffer_copy;
    be->buffer_upload   = s_buffer_upload;
    be->buffer_download = s_buffer_download;
    be->buffer_free     = s_buffer_free;

    be->graph_replay = s_graph_replay;
    be->graph_has    = s_graph_has;
    be->graph_bind   = s_graph_bind;

    be->stream       = s_stream;
    be->event        = s_event;
    be->event_record = s_event_record;
    be->event_query  = s_event_query;
    be->stream_wait  = s_stream_wait;
    be->sync         = s_sync;
    be->event_free   = s_event_free;

    be->fingerprint  = s_fingerprint;
}

extern "C" void stub_backend_fini(cap_backend* be) {
    delete (StubCtx*)be->self;
    be->self = nullptr;
}

extern "C" cap_graph stub_graph_make(void (*fn)(void* user), void* user) {
    StubGraph* g = new StubGraph();
    g->fn = fn; g->user = user;
    return (cap_graph)g;
}

extern "C" void stub_graph_free(cap_graph g) { delete (StubGraph*)g; }
