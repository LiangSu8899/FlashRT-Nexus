/* test_embedded_session.cpp — C embedded session ABI over a tickable model. */
#include "nexus/embedded/session.h"

#include "stub_backend.h"

#include <cstdio>
#include <cstring>
#include <vector>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::printf("FAIL: %s\n", (msg)); g_fail = 1; } \
    else { std::printf("ok  : %s\n", (msg)); } \
} while (0)

constexpr size_t N = 64;

struct GraphCtx {
    cap_backend* be = nullptr;
    cap_buffer obs = nullptr;
    cap_buffer out = nullptr;
    int fires = 0;
};

static void copy_obs_to_out(void* p) {
    auto* g = static_cast<GraphCtx*>(p);
    g->be->buffer_copy(g->be->self, g->out, 0, g->obs, 0, N, 0);
    ++g->fires;
}

struct VerbCtx {
    cap_backend* be = nullptr;
    cap_buffer obs = nullptr;
    cap_buffer out = nullptr;
    int set_calls = 0;
};

static int staged_set_input(void* p, uint32_t port, const void* data,
                            uint64_t bytes, int stream) {
    (void)stream;
    auto* v = static_cast<VerbCtx*>(p);
    if (port != 1 || bytes > N) return CAP_ERR_ARG;
    ++v->set_calls;
    return v->be->buffer_upload(v->be->self, v->obs, 0, data, (size_t)bytes, 0);
}

static int get_output(void* p, uint32_t port, void* out, uint64_t capacity,
                      uint64_t* written, int stream) {
    (void)stream;
    auto* v = static_cast<VerbCtx*>(p);
    if (port != 2 || capacity < N) return CAP_ERR_ARG;
    if (written) *written = N;
    return v->be->buffer_download(v->be->self, v->out, 0, out, N, 0);
}

static const char* last_error(void*) { return ""; }

static bool same(const unsigned char* a, const unsigned char* b) {
    return std::memcmp(a, b, N) == 0;
}

