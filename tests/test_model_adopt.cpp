/* test_model_adopt.cpp — GPU acceptance for model-runtime adoption + the
 * HOT-INPUT contract.
 *
 * Plays the producer by hand (header-only, no FlashRT runtime lib linked):
 * fabricates an frt_model_runtime_v1 over two captured graphs — a two-stage
 * DAG (pre: obs->mid, infer: mid->out, infer after pre) — with a SWAP
 * observation port and a STAGED command port. Adopts it into the standard
 * face and pins down:
 *
 *   - wiring: ports/stages/regions/schedule/fingerprint/verbs;
 *   - the hot-input contract: updating SWAP (cap_swap) and STAGED
 *     (set_input) ports BETWEEN ticks never invalidates the captured graphs —
 *     replay output tracks buffer contents, round after round;
 *   - stage scheduling: cap_model_fire per stage, cap_model_tick for the DAG;
 *   - capsule over the exported regions mid-loop;
 *   - lifetime: adoption retains the model once; close releases everything.
 */
#include "capsule/model_runtime.h"
#include "flashrt_model_adapter.h"

#include <cuda_runtime.h>
#include <cstdio>
#include <cstring>
#include <cstdint>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::printf("FAIL: %s\n", (msg)); g_fail = 1; } \
    else { std::printf("ok  : %s\n", (msg)); } \
} while (0)

namespace {

constexpr size_t N = 2048;

struct CopyRec { void* dst; const void* src; size_t n; };
void record_copy(void* user, void* stream) {
    auto* r = static_cast<CopyRec*>(user);
    cudaMemcpyAsync(r->dst, r->src, r->n, cudaMemcpyDeviceToDevice,
                    (cudaStream_t)stream);
}

int g_retains = 0, g_releases = 0;
extern "C" void model_retain(void*)  { ++g_retains; }
extern "C" void model_release(void*) { ++g_releases; }

/* STAGED verb: host bytes -> the obs device buffer (graph-safe overwrite) */
struct VerbCtx { void* obs_dptr = nullptr; cudaStream_t stream = nullptr; int calls = 0; };
extern "C" int staged_set_input(void* self, uint32_t port, const void* data,
                                uint64_t bytes, int stream) {
    (void)stream;
    auto* v = static_cast<VerbCtx*>(self);
    if (port != 1 || bytes > N) return -1;
    v->calls++;
    return cudaMemcpyAsync(v->obs_dptr, data, bytes, cudaMemcpyHostToDevice,
                           v->stream) == cudaSuccess ? 0 : -6;
}
extern "C" const char* verb_last_error(void*) { return ""; }

}  // namespace

