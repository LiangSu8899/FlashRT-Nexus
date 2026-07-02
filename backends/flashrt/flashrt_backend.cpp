/* flashrt_backend.cpp — Capsule backend over FlashRT exec + CUDA (see header). */
#include "flashrt_backend.h"

#include <cuda_runtime.h>
#include <vector>
#include <cstring>
#include <cstdint>

namespace {

/* A capsule buffer handle. Three flavours:
 *   FRT_DEV     : a frontend-owned frt_buffer (live model state); not freed here.
 *   CUDA_DEV    : adapter-owned device memory (capsule backing on the GPU tier).
 *   HOST_PINNED : adapter-owned pinned host memory (HOST/DISK tier, transport).  */
struct CapBuf {
    enum Kind { FRT_DEV, CUDA_DEV, HOST_PINNED } kind;
    frt_buffer fb;     /* FRT_DEV only */
    void*  ptr;        /* CUDA_DEV (device) / HOST_PINNED (host) */
    size_t bytes;
    int    space;      /* CAP_DEV | CAP_HOST */
    bool   owned;
};

struct CapGraph { frt_graph fg; };

struct Adapter {
    frt_ctx  ctx;
    uint64_t fingerprint;
    std::vector<cudaStream_t> streams;   /* index = capsule stream id */
    std::vector<int>          frt_ids;   /* parallel: frt stream id for replay */
    std::vector<bool>         owned;     /* parallel: destroy at fini? (adopted = no) */
    int prio_lo = 0, prio_hi = 0;
    std::vector<CapBuf*>   wrapped_bufs;
    std::vector<CapGraph*> wrapped_graphs;
};

inline cudaStream_t cu_stream(Adapter* a, int id) {
    if (id < 0 || id >= (int)a->streams.size()) return a->streams.empty() ? (cudaStream_t)0 : a->streams[0];
    return a->streams[id];
}
inline void* buf_ptr(CapBuf* b) {
    return (b->kind == CapBuf::FRT_DEV) ? frt_buffer_dptr(b->fb) : b->ptr;
}
inline cudaMemcpyKind kind_of(int src_space, int dst_space) {
    if (src_space == CAP_DEV && dst_space == CAP_DEV)  return cudaMemcpyDeviceToDevice;
    if (src_space == CAP_DEV && dst_space == CAP_HOST) return cudaMemcpyDeviceToHost;
    if (src_space == CAP_HOST && dst_space == CAP_DEV) return cudaMemcpyHostToDevice;
    return cudaMemcpyHostToHost;
}

/* ---- buffers ---- */
cap_buffer f_buffer_alloc(void* self, const char*, size_t bytes, int space) {
    (void)self;
    CapBuf* b = new CapBuf();
    b->bytes = bytes; b->space = space; b->owned = true; b->fb = nullptr;
    size_t n = bytes ? bytes : 1;
    if (space == CAP_DEV) {
        b->kind = CapBuf::CUDA_DEV;
        if (cudaMalloc(&b->ptr, n) != cudaSuccess) { delete b; return nullptr; }
    } else {
        b->kind = CapBuf::HOST_PINNED;
        if (cudaMallocHost(&b->ptr, n) != cudaSuccess) { delete b; return nullptr; }
    }
    return (cap_buffer)b;
}
cap_buffer f_buffer_wrap(void* self, const char*, void* ptr, size_t bytes, int space) {
    (void)self;
    CapBuf* b = new CapBuf();
    b->kind = (space == CAP_DEV) ? CapBuf::CUDA_DEV : CapBuf::HOST_PINNED;
    b->ptr = ptr; b->bytes = bytes; b->space = space; b->owned = false; b->fb = nullptr;
    return (cap_buffer)b;
}
void*  f_buffer_ptr  (void*, cap_buffer h) { return buf_ptr((CapBuf*)h); }
size_t f_buffer_bytes(void*, cap_buffer h) { return ((CapBuf*)h)->bytes; }

int f_buffer_copy(void* self, cap_buffer dst, size_t doff, cap_buffer src, size_t soff, size_t n, int stream) {
    Adapter* a = (Adapter*)self;
    CapBuf* d = (CapBuf*)dst; CapBuf* s = (CapBuf*)src;
    char* dp = (char*)buf_ptr(d) + doff;
    char* sp = (char*)buf_ptr(s) + soff;
    cudaError_t e = cudaMemcpyAsync(dp, sp, n, kind_of(s->space, d->space), cu_stream(a, stream));
    return e == cudaSuccess ? 0 : -1;
}
int f_buffer_upload(void* self, cap_buffer dst, size_t off, const void* src, size_t n, int stream) {
    Adapter* a = (Adapter*)self; CapBuf* d = (CapBuf*)dst;
    cudaMemcpyKind k = (d->space == CAP_DEV) ? cudaMemcpyHostToDevice : cudaMemcpyHostToHost;
    cudaError_t e = cudaMemcpyAsync((char*)buf_ptr(d) + off, src, n, k, cu_stream(a, stream));
    return e == cudaSuccess ? 0 : -1;
}
int f_buffer_download(void* self, cap_buffer src, size_t off, void* dst, size_t n, int stream) {
    Adapter* a = (Adapter*)self; CapBuf* s = (CapBuf*)src;
    cudaMemcpyKind k = (s->space == CAP_DEV) ? cudaMemcpyDeviceToHost : cudaMemcpyHostToHost;
    cudaError_t e = cudaMemcpyAsync(dst, (char*)buf_ptr(s) + off, n, k, cu_stream(a, stream));
    return e == cudaSuccess ? 0 : -1;
}
void f_buffer_free(void*, cap_buffer h) {
    CapBuf* b = (CapBuf*)h;
    if (b->owned) {
        if (b->kind == CapBuf::CUDA_DEV)    cudaFree(b->ptr);
        else if (b->kind == CapBuf::HOST_PINNED) cudaFreeHost(b->ptr);
    }
    delete b;
}

/* ---- graphs (execution delegated to frt; the frontend captured them) ---- */
int f_graph_replay(void* self, cap_graph g, cap_shape_key key, int stream) {
    Adapter* a = (Adapter*)self;
    int frt_id = (stream >= 0 && stream < (int)a->frt_ids.size()) ? a->frt_ids[stream] : a->frt_ids[0];
    return frt_graph_replay(((CapGraph*)g)->fg, (frt_shape_key)key, frt_id) == FRT_OK ? 0 : -1;
}
int f_graph_has(void*, cap_graph g, cap_shape_key key) {
    return frt_graph_has_variant(((CapGraph*)g)->fg, (frt_shape_key)key);
}
int f_graph_bind(void*, cap_graph g, const char* port, cap_buffer buf) {
    CapBuf* b = (CapBuf*)buf;
    if (b->kind != CapBuf::FRT_DEV) return -1;   /* only live frt buffers bind to graph ports */
    return frt_graph_bind(((CapGraph*)g)->fg, port, b->fb) == FRT_OK ? 0 : -1;
}

/* ---- streams + events (raw CUDA; gives non-blocking query the frt ABI lacks) ---- */
int f_stream(void* self, int priority) {
    Adapter* a = (Adapter*)self;
    if (priority == 0) return 0;                 /* index 0 = the default working stream */
    int p = priority; if (p < a->prio_hi) p = a->prio_hi; if (p > a->prio_lo) p = a->prio_lo;
    cudaStream_t s;
    if (cudaStreamCreateWithPriority(&s, cudaStreamNonBlocking, p) != cudaSuccess) return -1;
    int frt_id = frt_ctx_wrap_stream(a->ctx, (void*)s);
    if (frt_id < 0) { cudaStreamDestroy(s); return -1; }
    a->streams.push_back(s); a->frt_ids.push_back(frt_id); a->owned.push_back(true);
    return (int)a->streams.size() - 1;
}
cap_event f_event(void*) {
    cudaEvent_t e;
    if (cudaEventCreateWithFlags(&e, cudaEventDisableTiming) != cudaSuccess) return nullptr;
    return (cap_event)e;
}
int f_event_record(void* self, cap_event ev, int stream) {
    Adapter* a = (Adapter*)self;
    return cudaEventRecord((cudaEvent_t)ev, cu_stream(a, stream)) == cudaSuccess ? 0 : -1;
}
int f_event_query(void*, cap_event ev) {
    cudaError_t e = cudaEventQuery((cudaEvent_t)ev);
    if (e == cudaSuccess)        return 0;   /* ready   */
    if (e == cudaErrorNotReady)  return 1;   /* pending */
    return -1;                                /* error   */
}
int f_stream_wait(void* self, int stream, cap_event ev) {
    Adapter* a = (Adapter*)self;
    return cudaStreamWaitEvent(cu_stream(a, stream), (cudaEvent_t)ev, 0) == cudaSuccess ? 0 : -1;
}
int f_sync(void* self, int stream) {
    Adapter* a = (Adapter*)self;
    return cudaStreamSynchronize(cu_stream(a, stream)) == cudaSuccess ? 0 : -1;
}
void f_event_free(void*, cap_event ev) { cudaEventDestroy((cudaEvent_t)ev); }

uint64_t f_fingerprint(void* self) { return ((Adapter*)self)->fingerprint; }

} /* namespace */

