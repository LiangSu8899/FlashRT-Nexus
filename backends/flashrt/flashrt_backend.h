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

#ifdef __cplusplus
}
#endif

#endif /* CAPSULE_FLASHRT_BACKEND_H */
