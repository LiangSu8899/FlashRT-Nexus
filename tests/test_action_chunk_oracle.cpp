/* test_action_chunk_oracle.cpp — golden-vector oracle replay.
 *
 * Replays externally generated golden vectors through the temporal-fusion
 * consume policy and checks the fused chunk (f32, bit-exact) and the seated
 * switch index (exact) against the recorded reference outputs.
 *
 * The vector file is produced by the reference-seeded generator and is not
 * part of the repository; point NEXUS_AC_VECTORS at it to run. Without the
 * variable the test reports SKIP and succeeds, like the GPU smokes.
 *
 * Binary format (little-endian), version 1:
 *   u32 magic 'ACGV' (0x41434756), u32 version, u32 n_cases
 *   per case:
 *     u32 chunk_length, action_dim, max_chunks, switch_mode,
 *         representation, metric, state_dim, n_indices, n_chunks
 *     i32 switch_offset
 *     f64 decay
 *     u32 indices[n_indices]
 *     per chunk:
 *       u64 fire_step, ready_step
 *       f32 state_fire[state_dim], state_ready[state_dim]
 *       f32 actions[chunk_length * action_dim]
 *       f32 expected_fused[chunk_length * action_dim]
 *       u32 expected_switch_index
 *
 * A second optional file (NEXUS_AC_PROJ_VECTORS) replays projected-state
 * prepare vectors, magic 'ACPV' (0x41435056), version 1:
 *   u32 magic, version, n_cases
 *   per case:
 *     u32 chunk_length, action_dim, state_dim, n_indices, lookahead,
 *         start_index
 *     u32 indices[n_indices]
 *     f32 chunk[chunk_length * action_dim]
 *     f32 measured[state_dim]
 *     f32 expected_projected[state_dim]   (equality graded to <= 1 ulp:
 *         the reference sums with numpy's pairwise reduction, this
 *         implementation sequentially)
 *     u32 expected_count
 */
#include "nexus/modes/action_chunk/action_chunk.h"

#include "stub_backend.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static int g_fail = 0;

struct Reader {
    FILE* f = nullptr;
    bool ok = true;
    template <typename T> T get() {
        T v{};
        if (std::fread(&v, sizeof(T), 1, f) != 1) ok = false;
        return v;
    }
    template <typename T> void get_vec(std::vector<T>& out, size_t n) {
        out.resize(n);
        if (n && std::fread(out.data(), sizeof(T), n, f) != n) ok = false;
    }
};

struct VectorProbe {
    const float* data = nullptr;
    uint64_t bytes = 0;
};

static int vector_get_output(void* p, uint32_t, void* out, uint64_t capacity,
                             uint64_t* written, int) {
    auto* probe = static_cast<VectorProbe*>(p);
    if (written) *written = probe->bytes;
    if (capacity < probe->bytes) return CAP_ERR_ARG;
    std::memcpy(out, probe->data, probe->bytes);
    return CAP_OK;
}

static void noop_fire(void*) {}

static bool f32_within_one_ulp(float actual, float expected) {
    if (actual == expected) return true;
    return actual == std::nextafterf(expected, INFINITY) ||
           actual == std::nextafterf(expected, -INFINITY);
}

