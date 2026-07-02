/* test_runtime_adopt.cpp — GPU acceptance for the runtime-export adapter.
 *
 * Plays the PRODUCER by hand: captures a trivial graph via the frt exec
 * contract and fabricates an frt_runtime_export_v1 in place — deliberately
 * WITHOUT linking FlashRT's runtime library. That proves the consumer
 * property the ABI promises: an export is plain data; a host needs only the
 * headers. Then adopts it and drives fire / snapshot / restore THROUGH THE
 * CORE, and verifies lifetime (retain on adopt, release on fini, adopted
 * streams never destroyed).
 *
 * Requires a CUDA GPU + a built libflashrt_exec (same setup as test_flashrt_gpu).
 */
#include "capsule/capsule.h"
#include "flashrt_runtime_adapter.h"

#include <cuda_runtime.h>
#include <cstdio>
#include <cstring>
#include <cstdint>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::printf("FAIL: %s\n", (msg)); g_fail = 1; } \
    else { std::printf("ok  : %s\n", (msg)); } \
} while (0)

struct CopyRec { void* dst; const void* src; size_t n; };
static void record_copy(void* user, void* stream) {
    CopyRec* r = (CopyRec*)user;
    cudaMemcpyAsync(r->dst, r->src, r->n, cudaMemcpyDeviceToDevice, (cudaStream_t)stream);
}

static int g_retains = 0, g_releases = 0;
static void owner_retain(void*)  { ++g_retains; }
static void owner_release(void*) { ++g_releases; }

