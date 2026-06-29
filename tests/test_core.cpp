/* test_core.cpp — P0 acceptance: the core builds and runs with ZERO dependency,
 * over the host stub backend. Exercises every core verb and the fingerprint guard.
 *
 * Returns 0 on success; prints the first failing check and returns non-zero.
 */
#include "capsule/capsule.h"
#include "stub_backend.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::printf("FAIL: %s  (%s:%d)\n", (msg), __FILE__, __LINE__); g_fail = 1; } \
    else { std::printf("ok  : %s\n", (msg)); } \
} while (0)

/* a stub-graph callback: writes a marker byte into a wrapped buffer */
struct FireProbe { unsigned char* target; unsigned char value; int count; };
static void fire_cb(void* user) {
    FireProbe* p = (FireProbe*)user;
    if (p->target) *p->target = p->value;
    p->count++;
}

int main() {
    const uint64_t FP = 0xC0FFEEull;
    cap_backend be;  stub_backend_init(&be, FP);
    cap_ctx c = cap_ctx_create(&be);
    CHECK(c != nullptr, "cap_ctx_create with matching ABI");
    CHECK(cap_ctx_fingerprint(c) == FP, "fingerprint propagates from backend");

    /* version guard: a backend claiming the wrong ABI is rejected */
    {
        cap_backend bad; stub_backend_init(&bad, FP); bad.abi_version = 999;
        cap_ctx cbad = cap_ctx_create(&bad);
        CHECK(cbad == nullptr, "ctx_create rejects wrong abi_version");
        stub_backend_fini(&bad);
    }

    /* --- live state buffers (the "frontend" creates these via the backend) --- */
    const size_t N = 256;
    cap_buffer A = be.buffer_alloc(be.self, "A", N, CAP_DEV);
    cap_buffer B = be.buffer_alloc(be.self, "B", N, CAP_DEV);
    unsigned char known[N];   for (size_t i = 0; i < N; ++i) known[i] = (unsigned char)(i * 7 + 1);
    be.buffer_upload(be.self, A, 0, known, N, 0);

    /* --- snapshot A at a boundary --- */
    cap_region reg = { A, 0, N };
    unsigned char meta[8] = {1,2,3,4,5,6,7,8};
    cap_boundary bnd = { &reg, 1, meta, sizeof(meta) };
    cap_capsule cap = cap_snapshot(c, &bnd, CAP_TIER_HOST, 0);
    CHECK(cap != nullptr, "cap_snapshot");
    CHECK(cap_capsule_ready(c, cap) == CAP_OK, "cap_capsule_ready (stub synchronous)");

    /* --- dirty A, then restore: A must come back bit-exact --- */
    std::memset(known + 0, 0, 0);
    unsigned char dirty[N]; std::memset(dirty, 0xAB, N);
    be.buffer_upload(be.self, A, 0, dirty, N, 0);
    CHECK(cap_restore(c, cap, 0) == CAP_OK, "cap_restore returns OK");
    unsigned char back[N]; be.buffer_download(be.self, A, 0, back, N, 0);
    CHECK(std::memcmp(back, known, N) == 0, "restore is bit-exact into origin");

    /* --- restore_into a DIFFERENT live buffer (the fork/branch mechanism) --- */
    cap_region into = { B, 0, N };
    CHECK(cap_restore_into(c, cap, &into, 1, 0) == CAP_OK, "cap_restore_into (branch)");
    unsigned char bbuf[N]; be.buffer_download(be.self, B, 0, bbuf, N, 0);
    CHECK(std::memcmp(bbuf, known, N) == 0, "restore_into is bit-exact into a new live set");

    /* --- zero-copy region view --- */
    int nv = 0; CHECK(cap_regions(c, cap, nullptr, &nv) == CAP_OK && nv == 1, "cap_regions count");
    cap_region_view view; nv = 1;
    CHECK(cap_regions(c, cap, &view, &nv) == CAP_OK && view.bytes == N && view.ptr != nullptr,
          "cap_regions view (ptr+len for transport)");

    /* --- tier move HOST -> GPU(stub: still host) -> HOST, data preserved --- */
    CHECK(cap_tier_move(c, cap, CAP_TIER_GPU, 0) == CAP_OK, "cap_tier_move to GPU tier");
    CHECK(cap_restore_into(c, cap, &into, 1, 0) == CAP_OK, "restore_into after tier_move");
    be.buffer_download(be.self, B, 0, bbuf, N, 0);
    CHECK(std::memcmp(bbuf, known, N) == 0, "data preserved across tier_move");
    cap_tier_move(c, cap, CAP_TIER_HOST, 0);

    /* --- serialize -> load (same fingerprint) -> restore_into matches --- */
    size_t blen = 0; CHECK(cap_serialize(c, cap, nullptr, &blen) == CAP_OK && blen > N, "serialize size query");
    std::vector<unsigned char> blob(blen);
    CHECK(cap_serialize(c, cap, blob.data(), &blen) == CAP_OK, "serialize into blob");
    cap_capsule loaded = cap_load(c, blob.data(), blob.size());
    CHECK(loaded != nullptr, "cap_load (matching fingerprint)");
    cap_buffer Cb = be.buffer_alloc(be.self, "C", N, CAP_DEV);
    cap_region cinto = { Cb, 0, N };
    CHECK(cap_restore_into(c, loaded, &cinto, 1, 0) == CAP_OK, "restore_into from a loaded capsule");
    unsigned char cbuf[N]; be.buffer_download(be.self, Cb, 0, cbuf, N, 0);
    CHECK(std::memcmp(cbuf, known, N) == 0, "loaded capsule restores bit-exact");

    /* --- fingerprint guard: a capsule from a DIFFERENT deployment is refused --- */
    {
        cap_backend be2; stub_backend_init(&be2, 0xDEADBEEFull);  /* different fingerprint */
        cap_ctx c2 = cap_ctx_create(&be2);
        cap_capsule alien = cap_load(c2, blob.data(), blob.size());
        CHECK(alien == nullptr, "cap_load refuses a foreign-fingerprint blob");
        /* also: restoring our capsule under c2 must refuse */
        CHECK(cap_restore_into(c2, cap, &into, 1, 0) == CAP_ERR_FINGERPRINT,
              "cap_restore_into refuses fingerprint mismatch");
        cap_ctx_destroy(c2); stub_backend_fini(&be2);
    }

    /* --- Drive: cap_fire runs a replay --- */
    unsigned char fire_target = 0;
    FireProbe probe = { &fire_target, 0x5A, 0 };
    cap_graph g = stub_graph_make(fire_cb, &probe);
    cap_stage st = { g, /*key*/42, /*stream*/0, /*prio*/0, /*cad*/1, 1, CAP_EVERY };
    CHECK(cap_fire(c, &st) == CAP_OK && fire_target == 0x5A && probe.count == 1, "cap_fire runs the replay");

    /* --- Drive: tick fires due stages and honors cadence --- */
    cap_graph g2 = stub_graph_make(fire_cb, &probe);
    cap_stage stages[2] = {
        { g,  1, 0, 0, 1, 1, CAP_EVERY },   /* every tick   */
        { g2, 2, 0, 0, 1, 3, CAP_EVERY },   /* 1 per 3 ticks */
    };
    int deps[1][2] = { {1, 0} };            /* stage 1 waits on stage 0 */
    cap_schedule sched = { stages, 2, deps, 1 };
    probe.count = 0;
    int failed = -99;
    CHECK(cap_drive_tick(c, &sched, /*clock*/0, &failed) == CAP_OK && failed == -1, "drive_tick ok");
    /* clock 0: stage0 due (every), stage1 due (0%3<1) -> 2 fires */
    CHECK(probe.count == 2, "drive_tick clock=0 fired both stages");
    probe.count = 0;
    cap_drive_tick(c, &sched, /*clock*/1, &failed);
    /* clock 1: stage0 due, stage1 NOT due (1%3=1, not <1) -> 1 fire */
    CHECK(probe.count == 1, "drive_tick clock=1 respected cadence (only stage0)");

    /* --- Drive: cap_swap overwrites buffer content --- */
    unsigned char fill[N]; std::memset(fill, 0x33, N);
    CHECK(cap_swap(c, A, fill, N, 0) == CAP_OK, "cap_swap returns OK");
    be.buffer_download(be.self, A, 0, back, N, 0);
    CHECK(std::memcmp(back, fill, N) == 0, "cap_swap overwrote content");
    CHECK(cap_sync(c, 0) == CAP_OK, "cap_sync");

    /* --- teardown --- */
    stub_graph_free(g); stub_graph_free(g2);
    be.buffer_free(be.self, A); be.buffer_free(be.self, B); be.buffer_free(be.self, Cb);
    cap_capsule_drop(c, cap); cap_capsule_drop(c, loaded);
    cap_ctx_destroy(c); stub_backend_fini(&be);

    std::printf(g_fail ? "\n== CORE TEST FAILED ==\n" : "\n== CORE TEST PASSED ==\n");
    return g_fail;
}