extern "C" int flashrt_backend_init(cap_backend* be, frt_ctx ctx, uint64_t fingerprint) {
    std::memset(be, 0, sizeof(*be));
    Adapter* a = new Adapter();
    a->ctx = ctx; a->fingerprint = fingerprint;
    cudaDeviceGetStreamPriorityRange(&a->prio_lo, &a->prio_hi);
    cudaStream_t s0;
    if (cudaStreamCreateWithPriority(&s0, cudaStreamNonBlocking, 0) != cudaSuccess) { delete a; return -1; }
    int frt0 = frt_ctx_wrap_stream(ctx, (void*)s0);
    if (frt0 < 0) { cudaStreamDestroy(s0); delete a; return -1; }
    a->streams.push_back(s0); a->frt_ids.push_back(frt0); a->owned.push_back(true);

    be->abi_version = CAP_ABI_VERSION;
    be->struct_size = (uint32_t)sizeof(cap_backend);
    be->self        = a;
    be->buffer_alloc = f_buffer_alloc; be->buffer_wrap = f_buffer_wrap;
    be->buffer_ptr = f_buffer_ptr;     be->buffer_bytes = f_buffer_bytes;
    be->buffer_copy = f_buffer_copy;   be->buffer_upload = f_buffer_upload;
    be->buffer_download = f_buffer_download; be->buffer_free = f_buffer_free;
    be->graph_replay = f_graph_replay; be->graph_has = f_graph_has; be->graph_bind = f_graph_bind;
    be->stream = f_stream;             be->event = f_event;
    be->event_record = f_event_record; be->event_query = f_event_query;
    be->stream_wait = f_stream_wait;   be->sync = f_sync; be->event_free = f_event_free;
    be->fingerprint = f_fingerprint;
    return 0;
}