int main() {
    cap_backend be{};
    stub_backend_init(&be, 0xE11BEDu);
    cap_buffer obs = be.buffer_alloc(be.self, "obs", N, CAP_HOST);
    cap_buffer out = be.buffer_alloc(be.self, "out", N, CAP_HOST);
    GraphCtx graph_ctx{&be, obs, out, 0};
    cap_graph graph = stub_graph_make(copy_obs_to_out, &graph_ctx);

    cap_stage sched_stage{graph, 0, 0, 0, 1, 1, CAP_EVERY};
    cap_schedule schedule{&sched_stage, 1, nullptr, 0};
    cap_model_stage model_stage{};
    model_stage.name = "infer";
    model_stage.graph = graph;
    model_stage.key = 0;
    model_stage.stream = 0;

    const int64_t flat_shape[1] = {(int64_t)N};
    cap_model_port ports[3]{};
    ports[0].name = "obs";
    ports[0].direction = 0;
    ports[0].update = 0;  /* SWAP */
    ports[0].shape = flat_shape;
    ports[0].rank = 1;
    ports[0].buffer = obs;
    ports[0].bytes = N;
    ports[1].name = "cmd";
    ports[1].direction = 0;
    ports[1].update = 1;  /* STAGED */
    ports[2].name = "actions";
    ports[2].direction = 1;
    ports[2].update = 1;
    ports[2].shape = flat_shape;
    ports[2].rank = 1;

    cap_region regions[1] = {{out, 0, N}};
    VerbCtx verbs{&be, obs, out, 0};
    cap_model_runtime model{};
    model.backend = &be;
    model.ports = ports;
    model.n_ports = 3;
    model.stages = &model_stage;
    model.n_stages = 1;
    model.regions = regions;
    model.n_regions = 1;
    model.schedule = schedule;
    model.fingerprint = 0xE11BEDu;
    model.identity = "embedded-session-test";
    model.self = &verbs;
    model.set_input = staged_set_input;
    model.get_output = get_output;
    model.last_error = last_error;

    nexus_embedded_config cfg{};
    cfg.struct_size = sizeof(cfg);
    cfg.model = &model;
    nexus_embedded_session* s = nullptr;
    CHECK(nexus_embedded_open(&cfg, &s) == CAP_OK && s,
          "embedded session opens over adopted model face");
    CHECK(nexus_embedded_fingerprint(s) == 0xE11BEDu,
          "embedded session exposes fingerprint");
    CHECK(nexus_embedded_find_port(s, "obs") == 0 &&
              nexus_embedded_find_port(s, "actions") == 2,
          "embedded session resolves ports by name");

    unsigned char a[N], b[N], got[N];
    for (size_t i = 0; i < N; ++i) {
        a[i] = (unsigned char)(i + 1);
        b[i] = (unsigned char)(255 - i);
    }

    nexus_embedded_tick_result tr{};
    CHECK(nexus_embedded_swap(s, "obs", a, N, 0) == CAP_OK,
          "embedded SWAP writes raw input window");
    CHECK(nexus_embedded_tick(s, &tr) == CAP_OK && tr.chunk_id == 1,
          "embedded tick runs the model DAG");
    uint64_t written = 0;
    CHECK(nexus_embedded_get_output(s, "actions", got, sizeof(got),
                                    &written, -1) == CAP_OK &&
              written == N && same(got, a),
          "embedded output returns replay result");

    std::memset(got, 0, sizeof(got));
    CHECK(nexus_embedded_act(s, "cmd", b, N, "actions", got, sizeof(got),
                             &tr) == CAP_OK &&
              tr.chunk_id == 2 && tr.written == N && same(got, b) &&
              verbs.set_calls == 1,
          "embedded act drives STAGED input, tick, output");

    std::memset(got, 0, sizeof(got));
    nexus_embedded_input step_in{};
    step_in.struct_size = sizeof(step_in);
    step_in.port = "obs";
    step_in.data = a;
    step_in.bytes = N;
    step_in.update = NEXUS_EMBEDDED_SWAP;
    step_in.stream = 0;
    nexus_embedded_output step_out{};
    step_out.struct_size = sizeof(step_out);
    step_out.port = "actions";
    step_out.data = got;
    step_out.capacity = sizeof(got);
    step_out.stream = -1;
    CHECK(nexus_embedded_step(s, &step_in, 1, &step_out, 1, &tr) == CAP_OK &&
              tr.chunk_id == 3 && tr.written == N &&
              step_out.written == N && same(got, a),
          "embedded step batches transport inputs, tick, and outputs");

    CHECK(nexus_embedded_snapshot(s, "ep-001") == CAP_OK &&
              nexus_embedded_capsule_count(s) == 1,
          "embedded snapshot stores a named capsule");
    CHECK(nexus_embedded_swap(s, "obs", b, N, 0) == CAP_OK &&
              nexus_embedded_tick(s, nullptr) == CAP_OK,
          "embedded dirty step changes live state");
    CHECK(nexus_embedded_restore(s, "ep-001") == CAP_OK &&
              nexus_embedded_get_output(s, "actions", got, sizeof(got),
                                        &written, -1) == CAP_OK &&
              same(got, a),
          "embedded restore restores origin capsule");

    size_t blob_len = 0;
    CHECK(nexus_embedded_serialize(s, "ep-001", nullptr, &blob_len) == CAP_OK &&
              blob_len > N,
          "embedded serialize size query");
    std::vector<unsigned char> blob(blob_len);
    CHECK(nexus_embedded_serialize(s, "ep-001", blob.data(), &blob_len) == CAP_OK,
          "embedded serialize blob");
    CHECK(nexus_embedded_load(s, "loaded", blob.data(), blob.size()) == CAP_OK &&
              nexus_embedded_capsule_count(s) == 2,
          "embedded load accepts persisted capsule");
    CHECK(nexus_embedded_swap(s, "obs", b, N, 0) == CAP_OK &&
              nexus_embedded_tick(s, nullptr) == CAP_OK &&
              nexus_embedded_restore(s, "loaded") == CAP_OK &&
              nexus_embedded_get_output(s, "actions", got, sizeof(got),
                                        &written, -1) == CAP_OK &&
              same(got, a),
          "embedded restore_into restores loaded capsule");
    CHECK(nexus_embedded_snapshot(s, "../bad") == CAP_ERR_ARG,
          "embedded capsule names reject paths");

    nexus_embedded_close(s);
    stub_graph_free(graph);
    be.buffer_free(be.self, obs);
    be.buffer_free(be.self, out);
    stub_backend_fini(&be);

    std::printf(g_fail ? "\n== EMBEDDED SESSION FAILED ==\n"
                       : "\n== EMBEDDED SESSION PASSED ==\n");
    return g_fail;
}
