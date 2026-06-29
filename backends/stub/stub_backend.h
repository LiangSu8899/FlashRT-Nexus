/* stub_backend.h — a host-memory reference backend for testing the core.
 *
 * Implements the cap_backend seam entirely on the host (malloc + memcpy), with
 * synchronous "events". It carries NO GPU dependency, so the core can be built
 * and tested anywhere with zero third-party libraries. Graphs hold an optional
 * callback so a test can observe that a replay ran (and mutate a buffer).
 */
#ifndef CAPSULE_STUB_BACKEND_H
#define CAPSULE_STUB_BACKEND_H

#include "capsule/capsule.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fill `be` with a host-memory backend whose fingerprint() returns `fingerprint`.
 * Allocates an internal context; release it with stub_backend_fini(). */
void stub_backend_init(cap_backend* be, uint64_t fingerprint);
void stub_backend_fini(cap_backend* be);

/* Mint a stub graph whose replay invokes fn(user) (fn may be NULL). The returned
 * handle is used as cap_stage.graph. Free with stub_graph_free(). */
cap_graph stub_graph_make(void (*fn)(void* user), void* user);
void      stub_graph_free(cap_graph);

#ifdef __cplusplus
}
#endif

#endif /* CAPSULE_STUB_BACKEND_H */
