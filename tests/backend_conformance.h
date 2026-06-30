/* backend_conformance.h — the contract any capsule backend must pass.
 *
 * Backend-agnostic: exercises the buffer / event / capsule / blob contract of the
 * cap_backend seam (not graph replay, which is backend-specific and covered by each
 * backend's smoke test). Run it on the stub (CI, zero-dep) and on real backends
 * (GPU). A backend that passes this is safe to plug into Nexus.
 *
 * Usage: wire two backends — `be` (primary) and `be_foreign` (a DIFFERENT
 * fingerprint) — then call run_backend_conformance(be, be_foreign). Returns 0 on
 * success; prints the first failing check and returns non-zero.
 */
#ifndef CAPSULE_BACKEND_CONFORMANCE_H
#define CAPSULE_BACKEND_CONFORMANCE_H

#include "capsule/capsule.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>

#define CONF_CHECK(cond, msg) do { \
    if (!(cond)) { std::printf("FAIL[conformance]: %s\n", (msg)); return 1; } \
    else { std::printf("ok  [conformance]: %s\n", (msg)); } \
} while (0)

static inline int run_backend_conformance(cap_backend* be, cap_backend* be_foreign) {
    const size_t N = 128;
    unsigned char known[N], known2[N], tmp[N];
    for (size_t i = 0; i < N; ++i) { known[i] = (unsigned char)(i*7+1); known2[i] = (unsigned char)(i*5+9); }

    int s = be->stream(be->self, 0);

    /* --- 1. buffer copy in all four directions (H2D, D2H, D2D, H2H) --- */
    cap_buffer d1 = be->buffer_alloc(be->self, "d1", N, CAP_DEV);
    cap_buffer d2 = be->buffer_alloc(be->self, "d2", N, CAP_DEV);
    cap_buffer h1 = be->buffer_alloc(be->self, "h1", N, CAP_HOST);
    CONF_CHECK(d1 && d2 && h1, "buffer_alloc dev/host");
    CONF_CHECK(be->buffer_upload(be->self, d1, 0, known, N, s) == 0, "buffer_upload H2D");
    CONF_CHECK(be->buffer_copy(be->self, d2, 0, d1, 0, N, s) == 0, "buffer_copy D2D");
    CONF_CHECK(be->buffer_copy(be->self, h1, 0, d2, 0, N, s) == 0, "buffer_copy D2H");
    be->sync(be->self, s);
    CONF_CHECK(std::memcmp(be->buffer_ptr(be->self, h1), known, N) == 0, "H2D->D2D->D2H round-trip exact");
    be->buffer_upload(be->self, h1, 0, known2, N, s);              /* H2H into host buffer */
    be->buffer_copy(be->self, d1, 0, h1, 0, N, s);                /* H2D */
    be->buffer_download(be->self, d1, 0, tmp, N, s);              /* D2H */
    be->sync(be->self, s);
    CONF_CHECK(std::memcmp(tmp, known2, N) == 0, "H2H->H2D->D2H round-trip exact");
    be->buffer_free(be->self, d1); be->buffer_free(be->self, d2); be->buffer_free(be->self, h1);

    /* --- 2. event_query is NON-BLOCKING and resolves after sync --- */
    cap_event ev = be->event(be->self);
    be->event_record(be->self, ev, s);
    int q = be->event_query(be->self, ev);
    CONF_CHECK(q == 0 || q == 1, "event_query non-blocking (ready or pending, no crash)");
    be->sync(be->self, s);
    CONF_CHECK(be->event_query(be->self, ev) == 0, "event_query == ready after sync");
    be->event_free(be->self, ev);

    /* --- 3. snapshot / restore / restore_into through the core --- */
    cap_ctx c = cap_ctx_create(be);
    CONF_CHECK(c != nullptr, "cap_ctx_create accepts the backend");
    CONF_CHECK(cap_ctx_fingerprint(c) == be->fingerprint(be->self), "fingerprint propagates");
    cap_buffer L = be->buffer_alloc(be->self, "L", N, CAP_DEV);
    cap_buffer B = be->buffer_alloc(be->self, "B", N, CAP_DEV);
    be->buffer_upload(be->self, L, 0, known, N, s); be->sync(be->self, s);
    cap_region reg = { L, 0, N };
    cap_boundary bnd = { &reg, 1, nullptr, 0 };
    cap_capsule cap = cap_snapshot(c, &bnd, CAP_TIER_HOST, s);
    CONF_CHECK(cap != nullptr, "cap_snapshot");
    be->sync(be->self, s);
    be->buffer_upload(be->self, L, 0, known2, N, s); be->sync(be->self, s);   /* dirty */
    CONF_CHECK(cap_restore(c, cap, s) == CAP_OK, "cap_restore");
    be->sync(be->self, s); be->buffer_download(be->self, L, 0, tmp, N, s); be->sync(be->self, s);
    CONF_CHECK(std::memcmp(tmp, known, N) == 0, "restore bit-exact into origin");
    cap_region into = { B, 0, N };
    CONF_CHECK(cap_restore_into(c, cap, &into, 1, s) == CAP_OK, "cap_restore_into");
    be->sync(be->self, s); be->buffer_download(be->self, B, 0, tmp, N, s); be->sync(be->self, s);
    CONF_CHECK(std::memcmp(tmp, known, N) == 0, "restore_into bit-exact into a new live set");

    /* --- 4. serialize -> load -> restore_into (matching fingerprint) --- */
    size_t blen = 0;
    CONF_CHECK(cap_serialize(c, cap, nullptr, &blen) == CAP_OK && blen > N, "serialize size query");
    std::vector<unsigned char> blob(blen);
    CONF_CHECK(cap_serialize(c, cap, blob.data(), &blen) == CAP_OK, "serialize");
    cap_capsule loaded = cap_load(c, blob.data(), blob.size());
    CONF_CHECK(loaded != nullptr, "cap_load (matching fingerprint)");
    CONF_CHECK(cap_restore_into(c, loaded, &into, 1, s) == CAP_OK, "restore_into from loaded capsule");
    be->sync(be->self, s); be->buffer_download(be->self, B, 0, tmp, N, s); be->sync(be->self, s);
    CONF_CHECK(std::memcmp(tmp, known, N) == 0, "loaded capsule restores bit-exact");

    /* --- 5. cap_load rejects malformed / hostile blobs --- */
    CONF_CHECK(cap_load(c, blob.data(), blob.size() - 1) == nullptr, "reject truncated blob");
    { auto b = blob; b[0] ^= 0xFF; CONF_CHECK(cap_load(c, b.data(), b.size()) == nullptr, "reject wrong magic"); }
    { auto b = blob; b[4] ^= 0xFF; CONF_CHECK(cap_load(c, b.data(), b.size()) == nullptr, "reject wrong version"); }
    { auto b = blob; uint32_t huge = 0xFFFFFFFFu; std::memcpy(&b[20], &huge, 4);   /* n_regions field */
      CONF_CHECK(cap_load(c, b.data(), b.size()) == nullptr, "reject absurd n_regions"); }
    { auto b = blob; b.push_back(0x00); CONF_CHECK(cap_load(c, b.data(), b.size()) == nullptr, "reject trailing bytes"); }

    /* --- 6. fingerprint guard under a FOREIGN backend --- */
    cap_ctx cf = cap_ctx_create(be_foreign);
    CONF_CHECK(cf != nullptr, "foreign ctx_create");
    CONF_CHECK(cap_load(cf, blob.data(), blob.size()) == nullptr, "foreign fingerprint: cap_load refuses");
    CONF_CHECK(cap_restore_into(cf, cap, &into, 1, s) == CAP_ERR_FINGERPRINT, "foreign fingerprint: restore refuses");

    cap_capsule_drop(c, cap); cap_capsule_drop(c, loaded);
    be->buffer_free(be->self, L); be->buffer_free(be->self, B);
    cap_ctx_destroy(cf); cap_ctx_destroy(c);
    std::printf("== BACKEND CONFORMANCE PASSED ==\n");
    return 0;
}

#undef CONF_CHECK
#endif /* CAPSULE_BACKEND_CONFORMANCE_H */