int main() {
    frt_ctx ctx = frt_ctx_create();
    if (!ctx) { std::printf("FAIL: frt_ctx_create (no GPU?)\n"); return 1; }

    frt_buffer obs = frt_buffer_alloc(ctx, "obs", N);
    frt_buffer mid = frt_buffer_alloc(ctx, "mid", N);
    frt_buffer out = frt_buffer_alloc(ctx, "out", N);
    cudaMemset(frt_buffer_dptr(obs), 0, N);

    cudaStream_t raw_main;
    cudaStreamCreateWithFlags(&raw_main, cudaStreamNonBlocking);
    int frt_main = frt_ctx_wrap_stream(ctx, (void*)raw_main);

    frt_graph g_pre = frt_graph_create(ctx, "pre", 1);
    CopyRec pre_rec{ frt_buffer_dptr(mid), frt_buffer_dptr(obs), N };
    CHECK(frt_graph_capture(g_pre, 0, record_copy, &pre_rec) == FRT_OK,
          "producer: capture pre (obs->mid)");
    frt_graph g_infer = frt_graph_create(ctx, "infer", 1);
    CopyRec infer_rec{ frt_buffer_dptr(out), frt_buffer_dptr(mid), N };
    CHECK(frt_graph_capture(g_infer, 0, record_copy, &infer_rec) == FRT_OK,
          "producer: capture infer (mid->out)");

    /* ---- fabricate the export + model runtime (plain data) ---- */
    frt_runtime_stream_desc streams[1] = { { "main", frt_main, 0, (void*)raw_main } };
    frt_runtime_graph_desc graphs[2] = {
        { "pre",   g_pre,   0, nullptr, 0, frt_main },
        { "infer", g_infer, 0, nullptr, 0, frt_main },
    };
    frt_runtime_buffer_desc buffers[3] = {
        { "obs", obs, N, FRT_RT_ROLE_INPUT, 0 },
        { "mid", mid, N, FRT_RT_ROLE_SCRATCH, 0 },
        { "out", out, N, FRT_RT_ROLE_OUTPUT, 0 },
    };
    frt_runtime_region_desc regions[1] = {
        { "boundary", out, 0, N, 3, 0 },
    };
    frt_runtime_export_v1 exp{};
    exp.abi_version = FRT_RUNTIME_ABI_VERSION;
    exp.struct_size = (uint32_t)sizeof(exp);
    exp.ctx = ctx;
    exp.streams = streams;         exp.n_streams = 1;
    exp.graphs = graphs;           exp.n_graphs = 2;
    exp.buffers = buffers;         exp.n_buffers = 3;
    exp.capsule_regions = regions; exp.n_capsule_regions = 1;
    exp.identity = "model-adopt-test";
    exp.fingerprint = 0xAB12CD34ull;
    exp.retain = model_retain; exp.release = model_release;

    const int64_t obs_shape[1] = { (int64_t)N };
    frt_runtime_port_desc ports[3] = {};
    ports[0] = { "obs", FRT_RT_MOD_STATE, FRT_RT_DTYPE_U8, FRT_RT_LAYOUT_FLAT,
                 FRT_RT_PORT_IN, FRT_RT_PORT_SWAP, 1, obs_shape, 1, 100,
                 obs, 0, N };
    ports[1] = { "cmd", FRT_RT_MOD_TEXT, FRT_RT_DTYPE_U8, FRT_RT_LAYOUT_FLAT,
                 FRT_RT_PORT_IN, FRT_RT_PORT_STAGED, 0, nullptr, 0, 0,
                 nullptr, 0, 0 };
    ports[2] = { "act", FRT_RT_MOD_ACTION, FRT_RT_DTYPE_U8, FRT_RT_LAYOUT_FLAT,
                 FRT_RT_PORT_OUT, FRT_RT_PORT_SWAP, 0, obs_shape, 1, 0,
                 out, 0, N };
    const uint32_t infer_after[1] = { 0 };
    frt_runtime_stage_desc stages[2] = {
        { 0, 0, nullptr },          /* pre                     */
        { 1, 1, infer_after },      /* infer, after pre        */
    };

    VerbCtx vctx;
    vctx.obs_dptr = frt_buffer_dptr(obs);
    vctx.stream = raw_main;
    frt_model_runtime_v1 model{};
    model.abi_version = FRT_MODEL_RUNTIME_ABI_VERSION;
    model.struct_size = (uint32_t)sizeof(model);
    model.exp = &exp;
    model.ports = ports;   model.n_ports = 3;
    model.stages = stages; model.n_stages = 2;
    model.self = &vctx;
    model.verbs.struct_size = (uint32_t)sizeof(model.verbs);
    model.verbs.set_input = staged_set_input;
    model.verbs.last_error = verb_last_error;
    model.retain = model_retain; model.release = model_release;

    /* ---- adopt ---- */
    {
        frt_model_runtime_v1 bad = model; bad.abi_version = 99;
        cap_model_runtime* rej = nullptr;
        CHECK(flashrt_adopt_model_runtime(&bad, &rej) == -2,
              "adopt rejects wrong abi_version");
    }
    cap_model_runtime* m = nullptr;
    CHECK(flashrt_adopt_model_runtime(&model, &m) == 0 && m,
          "flashrt_adopt_model_runtime");
    const int base_retains = g_retains;
    CHECK(base_retains >= 2, "adoption retained model + export");

    cap_ctx c = cap_ctx_create(cap_model_backend(m));
    CHECK(c != nullptr, "cap_ctx over the adopted model");
    CHECK(cap_ctx_fingerprint(c) == exp.fingerprint &&
              cap_model_fingerprint(m) == exp.fingerprint,
          "producer fingerprint reaches core and face");

    int p_obs = cap_model_find_port(m, "obs");
    int p_cmd = cap_model_find_port(m, "cmd");
    int p_act = cap_model_find_port(m, "act");
    CHECK(p_obs == 0 && p_cmd == 1 && p_act == 2 &&
              cap_model_find_port(m, "nope") < 0,
          "port lookups");
    CHECK(cap_model_port_update(m, p_obs) == 0 &&
              cap_model_port_buffer(m, p_obs) != nullptr,
          "SWAP port carries a wired capsule buffer");
    CHECK(m->n_stages == 2 && m->stages[1].n_after == 1 &&
              m->stages[1].after[0] == 0 &&
              std::strcmp(m->stages[1].name, "infer") == 0,
          "stage DAG survives adoption");
    CHECK(m->schedule.n_stages == 2 && m->schedule.n_deps == 1,
          "prepared schedule mirrors the DAG");

    /* ---- HOT-INPUT CONTRACT: update ports between ticks, replay tracks
     * contents, the captured graphs stay valid round after round ---- */
    unsigned char pattern[N], got[N];
    bool hot_ok = true;
    for (int round = 0; round < 5 && hot_ok; ++round) {
        for (size_t i = 0; i < N; ++i)
            pattern[i] = (unsigned char)(i * 3 + round * 17 + 1);
        if (round % 2 == 0) {
            /* fast lane: raw window write through the core */
            hot_ok = cap_swap(c, cap_model_port_buffer(m, p_obs), pattern, N,
                              m->stages[0].stream) == CAP_OK;
        } else {
            /* staged lane: the producer's verb */
            hot_ok = cap_model_set_input(m, (uint32_t)p_cmd, pattern, N,
                                         m->stages[0].stream) == 0;
        }
        if (!hot_ok) break;
        hot_ok = cap_model_tick(c, m) == CAP_OK;
        if (!hot_ok) break;
        cap_sync(c, m->stages[1].stream);
        cudaMemcpy(got, frt_buffer_dptr(out), N, cudaMemcpyDeviceToHost);
        hot_ok = std::memcmp(got, pattern, N) == 0;
    }
    CHECK(hot_ok, "hot inputs: 5 rounds of SWAP/STAGED updates between ticks, "
                  "replay output tracks contents, no recapture");
    CHECK(vctx.calls == 2, "staged rounds went through the producer verb");

    /* per-stage fire (the scheduling-host path) */
    cudaMemset(frt_buffer_dptr(out), 0xEE, N);
    CHECK(cap_model_fire(c, m, 0) == CAP_OK && cap_model_fire(c, m, 1) == CAP_OK,
          "cap_model_fire per stage");
    cap_sync(c, m->stages[1].stream);
    cudaMemcpy(got, frt_buffer_dptr(out), N, cudaMemcpyDeviceToHost);
    CHECK(std::memcmp(got, pattern, N) == 0, "manual stage firing is equivalent");

    /* capsule over the exported boundary, mid-loop */
    cap_boundary bnd = { cap_model_region_array(m), cap_model_region_count(m),
                         nullptr, 0 };
    cap_capsule cap = cap_snapshot(c, &bnd, CAP_TIER_HOST, m->stages[1].stream);
    CHECK(cap != nullptr, "snapshot over the model's regions");
    cap_sync(c, m->stages[1].stream);
    cudaMemset(frt_buffer_dptr(out), 0, N);
    CHECK(cap_restore(c, cap, m->stages[1].stream) == CAP_OK, "restore");
    cap_sync(c, m->stages[1].stream);
    cudaMemcpy(got, frt_buffer_dptr(out), N, cudaMemcpyDeviceToHost);
    CHECK(std::memcmp(got, pattern, N) == 0, "restore is bit-exact");

    cap_capsule_drop(c, cap);
    cap_ctx_destroy(c);
    flashrt_model_close(m);
    CHECK(g_releases == g_retains, "close released every reference");

    cudaStreamDestroy(raw_main);
    frt_graph_destroy(g_pre); frt_graph_destroy(g_infer);
    frt_ctx_destroy(ctx);

    std::printf(g_fail ? "\n== MODEL ADOPT FAILED ==\n"
                       : "\n== MODEL ADOPT PASSED ==\n");
    return g_fail;
}