static int run_projection_vectors(const char* path) {
    int fail = 0;
    Reader r;
    r.f = std::fopen(path, "rb");
    if (!r.f) {
        std::printf("FAIL: cannot open %s\n", path);
        return 1;
    }
    if (r.get<uint32_t>() != 0x41435056u || r.get<uint32_t>() != 1u) {
        std::printf("FAIL: bad magic/version in %s\n", path);
        std::fclose(r.f);
        return 1;
    }
    const uint32_t n_cases = r.get<uint32_t>();
    uint32_t checked = 0;
    for (uint32_t c = 0; c < n_cases && r.ok; ++c) {
        const uint32_t length = r.get<uint32_t>();
        const uint32_t action_dim = r.get<uint32_t>();
        const uint32_t state_dim = r.get<uint32_t>();
        const uint32_t n_indices = r.get<uint32_t>();
        const uint32_t lookahead = r.get<uint32_t>();
        const uint32_t start_index = r.get<uint32_t>();
        std::vector<uint32_t> indices;
        r.get_vec(indices, n_indices);
        std::vector<float> chunk, measured, expected;
        r.get_vec(chunk, static_cast<size_t>(length) * action_dim);
        r.get_vec(measured, state_dim);
        r.get_vec(expected, state_dim);
        const uint32_t expected_count = r.get<uint32_t>();
        if (!r.ok) break;

        cap_backend be{};
        stub_backend_init(&be, 0x1234);
        cap_ctx ctx = cap_ctx_create(&be);
        cap_graph g0 = stub_graph_make(noop_fire, nullptr);
        cap_graph g1 = stub_graph_make(noop_fire, nullptr);
        uint32_t after1[1] = {0};
        cap_model_stage stages[2]{};
        stages[0].name = "context";
        stages[0].graph = g0;
        stages[0].stream = 0;
        stages[1].name = "action";
        stages[1].graph = g1;
        stages[1].stream = 1;
        stages[1].after = after1;
        stages[1].n_after = 1;
        cap_stage sched_stages[2] = {
            {g0, 0, 0, 0, 1, 1, CAP_EVERY},
            {g1, 0, 1, 0, 1, 1, CAP_EVERY},
        };
        int deps[1][2] = {{1, 0}};
        cap_schedule schedule{sched_stages, 2, deps, 1};
        const int64_t shape[2] = {static_cast<int64_t>(length),
                                  static_cast<int64_t>(action_dim)};
        cap_model_port ports[1]{};
        ports[0].name = "actions";
        ports[0].direction = CAP_MODEL_PORT_OUT;
        ports[0].update = CAP_MODEL_PORT_STAGED;
        ports[0].shape = shape;
        ports[0].rank = 2;
        VectorProbe probe;
        probe.bytes = static_cast<uint64_t>(length) * action_dim * 4;
        probe.data = chunk.data();
        cap_model_runtime model{};
        model.backend = &be;
        model.ports = ports;
        model.n_ports = 1;
        model.stages = stages;
        model.n_stages = 2;
        model.schedule = schedule;
        model.self = &probe;
        model.get_output = vector_get_output;

        nexus::StageDagRunner runner(ctx, &model);
        (void)runner.fire(0);

        nexus::ActionChunkConfig cfg{};
        cfg.action_stage = 1;
        cfg.output_port = 0;
        cfg.chunk_length = length;
        cfg.action_bytes = action_dim * 4;
        cfg.ring_slots = 2;
        cfg.execute_horizon = 0;
        cfg.prepare_policy = nexus::kActionChunkPrepareProjectedState;
        cfg.scalar_dtype = nexus::kActionChunkDtypeF32;
        cfg.action_representation = nexus::kActionChunkReprDeltaCumulative;
        cfg.state_dim = state_dim;
        cfg.lookahead_steps = lookahead;
        nexus::ActionChunkMode mode(&runner, cfg);
        if (n_indices)
            (void)mode.set_state_action_indices(indices.data(), n_indices);
        std::vector<float> out(state_dim);
        std::vector<float> scratch(action_dim);
        uint64_t written = 0;
        bool drove = true;
        drove = drove && mode.set_state(measured.data(), state_dim) == CAP_OK;
        drove = drove && mode.request() == CAP_OK &&
                mode.poll() == nexus::ActionChunkState::kReady;
        for (uint32_t i = 0; i < start_index && drove; ++i)
            drove = mode.next_action(scratch.data(), scratch.size() * 4,
                                     &written) ==
                    nexus::ActionChunkState::kReady;
        drove = drove && mode.set_state(measured.data(), state_dim) == CAP_OK;
        drove = drove && mode.begin_request() == CAP_OK;
        uint32_t dims = 0;
        drove = drove &&
                mode.projected_state(out.data(), state_dim, &dims) == CAP_OK;
        if (!drove) {
            std::printf("FAIL: proj case %u did not drive\n", c);
            fail = 1;
        } else {
            for (uint32_t k = 0; k < state_dim; ++k) {
                if (!f32_within_one_ulp(out[k], expected[k])) {
                    std::printf("FAIL: proj case %u dim %u %g != %g\n",
                                c, k, out[k], expected[k]);
                    fail = 1;
                }
            }
            if (mode.projected_count() != expected_count) {
                std::printf("FAIL: proj case %u count %u != %u\n", c,
                            mode.projected_count(), expected_count);
                fail = 1;
            }
            ++checked;
        }
        cap_ctx_destroy(ctx);
        stub_graph_free(g0);
        stub_graph_free(g1);
        stub_backend_fini(&be);
    }
    if (!r.ok) {
        std::printf("FAIL: truncated projection vector file\n");
        fail = 1;
    }
    std::fclose(r.f);
    std::printf("%s: %u projection cases replayed\n",
                fail ? "FAILED" : "PASSED", checked);
    return fail;
}

