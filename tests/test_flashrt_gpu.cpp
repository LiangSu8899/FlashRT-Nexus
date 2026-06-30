/* test_flashrt_gpu.cpp — GPU smoke for the FlashRT backend (no model needed).
 *
 * Captures a trivial graph (device copy S->L) via the frt exec contract, then
 * drives it and a capsule snapshot/restore THROUGH THE CORE over the FlashRT
 * backend. Proves the seam wiring on a real GPU without any model/checkpoint.
 *
 * Requires a CUDA GPU + a built libflashrt_exec; run in-container. The full
 * model E2E (qwen36 / pi05, token/cos exact) wires a real frontend's captured
 * graph + buffers the same way and is the P1 acceptance gate.
 */
#include "capsule/capsule.h"
#include "flashrt_backend.h"
#include "flashrt/exec.h"
#include "backend_conformance.h"

#include <cuda_runtime.h>
#include <cstdio>
#include <cstring>
#include <cstdint>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::printf("FAIL: %s\n", (msg)); g_fail = 1; } \
    else { std::printf("ok  : %s\n", (msg)); } \
} while (0)

/* record callback for graph capture: enqueue a device-to-device copy S->L */
struct CopyRec { void* dst; const void* src; size_t n; };
static void record_copy(void* user, void* stream) {
    CopyRec* r = (CopyRec*)user;
    cudaMemcpyAsync(r->dst, r->src, r->n, cudaMemcpyDeviceToDevice, (cudaStream_t)stream);
}

int main() {
    const size_t N = 1024;
    const uint64_t FP = 0xBEEF1234ull;

    frt_ctx ctx = frt_ctx_create();
    if (!ctx) { std::printf("FAIL: frt_ctx_create (no GPU?)\n"); return 1; }

    /* live model state: S (source, preset) and L (the graph's output) */
    frt_buffer S = frt_buffer_alloc(ctx, "S", N);
    frt_buffer L = frt_buffer_alloc(ctx, "L", N);
    unsigned char known[N]; for (size_t i = 0; i < N; ++i) known[i] = (unsigned char)(i * 3 + 5);
    cudaMemcpy(frt_buffer_dptr(S), known, N, cudaMemcpyHostToDevice);
    cudaMemset(frt_buffer_dptr(L), 0, N);

    /* capture a graph: L <- S, under key 1 */
    frt_graph G = frt_graph_create(ctx, "copy", 8);
    CopyRec rec{ frt_buffer_dptr(L), frt_buffer_dptr(S), N };
    CHECK(frt_graph_capture(G, /*key*/1, record_copy, &rec) == FRT_OK, "frt_graph_capture");

    /* wire the FlashRT backend + the core */
    cap_backend be;
    CHECK(flashrt_backend_init(&be, ctx, FP) == 0, "flashrt_backend_init");
    cap_ctx c = cap_ctx_create(&be);
    CHECK(c != nullptr, "cap_ctx_create over flashrt backend");
    CHECK(cap_ctx_fingerprint(c) == FP, "fingerprint via backend");

    cap_graph  cg = flashrt_wrap_graph(&be, G);
    cap_buffer cL = flashrt_wrap_buffer(&be, L);

    /* Drive: fire the captured graph through the core; L must become `known` */
    cap_stage st = { cg, /*key*/1, /*stream*/0, 0, 1, 1, CAP_EVERY };
    CHECK(cap_fire(c, &st) == CAP_OK, "cap_fire (frt graph_replay)");
    cap_sync(c, 0);
    unsigned char got[N]; cudaMemcpy(got, frt_buffer_dptr(L), N, cudaMemcpyDeviceToHost);
    CHECK(std::memcmp(got, known, N) == 0, "replayed graph produced expected output");

    /* Capsule: snapshot L (to HOST tier), dirty it, restore, verify bit-exact */
    cap_region reg = { cL, 0, N };
    cap_boundary bnd = { &reg, 1, nullptr, 0 };
    cap_capsule cap = cap_snapshot(c, &bnd, CAP_TIER_HOST, 0);
    CHECK(cap != nullptr, "cap_snapshot (D2H to host backing)");
    cap_sync(c, 0);
    cudaMemset(frt_buffer_dptr(L), 0xCD, N);                 /* dirty the live buffer */
    CHECK(cap_restore(c, cap, 0) == CAP_OK, "cap_restore");
    cap_sync(c, 0);
    cudaMemcpy(got, frt_buffer_dptr(L), N, cudaMemcpyDeviceToHost);
    CHECK(std::memcmp(got, known, N) == 0, "restore is bit-exact (H2D from host backing)");

    /* GPU-tier capsule + restore_into a different live buffer */
    cap_capsule capg = cap_snapshot(c, &bnd, CAP_TIER_GPU, 0);
    cap_region into = { flashrt_wrap_buffer(&be, S), 0, N };  /* reuse S as a branch target */
    cudaMemset(frt_buffer_dptr(S), 0, N);
    CHECK(cap_restore_into(c, capg, &into, 1, 0) == CAP_OK, "cap_restore_into (GPU tier, D2D)");
    cap_sync(c, 0);
    cudaMemcpy(got, frt_buffer_dptr(S), N, cudaMemcpyDeviceToHost);
    CHECK(std::memcmp(got, known, N) == 0, "restore_into bit-exact on GPU tier");

    /* fingerprint guard under a foreign backend */
    {
        cap_backend be2; flashrt_backend_init(&be2, ctx, 0x99999999ull);
        cap_ctx c2 = cap_ctx_create(&be2);
        CHECK(cap_restore(c2, cap, 0) == CAP_ERR_FINGERPRINT, "fingerprint guard refuses foreign restore");
        cap_ctx_destroy(c2); flashrt_backend_fini(&be2);
    }

    cap_capsule_drop(c, cap); cap_capsule_drop(c, capg);
    cap_ctx_destroy(c);

    /* run the full backend conformance contract on the real GPU backend */
    {
        cap_backend bef; flashrt_backend_init(&bef, ctx, 0x99999999ull);
        int crc = run_backend_conformance(&be, &bef);
        CHECK(crc == 0, "backend conformance suite on FlashRT (GPU)");
        flashrt_backend_fini(&bef);
    }
    flashrt_backend_fini(&be);
    frt_graph_destroy(G); frt_ctx_destroy(ctx);

    std::printf(g_fail ? "\n== FLASHRT GPU SMOKE FAILED ==\n" : "\n== FLASHRT GPU SMOKE PASSED ==\n");
    return g_fail;
}
