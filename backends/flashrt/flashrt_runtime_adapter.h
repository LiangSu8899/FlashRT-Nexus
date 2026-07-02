/* flashrt_runtime_adapter.h — adopt a FlashRT runtime export as a capsule backend.
 *
 * The consumer side of frt_runtime_export_v1 (FlashRT's runtime/ layer): one
 * call turns a producer-packaged model — context, streams, graphs, buffers,
 * restorable regions, identity — into a wired cap_backend plus ready-to-use
 * capsule handles. This is the ONLY entry a host needs; it never sees Python,
 * torch, or model code, and it does not care whether the export came from the
 * Python setup bridge (today) or a native model-runtime .so (later).
 *
 * What adoption does:
 *   - validates abi_version/struct_size, retains the export;
 *   - flashrt_backend_init over the export's frt_ctx + fingerprint (so every
 *     capsule is stamped with the producer-computed identity hash);
 *   - adopts each export stream (frt id + native handle; never destroyed);
 *   - wraps each graph/buffer as capsule handles;
 *   - materializes capsule_regions as a cap_region array ready for
 *     cap_boundary / cap_restore_into (order preserved — it is contractual).
 *
 * Region flags (snapshot-only etc.) are policy: read them from exp->
 * capsule_regions[i].flags and subset the region array as needed.
 */
#ifndef CAPSULE_FLASHRT_RUNTIME_ADAPTER_H
#define CAPSULE_FLASHRT_RUNTIME_ADAPTER_H

#include "flashrt_backend.h"
#include "flashrt/runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct flashrt_runtime_binding {
    const frt_runtime_export_v1* exp;  /* retained until _fini; names/flags live here */
    cap_backend backend;               /* initialized; pass to cap_ctx_create */

    /* Arrays parallel to the export's descriptor arrays. */
    int*        streams;               /* capsule stream index per export stream  */
    cap_graph*  graphs;                /* capsule graph handle per export graph   */
    cap_buffer* buffers;               /* capsule buffer handle per export buffer */
    cap_region* regions;               /* capsule_regions, ready for cap_boundary */
    uint64_t n_streams, n_graphs, n_buffers, n_regions;
} flashrt_runtime_binding;

/* Adopt an export. On success fills `out` (backend initialized, export
 * retained) and returns 0. Negative on failure:
 *   -1 bad args, -2 ABI version/size mismatch, -3 backend init failed,
 *   -4 adoption failed (a stream/graph/buffer could not be wired).
 * On failure `out` is fully cleaned up — nothing to fini. */
int flashrt_adopt_runtime_export(const frt_runtime_export_v1* exp,
                                 flashrt_runtime_binding* out);

/* Release everything adoption created: backend (adopted streams stay alive),
 * arrays, and the export reference. Safe on a zeroed binding. */
void flashrt_runtime_binding_fini(flashrt_runtime_binding*);

/* Name lookups over the export's descriptors (setup-time; linear scan).
 * Return the wired capsule handle, or null / negative when absent. */
cap_graph  flashrt_runtime_graph (const flashrt_runtime_binding*, const char* name);
cap_buffer flashrt_runtime_buffer(const flashrt_runtime_binding*, const char* name);
int        flashrt_runtime_stream(const flashrt_runtime_binding*, const char* name);

#ifdef __cplusplus
}
#endif

#endif /* CAPSULE_FLASHRT_RUNTIME_ADAPTER_H */