int main() {
    const char* proj_path = std::getenv("NEXUS_AC_PROJ_VECTORS");
    const char* path = std::getenv("NEXUS_AC_VECTORS");
    if ((!path || !*path) && (!proj_path || !*proj_path)) {
        std::printf("SKIP: set NEXUS_AC_VECTORS and/or NEXUS_AC_PROJ_VECTORS"
                    " to golden-vector files\n");
        return 0;
    }
    if (proj_path && *proj_path) g_fail |= run_projection_vectors(proj_path);
    if (!path || !*path) return g_fail;
    Reader r;
    r.f = std::fopen(path, "rb");
    if (!r.f) {
        std::printf("FAIL: cannot open %s\n", path);
        return 1;
    }
    if (r.get<uint32_t>() != 0x41434756u || r.get<uint32_t>() != 1u) {
        std::printf("FAIL: bad magic/version in %s\n", path);
        return 1;
    }
    const uint32_t n_cases = r.get<uint32_t>();
    uint32_t checked_chunks = 0;

    for (uint32_t c = 0; c < n_cases && r.ok; ++c) {
        const uint32_t length = r.get<uint32_t>();
        const uint32_t action_dim = r.get<uint32_t>();
        const uint32_t max_chunks = r.get<uint32_t>();
        const uint32_t switch_mode = r.get<uint32_t>();
        const uint32_t representation = r.get<uint32_t>();
        const uint32_t metric = r.get<uint32_t>();
        const uint32_t state_dim = r.get<uint32_t>();
        const uint32_t n_indices = r.get<uint32_t>();
        const uint32_t n_chunks = r.get<uint32_t>();
        const int32_t switch_offset = r.get<int32_t>();
        const double decay = r.get<double>();
        std::vector<uint32_t> indices;
        r.get_vec(indices, n_indices);

        cap_backend be{};
        stub_backend_init(&be, 0x1234);
        cap_ctx ctx = cap_ctx_create(&be);
        cap_graph g0 = stub_graph_make(noop_fire, nullptr);
        cap_graph g1 = stub_graph_make(noop_fire, nullptr);
        uint32_t after1[1] = {0};
        cap_model_stage stages[2]{};
        stages[0].name = "context";
        stages[0].graph = g0;
        stages[0].stream = 0;
        stages[1].name = "action";
        stages[1].graph = g1;
        stages[1].stream = 1;
        stages[1].after = after1;
        stages[1].n_after = 1;
        cap_stage sched_stages[2] = {
            {g0, 0, 0, 0, 1, 1, CAP_EVERY},
            {g1, 0, 1, 0, 1, 1, CAP_EVERY},
        };
        int deps[1][2] = {{1, 0}};
        cap_schedule schedule{sched_stages, 2, deps, 1};
        const int64_t shape[2] = {static_cast<int64_t>(length),
                                  static_cast<int64_t>(action_dim)};
        cap_model_port ports[1]{};
        ports[0].name = "actions";
        ports[0].direction = CAP_MODEL_PORT_OUT;
        ports[0].update = CAP_MODEL_PORT_STAGED;
        ports[0].shape = shape;
        ports[0].rank = 2;
        VectorProbe probe;
        probe.bytes = static_cast<uint64_t>(length) * action_dim * 4;
        cap_model_runtime model{};
        model.backend = &be;
        model.ports = ports;
        model.n_ports = 1;
        model.stages = stages;
        model.n_stages = 2;
        model.schedule = schedule;
        model.self = &probe;
        model.get_output = vector_get_output;

        nexus::StageDagRunner runner(ctx, &model);
        (void)runner.fire(0);

        nexus::ActionChunkConfig cfg{};
        cfg.action_stage = 1;
        cfg.output_port = 0;
        cfg.chunk_length = length;
        cfg.action_bytes = action_dim * 4;
        cfg.ring_slots = max_chunks + 1;
        cfg.execute_horizon = 0;
        cfg.consume_policy = nexus::kActionChunkConsumeTemporalFusion;
        cfg.switch_mode = static_cast<uint8_t>(switch_mode);
        cfg.scalar_dtype = nexus::kActionChunkDtypeF32;
        cfg.action_representation = static_cast<uint8_t>(representation);
        cfg.distance_metric = static_cast<uint8_t>(metric);
        cfg.state_dim = state_dim;
        cfg.fusion_decay = decay;
        cfg.fusion_max_chunks = max_chunks;
        cfg.switch_offset = switch_offset;
        if (nexus::ActionChunkMode::validate(cfg) != CAP_OK) {
            std::printf("FAIL: case %u config rejected\n", c);
            g_fail = 1;
        }
        nexus::ActionChunkMode mode(&runner, cfg);
        if (n_indices)
            (void)mode.set_state_action_indices(indices.data(), n_indices);

        std::vector<float> state_fire(state_dim), state_ready(state_dim);
        std::vector<float> actions, expected;
        for (uint32_t k = 0; k < n_chunks && r.ok; ++k) {
            const uint64_t fire_step = r.get<uint64_t>();
            const uint64_t ready_step = r.get<uint64_t>();
            r.get_vec(state_fire, state_dim);
            r.get_vec(state_ready, state_dim);
            r.get_vec(actions, static_cast<size_t>(length) * action_dim);
            r.get_vec(expected, static_cast<size_t>(length) * action_dim);
            const uint32_t expected_index = r.get<uint32_t>();
            if (!r.ok) break;

            while (mode.action_step() < fire_step) (void)mode.advance_step();
            if (state_dim) (void)mode.set_state(state_fire.data(), state_dim);
            if (mode.request() != CAP_OK) {
                std::printf("FAIL: case %u chunk %u request\n", c, k);
                g_fail = 1;
                break;
            }
            while (mode.action_step() < ready_step) (void)mode.advance_step();
            if (state_dim) (void)mode.set_state(state_ready.data(), state_dim);
            probe.data = actions.data();
            if (mode.poll() != nexus::ActionChunkState::kReady) {
                std::printf("FAIL: case %u chunk %u not ready\n", c, k);
                g_fail = 1;
                break;
            }
            if (std::memcmp(mode.fused_chunk(), expected.data(),
                            probe.bytes) != 0) {
                std::printf("FAIL: case %u chunk %u fused mismatch\n", c, k);
                g_fail = 1;
            }
            if (mode.active_index() != expected_index) {
                std::printf("FAIL: case %u chunk %u index %u != %u\n", c, k,
                            mode.active_index(), expected_index);
                g_fail = 1;
            }
            ++checked_chunks;
        }

        cap_ctx_destroy(ctx);
        stub_graph_free(g0);
        stub_graph_free(g1);
        stub_backend_fini(&be);
    }
    if (!r.ok) {
        std::printf("FAIL: truncated vector file\n");
        g_fail = 1;
    }
    std::fclose(r.f);
    std::printf("%s: %u cases, %u chunks replayed\n",
                g_fail ? "FAILED" : "PASSED", n_cases, checked_chunks);
    return g_fail;
}