int main() {
    const size_t N = 1024;

    /* ---- producer side (by hand): capture one trivial model ---- */
    frt_ctx ctx = frt_ctx_create();
    if (!ctx) { std::printf("FAIL: frt_ctx_create (no GPU?)\n"); return 1; }

    frt_buffer S = frt_buffer_alloc(ctx, "src", N);
    frt_buffer L = frt_buffer_alloc(ctx, "out", N);
    unsigned char known[N]; for (size_t i = 0; i < N; ++i) known[i] = (unsigned char)(i * 3 + 5);
    cudaMemcpy(frt_buffer_dptr(S), known, N, cudaMemcpyHostToDevice);
    cudaMemset(frt_buffer_dptr(L), 0, N);

    frt_graph G = frt_graph_create(ctx, "copy", 8);
    CopyRec rec{ frt_buffer_dptr(L), frt_buffer_dptr(S), N };
    CHECK(frt_graph_capture(G, /*key*/7, record_copy, &rec) == FRT_OK, "producer: graph capture");

    cudaStream_t raw_main;                        /* producer-owned "main" stream */
    cudaStreamCreateWithFlags(&raw_main, cudaStreamNonBlocking);
    int frt_main = frt_ctx_wrap_stream(ctx, (void*)raw_main);
    CHECK(frt_main >= 0, "producer: wrap main stream");

    const frt_shape_key keys[1] = { 7 };
    frt_runtime_stream_desc streams[1] = { { "main", frt_main, 0, (void*)raw_main } };
    frt_runtime_graph_desc graphs[1] = { { "infer", G, 7, keys, 1, frt_main } };
    frt_runtime_buffer_desc buffers[2] = {
        { "src", S, N, FRT_RT_ROLE_INPUT, 0 },
        { "out", L, N, FRT_RT_ROLE_INPUT | FRT_RT_ROLE_OUTPUT, 0 },
    };
    frt_runtime_region_desc regions[1] = {
        { "boundary", L, 0, N, FRT_RT_REGION_SNAPSHOT | FRT_RT_REGION_RESTORE, 0 },
    };
    frt_runtime_export_v1 exp{};
    exp.abi_version = FRT_RUNTIME_ABI_VERSION;
    exp.struct_size = (uint32_t)sizeof(exp);
    exp.ctx = ctx;
    exp.streams = streams;         exp.n_streams = 1;
    exp.graphs = graphs;           exp.n_graphs = 1;
    exp.buffers = buffers;         exp.n_buffers = 2;
    exp.capsule_regions = regions; exp.n_capsule_regions = 1;
    exp.identity = "frt-runtime-identity-v1\nmodel=trivial\n";
    exp.fingerprint = 0xF00DF00Dull;
    exp.manifest_json = nullptr;
    exp.owner = nullptr; exp.retain = owner_retain; exp.release = owner_release;

    /* ---- consumer side: adopt + drive through the core ---- */
    {
        frt_runtime_export_v1 bad = exp; bad.abi_version = 999;
        flashrt_runtime_binding rej;
        CHECK(flashrt_adopt_runtime_export(&bad, &rej) == -2, "adopt rejects wrong abi_version");
    }

    flashrt_runtime_binding rb;
    CHECK(flashrt_adopt_runtime_export(&exp, &rb) == 0, "adopt_runtime_export");
    CHECK(g_retains == 1, "adopt retained the export exactly once");

    cap_ctx c = cap_ctx_create(&rb.backend);
    CHECK(c != nullptr, "cap_ctx_create over the adopted backend");
    CHECK(cap_ctx_fingerprint(c) == exp.fingerprint, "producer fingerprint reaches the core");

    cap_graph  g_infer = flashrt_runtime_graph(&rb, "infer");
    cap_buffer b_out   = flashrt_runtime_buffer(&rb, "out");
    int        s_main  = flashrt_runtime_stream(&rb, "main");
    CHECK(g_infer && b_out && s_main >= 0, "name lookups resolve");
    CHECK(!flashrt_runtime_graph(&rb, "nope") && flashrt_runtime_stream(&rb, "nope") < 0,
          "unknown names miss cleanly");
    (void)b_out;

    /* fire the exported graph on the ADOPTED stream, through the core */
    cap_stage st = { g_infer, /*key*/7, s_main, 0, 1, 1, CAP_EVERY };
    CHECK(cap_fire(c, &st) == CAP_OK, "cap_fire on the adopted stream");
    cap_sync(c, s_main);
    unsigned char got[N]; cudaMemcpy(got, frt_buffer_dptr(L), N, cudaMemcpyDeviceToHost);
    CHECK(std::memcmp(got, known, N) == 0, "exported graph replays bit-exact");

    /* capsule over the EXPORTED region set (order is contractual) */
    cap_boundary bnd = { rb.regions, (int)rb.n_regions, nullptr, 0 };
    cap_capsule cap = cap_snapshot(c, &bnd, CAP_TIER_HOST, s_main);
    CHECK(cap != nullptr, "snapshot over exported capsule_regions");
    cap_sync(c, s_main);
    cudaMemset(frt_buffer_dptr(L), 0xCD, N);
    CHECK(cap_restore(c, cap, s_main) == CAP_OK, "restore into the exported live region");
    cap_sync(c, s_main);
    cudaMemcpy(got, frt_buffer_dptr(L), N, cudaMemcpyDeviceToHost);
    CHECK(std::memcmp(got, known, N) == 0, "restore is bit-exact");

    cap_capsule_drop(c, cap);
    cap_ctx_destroy(c);
    flashrt_runtime_binding_fini(&rb);
    CHECK(g_releases == 1, "fini released the export exactly once");

    /* the adopted stream must still be alive and usable after fini */
    CHECK(cudaStreamQuery(raw_main) != cudaErrorInvalidResourceHandle,
          "adopted stream survives binding fini");
    cudaMemsetAsync(frt_buffer_dptr(L), 0, N, raw_main);
    CHECK(cudaStreamSynchronize(raw_main) == cudaSuccess, "adopted stream still usable");

    cudaStreamDestroy(raw_main);
    frt_graph_destroy(G); frt_ctx_destroy(ctx);

    std::printf(g_fail ? "\n== RUNTIME ADOPT FAILED ==\n" : "\n== RUNTIME ADOPT PASSED ==\n");
    return g_fail;
}
