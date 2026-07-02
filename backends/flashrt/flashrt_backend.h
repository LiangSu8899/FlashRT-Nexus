/* flashrt_backend.h — a Capsule backend (L0) over FlashRT's exec contract.
 *
 * Implements the cap_backend seam against libflashrt_exec + CUDA. FlashRT is
 * consumed UNCHANGED: the adapter uses frt_graph_replay for execution and
 * frt_ctx_wrap_stream to share the frontend's streams, but owns capsule backing
 * memory, host buffers, streams, and events through raw CUDA (the public frt
 * ABI has no per-buffer free, no host buffers, and no non-blocking event query,
 * all of which the core needs). This file lives in the capsule repo, not in
 * FlashRT.
 *
 * Live model state (graphs the frontend captured, buffers it owns) enters the
 * core as wrapped handles via flashrt_wrap_graph / flashrt_wrap_buffer. The
 * adapter never frees those — frt owns them.
 */
#ifndef CAPSULE_FLASHRT_BACKEND_H
#define CAPSULE_FLASHRT_BACKEND_H

#include "capsule/capsule.h"
#include "flashrt/exec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Wire `be` as a capsule backend over an existing (borrowed) FlashRT exec ctx.
 * `fingerprint` = hash{weights,quant,kernel,arch} the frontend computed for this
 * deployment (stamped into every capsule; restore refuses on mismatch).
 * Returns 0 on success, negative on a CUDA error. */
int  flashrt_backend_init(cap_backend* be, frt_ctx ctx, uint64_t fingerprint);
void flashrt_backend_fini(cap_backend* be);

/* Wrap a frontend-owned live frt_graph / frt_buffer as a capsule handle for use
 * in cap_stage / cap_boundary. The adapter does NOT own the underlying frt
 * object; the wrapper itself is released at flashrt_backend_fini. */
cap_graph  flashrt_wrap_graph (cap_backend* be, frt_graph  fg);
cap_buffer flashrt_wrap_buffer(cap_backend* be, frt_buffer fb);

/* Graph-cache passthrough for L2 eviction/budget POLICY (mechanism lives in
 * the exec layer). Discipline: evict only at a safe point — never while the
 * variant may be in flight (sync the streams that replayed it first). */
int      flashrt_graph_evict(cap_backend* be, cap_graph g, cap_shape_key key);
int      flashrt_graph_evict_lru(cap_backend* be, cap_graph g);
uint64_t flashrt_graph_variant_count(cap_backend* be, cap_graph g);

/* Adopt a frontend-owned stream as a capsule stream index (usable in
 * cap_stage.stream / capsule verbs). Bridges the two stream namespaces: the
 * frontend's frt stream id (used for graph_replay) and its raw backend handle
 * (e.g. cudaStream_t, used for copies/events). NOT owned: never destroyed by
 * flashrt_backend_fini. Returns the capsule stream index, or negative. */
int flashrt_adopt_stream(cap_backend* be, int frt_stream_id, void* native_handle);

#ifdef __cplusplus
}
#endif

#endif /* CAPSULE_FLASHRT_BACKEND_H */