extern "C" void flashrt_backend_fini(cap_backend* be) {
    Adapter* a = (Adapter*)be->self;
    if (!a) return;
    for (CapGraph* g : a->wrapped_graphs) delete g;
    for (CapBuf*   b : a->wrapped_bufs)   delete b;   /* wrappers only; frt owns the buffers */
    for (size_t i = 0; i < a->streams.size(); ++i)
        if (a->owned[i]) cudaStreamDestroy(a->streams[i]);   /* adopted streams stay alive */
    delete a;
    be->self = nullptr;
}

extern "C" cap_graph flashrt_wrap_graph(cap_backend* be, frt_graph fg) {
    Adapter* a = (Adapter*)be->self;
    CapGraph* g = new CapGraph{fg};
    a->wrapped_graphs.push_back(g);
    return (cap_graph)g;
}

extern "C" cap_buffer flashrt_wrap_buffer(cap_backend* be, frt_buffer fb) {
    Adapter* a = (Adapter*)be->self;
    CapBuf* b = new CapBuf();
    b->kind = CapBuf::FRT_DEV; b->fb = fb; b->ptr = nullptr;
    b->bytes = frt_buffer_bytes(fb); b->space = CAP_DEV; b->owned = false;
    a->wrapped_bufs.push_back(b);
    return (cap_buffer)b;
}

extern "C" int flashrt_graph_evict(cap_backend* be, cap_graph g, cap_shape_key key) {
    if (!be || !be->self || !g) return -1;
    return frt_graph_evict(((CapGraph*)g)->fg, (frt_shape_key)key) == FRT_OK ? 0 : -1;
}

extern "C" int flashrt_graph_evict_lru(cap_backend* be, cap_graph g) {
    if (!be || !be->self || !g) return -1;
    return frt_graph_evict_lru(((CapGraph*)g)->fg) == FRT_OK ? 0 : -1;
}

extern "C" uint64_t flashrt_graph_variant_count(cap_backend* be, cap_graph g) {
    if (!be || !be->self || !g) return 0;
    return (uint64_t)frt_graph_variant_count(((CapGraph*)g)->fg);
}

extern "C" int flashrt_adopt_stream(cap_backend* be, int frt_stream_id, void* native_handle) {
    Adapter* a = (Adapter*)be->self;
    if (!a || frt_stream_id < 0) return -1;
    /* native_handle may be 0: the legacy default stream is a valid handle. */
    a->streams.push_back((cudaStream_t)native_handle);
    a->frt_ids.push_back(frt_stream_id);
    a->owned.push_back(false);
    return (int)a->streams.size() - 1;
}
