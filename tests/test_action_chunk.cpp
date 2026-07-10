/* test_action_chunk.cpp — action-chunk mode mechanism properties.
 *
 * Covers the v2 mechanism on the stub backend: config versioning and
 * validation, the two-phase request, the two logical clocks (controller-step
 * grid vs pending-poll watchdog), hold-last miss handling, the blocking sync
 * verb, the state feed, and the determinism/replay contract.
 */
#include "nexus/modes/action_chunk/action_chunk.h"
#include "nexus/modes/action_chunk/action_chunk_c.h"
#include "nexus/schedulers/stage_dag_c.h"

#include "stub_backend.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::printf("FAIL: %s\n", (msg)); g_fail = 1; } \
    else { std::printf("ok  : %s\n", (msg)); } \
} while (0)

/* ---- fixture: a two-stage model whose action stage emits 3x1 chunks ---- */

struct ChunkProbe {
    int reads = 0;
    int state_writes = 0;
    float last_state = 0.0f;
};

static int chunk_get_output(void* p, uint32_t, void* out, uint64_t capacity,
                            uint64_t* written, int) {
    constexpr uint64_t kBytes = 12;  /* 3 actions * 4 bytes */
    if (written) *written = kBytes;
    if (capacity < kBytes) return CAP_ERR_ARG;
    auto* probe = static_cast<ChunkProbe*>(p);
    unsigned char* dst = static_cast<unsigned char*>(out);
    for (uint64_t i = 0; i < kBytes; ++i)
        dst[i] = static_cast<unsigned char>('A' + probe->reads + i);
    probe->reads += 1;
    return CAP_OK;
}

static int chunk_set_input(void* p, uint32_t port, const void* data,
                           uint64_t bytes, int) {
    if (port != 1 || !data || bytes != sizeof(float)) return CAP_ERR_ARG;
    auto* probe = static_cast<ChunkProbe*>(p);
    std::memcpy(&probe->last_state, data, sizeof(float));
    ++probe->state_writes;
    return CAP_OK;
}

struct AsyncEvent {
    int remaining = 0;
    bool recorded = false;
};

static int g_event_pending = 0;
static AsyncEvent* g_last_event = nullptr;

static cap_event async_event(void*) {
    return reinterpret_cast<cap_event>(new AsyncEvent());
}
static int async_event_record(void*, cap_event ev, int) {
    auto* e = reinterpret_cast<AsyncEvent*>(ev);
    e->remaining = g_event_pending;
    e->recorded = true;
    g_last_event = e;
    return CAP_OK;
}
static int async_event_query(void*, cap_event ev) {
    auto* e = reinterpret_cast<AsyncEvent*>(ev);
    if (!e->recorded) return 0;
    if (e->remaining > 0) {
        --e->remaining;
        return 1;
    }
    return 0;
}
static int async_stream_wait(void*, int, cap_event) { return CAP_OK; }
static int async_sync(void*, int) {
    if (g_last_event) g_last_event->remaining = 0;
    return CAP_OK;
}
static void async_event_free(void*, cap_event ev) {
    delete reinterpret_cast<AsyncEvent*>(ev);
}

static void noop_fire(void*) {}

struct Fixture {
    cap_backend be{};
    cap_ctx ctx = nullptr;
    ChunkProbe chunks;
    cap_graph g0 = nullptr;
    cap_graph g1 = nullptr;
    uint32_t after1[1] = {0};
    cap_model_stage stages[2]{};
    cap_stage sched_stages[2]{};
    int deps[1][2] = {{1, 0}};
    cap_schedule schedule{};
    int64_t shape[2] = {3, 1};
    int64_t state_shape[1] = {1};
    cap_model_port ports[2]{};
    cap_model_runtime model{};

    explicit Fixture(bool async_events) {
        stub_backend_init(&be, 0x1234);
        if (async_events) {
            be.event = async_event;
            be.event_record = async_event_record;
            be.event_query = async_event_query;
            be.stream_wait = async_stream_wait;
            be.sync = async_sync;
            be.event_free = async_event_free;
        }
        ctx = cap_ctx_create(&be);
        g0 = stub_graph_make(noop_fire, nullptr);
        g1 = stub_graph_make(noop_fire, nullptr);
        stages[0].name = "context";
        stages[0].graph = g0;
        stages[0].stream = 0;
        stages[1].name = "action";
        stages[1].graph = g1;
        stages[1].stream = 1;
        stages[1].after = after1;
        stages[1].n_after = 1;
        sched_stages[0] = {g0, 0, 0, 0, 1, 1, CAP_EVERY};
        sched_stages[1] = {g1, 0, 1, 0, 1, 1, CAP_EVERY};
        schedule = {sched_stages, 2, deps, 1};
        ports[0].name = "actions";
        ports[0].direction = 1;
        ports[0].update = 1;
        ports[0].shape = shape;
        ports[0].rank = 2;
        ports[1].name = "state";
        ports[1].modality = 3;
        ports[1].dtype = 1;
        ports[1].direction = 0;
        ports[1].update = 1;
        ports[1].shape = state_shape;
        ports[1].rank = 1;
        model.backend = &be;
        model.ports = ports;
        model.n_ports = 2;
        model.stages = stages;
        model.n_stages = 2;
        model.schedule = schedule;
        model.self = &chunks;
        model.set_input = chunk_set_input;
        model.get_output = chunk_get_output;
    }

    ~Fixture() {
        cap_ctx_destroy(ctx);
        stub_graph_free(g0);
        stub_graph_free(g1);
        stub_backend_fini(&be);
    }
};

/* Stage 1 declares `after: [0]`; fire the context stage once so the action
 * stage's dependency has an event, mirroring a real context/action loop. */
static void prime_context(nexus::StageDagRunner& runner) {
    const int save = g_event_pending;
    g_event_pending = 0;
    (void)runner.fire(0);
    g_event_pending = save;
}

static nexus::ActionChunkConfig chunk_config() {
    nexus::ActionChunkConfig cfg{};
    cfg.action_stage = 1;
    cfg.output_port = 0;
    cfg.chunk_length = 3;
    cfg.action_bytes = 4;
    cfg.ring_slots = 2;
    cfg.execute_horizon = 1;
    return cfg;
}

/* ---- C ABI config versioning and validation ---------------------------- */

static void test_config_versioning() {
    Fixture fx(false);
    nexus_stage_dag* dag = nullptr;
    CHECK(nexus_stage_dag_create(fx.ctx, &fx.model, &dag) == CAP_OK && dag,
          "versioning fixture creates a stage DAG");

    nexus_action_chunk_config cfg{};
    cfg.action_stage = 1;
    cfg.output_port = 0;
    cfg.chunk_length = 3;
    cfg.action_bytes = 4;
    cfg.ring_slots = 2;
    cfg.execute_horizon = 1;
    cfg.poll_budget = -1;

    /* A v1 caller passes the old sizeof (48: fields through reserved1 plus
     * tail padding). Every v2 field must take its default. */
    cfg.struct_size = 48;
    cfg.deadline_steps = 77;  /* garbage in the old padding zone: ignored */
    nexus_action_chunk* h = nullptr;
    CHECK(nexus_action_chunk_create(dag, &cfg, &h) == CAP_OK && h,
          "v1-sized config is accepted");
    nexus_action_chunk_destroy(h);
    h = nullptr;

    cfg.struct_size = sizeof(nexus_action_chunk_config);
    cfg.deadline_steps = -1;
    CHECK(nexus_action_chunk_create(dag, &cfg, &h) == CAP_OK && h,
          "v2-sized config is accepted");
    nexus_action_chunk_destroy(h);
    h = nullptr;

    cfg.struct_size = 20;
    CHECK(nexus_action_chunk_create(dag, &cfg, &h) == CAP_ERR_VERSION && !h,
          "undersized struct_size is a version error");
    cfg.struct_size = sizeof(nexus_action_chunk_config) + 8;
    CHECK(nexus_action_chunk_create(dag, &cfg, &h) == CAP_ERR_VERSION && !h,
          "oversized struct_size is a version error");

    cfg.struct_size = sizeof(nexus_action_chunk_config);
    cfg.consume_policy = 7;
    CHECK(nexus_action_chunk_create(dag, &cfg, &h) == CAP_ERR_ARG && !h,
          "unknown consume policy is rejected at create");
    cfg.consume_policy = NEXUS_AC_CONSUME_PLAIN;
    cfg.candidates = 2;
    CHECK(nexus_action_chunk_create(dag, &cfg, &h) == CAP_ERR_ARG && !h,
          "candidates > 1 is rejected at create");
    cfg.candidates = 1;
    CHECK(nexus_action_chunk_create(dag, &cfg, &h) == CAP_OK && h,
          "candidates == 1 is accepted");
    nexus_action_chunk_destroy(h);
    h = nullptr;

    cfg.prepare_policy = NEXUS_AC_PREPARE_PROJECTED_STATE;
    cfg.scalar_dtype = NEXUS_AC_DTYPE_F32;
    cfg.action_representation = NEXUS_AC_REPR_DELTA_CUMULATIVE;
    cfg.state_dim = 1;
    cfg.lookahead_steps = 2;
    cfg.state_input_port = 1;
    CHECK(nexus_action_chunk_create(dag, &cfg, &h) == CAP_ERR_ARG && !h,
          "C create rejects a non-state projected-state input port");
    cfg.state_input_port = 2;
    CHECK(nexus_action_chunk_create(dag, &cfg, &h) == CAP_OK && h,
          "C create accepts a matching STATE/F32/STAGED input port");
    const float measured[1] = {7.0f};
    CHECK(nexus_action_chunk_set_state(h, measured, 1) == CAP_OK &&
              nexus_action_chunk_begin_request(h) == CAP_OK &&
              fx.chunks.state_writes == 1 && fx.chunks.last_state == 7.0f,
          "C begin stages projected state through the declared model port");
    nexus_action_chunk_destroy(h);
    h = nullptr;

    cfg.prepare_policy = NEXUS_AC_PREPARE_NONE;
    CHECK(nexus_action_chunk_create(dag, &cfg, &h) == CAP_ERR_ARG && !h,
          "state_input_port is exclusive to projected-state prepare");

    nexus_stage_dag_destroy(dag);
}

/* ---- two-phase request -------------------------------------------------- */

static void test_two_phase_request() {
    Fixture fx(false);
    nexus::StageDagRunner runner(fx.ctx, &fx.model);
    prime_context(runner);
    nexus::ActionChunkMode mode(&runner, chunk_config());

    CHECK(mode.begin_request() == CAP_OK && mode.prepared_requests() == 1 &&
              !mode.in_flight(),
          "begin_request prepares without firing");
    CHECK(mode.begin_request() == CAP_OK && mode.prepared_requests() == 1,
          "begin_request is idempotent until commit");
    CHECK(mode.commit_request() == CAP_OK && mode.in_flight(),
          "commit_request fires the prepared request");
    CHECK(mode.begin_request() == CAP_ERR_ARG,
          "begin_request while in flight is an error");
    CHECK(mode.poll() == nexus::ActionChunkState::kReady,
          "prepared fire completes");
    CHECK(mode.request() == CAP_OK && mode.prepared_requests() == 2,
          "request() runs begin + commit in one verb");
    CHECK(mode.poll() == nexus::ActionChunkState::kReady,
          "one-verb request completes");
}

/* ---- the two clocks ------------------------------------------------------ */

static void test_dual_clocks() {
    Fixture fx(true);
    nexus::StageDagRunner runner(fx.ctx, &fx.model);
    prime_context(runner);

    /* Grid deadline: fire, advance the grid past deadline_steps, poll. */
    nexus::ActionChunkConfig cfg = chunk_config();
    cfg.poll_budget = -1;
    cfg.deadline_steps = 2;
    nexus::ActionChunkMode mode(&runner, cfg);
    g_event_pending = 4;
    CHECK(mode.request() == CAP_OK, "grid-deadline fixture fires");
    CHECK(mode.advance_step() == CAP_OK && mode.advance_step() == CAP_OK &&
              mode.advance_step() == CAP_OK && mode.action_step() == 3,
          "advance_step moves the controller-step grid");
    CHECK(mode.poll() == nexus::ActionChunkState::kFallback &&
              mode.fallbacks() == 1,
          "grid deadline overrun reports fallback once");
    CHECK(mode.poll() == nexus::ActionChunkState::kPending &&
              mode.fallbacks() == 1,
          "fallback is reported exactly once per fire");
    nexus::ActionChunkState s = mode.poll();
    while (s == nexus::ActionChunkState::kPending) s = mode.poll();
    CHECK(s == nexus::ActionChunkState::kReady && mode.late_chunks() == 1 &&
              mode.last_d_steps() == 3,
          "late chunk after grid overrun is accepted and stamped");

    /* Neither clock enabled: pending never falls back. */
    nexus::ActionChunkConfig cfg_off = chunk_config();
    cfg_off.poll_budget = -1;
    cfg_off.deadline_steps = -1;
    nexus::ActionChunkMode mode_off(&runner, cfg_off);
    g_event_pending = 6;
    CHECK(mode_off.request() == CAP_OK, "clock-free fixture fires");
    bool never_fallback = true;
    for (int i = 0; i < 5; ++i)
        never_fallback = never_fallback &&
                         mode_off.poll() == nexus::ActionChunkState::kPending;
    CHECK(never_fallback && mode_off.fallbacks() == 0,
          "with both clocks disabled pending never falls back");
    while (mode_off.poll() != nexus::ActionChunkState::kReady) {}

    /* poll() must never advance the grid. */
    CHECK(mode_off.action_step() == 0,
          "poll never advances the controller-step grid");
}

/* ---- hold-last miss policy ---------------------------------------------- */

static void test_hold_last() {
    Fixture fx(true);
    nexus::StageDagRunner runner(fx.ctx, &fx.model);
    nexus::ActionChunkConfig cfg = chunk_config();
    cfg.execute_horizon = 0;  /* no prefetch: exhaustion leaves nothing */
    cfg.miss_policy = nexus::kActionChunkMissHoldLast;
    nexus::ActionChunkMode mode(&runner, cfg);
    prime_context(runner);

    g_event_pending = 0;
    CHECK(mode.request() == CAP_OK &&
              mode.poll() == nexus::ActionChunkState::kReady,
          "hold-last fixture seats the first chunk");
    unsigned char out[4] = {};
    unsigned char last[4] = {};
    uint64_t written = 0;
    for (int i = 0; i < 3; ++i) {
        CHECK(mode.next_action(out, sizeof(out), &written) ==
                  nexus::ActionChunkState::kReady,
              "hold-last fixture emits a fresh action");
        std::memcpy(last, out, sizeof(last));
    }
    CHECK(!mode.has_active_chunk(), "chunk is exhausted");

    g_event_pending = 5;  /* refill stays pending */
    std::memset(out, 0, sizeof(out));
    CHECK(mode.next_action(out, sizeof(out), &written) ==
                  nexus::ActionChunkState::kFallback &&
              std::memcmp(out, last, sizeof(out)) == 0 &&
              mode.held_actions() == 1 && mode.in_flight(),
          "exhaustion with nothing ready re-emits the held action");
    CHECK(mode.action_step() == 4 && mode.emitted_actions() == 4,
          "held emission advances the grid and counts as emitted");
}

/* ---- blocking sync verb -------------------------------------------------- */

static void test_sync_next_chunk() {
    Fixture fx(true);
    nexus::StageDagRunner runner(fx.ctx, &fx.model);
    prime_context(runner);
    nexus::ActionChunkMode mode(&runner, chunk_config());
    g_event_pending = 100;  /* would pend for 100 polls */
    CHECK(mode.sync_next_chunk() == nexus::ActionChunkState::kReady &&
              mode.has_active_chunk(),
          "sync_next_chunk blocks through a pending fire and consumes");
}

/* ---- state feed ---------------------------------------------------------- */

static void test_state_feed() {
    Fixture fx(false);
    nexus::StageDagRunner runner(fx.ctx, &fx.model);
    nexus::ActionChunkConfig cfg = chunk_config();
    cfg.state_dim = 3;
    nexus::ActionChunkMode mode(&runner, cfg);
    prime_context(runner);

    const float good[3] = {0.5f, -1.0f, 2.0f};
    const float bad[3] = {0.5f, NAN, 2.0f};
    CHECK(mode.set_state(good, 2) == CAP_ERR_ARG,
          "state with the wrong dimension is rejected");
    CHECK(mode.set_state(bad, 3) == CAP_ERR_ARG && mode.state_updates() == 0,
          "non-finite state is rejected");
    CHECK(mode.set_state(good, 3) == CAP_OK && mode.state_updates() == 1,
          "finite state of the declared dimension is accepted");

    const uint32_t dup[3] = {0, 0, UINT32_MAX};
    const uint32_t idx[3] = {0, UINT32_MAX, UINT32_MAX};
    const uint32_t empty[3] = {UINT32_MAX, UINT32_MAX, UINT32_MAX};
    CHECK(mode.set_state_action_indices(dup, 3) == CAP_ERR_ARG,
          "duplicate state-action indices are rejected");
    CHECK(mode.set_state_action_indices(empty, 3) == CAP_ERR_ARG,
          "an entirely unprojected state-action map is rejected");
    CHECK(mode.set_state_action_indices(idx, 3) == CAP_OK,
          "state-action indices are accepted before the first request");
    CHECK(mode.request() == CAP_OK, "state fixture fires");
    CHECK(mode.set_state_action_indices(idx, 3) == CAP_ERR_ARG,
          "state-action indices are frozen after the first request");
    while (mode.poll() != nexus::ActionChunkState::kReady) {}
}

/* ---- consume policies: validation matrix --------------------------------- */

static void test_policy_validation() {
    nexus::ActionChunkConfig cfg = chunk_config();
    cfg.consume_policy = nexus::kActionChunkConsumeTemporalFusion;
    cfg.scalar_dtype = nexus::kActionChunkDtypeRaw;
    cfg.ring_slots = 4;
    CHECK(nexus::ActionChunkMode::validate(cfg) == CAP_ERR_ARG,
          "fusion with raw dtype is rejected");
    cfg.scalar_dtype = nexus::kActionChunkDtypeF32;
    cfg.ring_slots = 2;  /* default max_chunks 3 needs >= 4 */
    CHECK(nexus::ActionChunkMode::validate(cfg) == CAP_ERR_ARG,
          "fusion with an undersized ring is rejected");
    cfg.ring_slots = 4;
    CHECK(nexus::ActionChunkMode::validate(cfg) == CAP_OK,
          "fusion with f32 dtype and a sized ring is accepted");
    cfg.consume_policy = nexus::kActionChunkConsumeSwitch;
    cfg.switch_mode = nexus::kActionChunkSwitchState;
    cfg.state_dim = 0;
    CHECK(nexus::ActionChunkMode::validate(cfg) == CAP_ERR_ARG,
          "state switch without a state feed is rejected");
    cfg.state_dim = 1;
    CHECK(nexus::ActionChunkMode::validate(cfg) == CAP_OK,
          "state switch with a state feed is accepted");
    cfg.switch_mode = nexus::kActionChunkSwitchLatency;
    cfg.scalar_dtype = nexus::kActionChunkDtypeRaw;
    CHECK(nexus::ActionChunkMode::validate(cfg) == CAP_OK,
          "latency switch does no arithmetic and accepts raw dtype");
}

/* ---- float-emitting probe for the arithmetic policies -------------------- */

struct FloatProbe {
    const float* data = nullptr;  /* 3 floats per chunk */
    int state_writes = 0;
    float last_state = 0.0f;
};

static int float_get_output(void* p, uint32_t, void* out, uint64_t capacity,
                            uint64_t* written, int) {
    constexpr uint64_t kBytes = 12;
    if (written) *written = kBytes;
    if (capacity < kBytes) return CAP_ERR_ARG;
    auto* probe = static_cast<FloatProbe*>(p);
    std::memcpy(out, probe->data, kBytes);
    return CAP_OK;
}

static int float_set_input(void* p, uint32_t port, const void* data,
                           uint64_t bytes, int) {
    if (port != 1 || !data || bytes != sizeof(float)) return CAP_ERR_ARG;
    auto* probe = static_cast<FloatProbe*>(p);
    std::memcpy(&probe->last_state, data, sizeof(float));
    ++probe->state_writes;
    return CAP_OK;
}

/* ---- consume = switch ----------------------------------------------------- */

static void test_consume_switch_latency() {
    Fixture fx(true);
    FloatProbe probe;
    fx.model.self = &probe;
    fx.model.get_output = float_get_output;
    nexus::StageDagRunner runner(fx.ctx, &fx.model);
    prime_context(runner);

    nexus::ActionChunkConfig cfg = chunk_config();
    cfg.consume_policy = nexus::kActionChunkConsumeSwitch;
    nexus::ActionChunkMode mode(&runner, cfg);
    const float chunk[3] = {10.0f, 20.0f, 30.0f};
    probe.data = chunk;
    g_event_pending = 0;
    CHECK(mode.request() == CAP_OK, "latency-switch fixture fires");
    CHECK(mode.advance_step() == CAP_OK && mode.advance_step() == CAP_OK,
          "two controller steps elapse during inference");
    float out = 0.0f;
    uint64_t written = 0;
    CHECK(mode.poll() == nexus::ActionChunkState::kReady &&
              mode.active_index() == 2,
          "latency switch seats the chunk at the elapsed-step index");
    CHECK(mode.next_action(&out, sizeof(out), &written) ==
                  nexus::ActionChunkState::kReady && out == 30.0f,
          "latency switch skips the stale prefix");

    nexus::ActionChunkConfig cfg_off = cfg;
    cfg_off.switch_offset = -1;
    nexus::ActionChunkMode mode_off(&runner, cfg_off);
    probe.data = chunk;
    CHECK(mode_off.request() == CAP_OK, "offset fixture fires");
    (void)mode_off.advance_step(); (void)mode_off.advance_step();
    CHECK(mode_off.poll() == nexus::ActionChunkState::kReady &&
              mode_off.active_index() == 1,
          "switch_offset biases the seated index");

    nexus::ActionChunkMode mode_clip(&runner, cfg);
    probe.data = chunk;
    CHECK(mode_clip.request() == CAP_OK, "clip fixture fires");
    for (int i = 0; i < 7; ++i) (void)mode_clip.advance_step();
    CHECK(mode_clip.poll() == nexus::ActionChunkState::kReady &&
              mode_clip.active_index() == 2,
          "latency switch clips at the last index");
}

static void test_consume_switch_state() {
    Fixture fx(true);
    FloatProbe probe;
    fx.model.self = &probe;
    fx.model.get_output = float_get_output;
    nexus::StageDagRunner runner(fx.ctx, &fx.model);
    prime_context(runner);

    nexus::ActionChunkConfig cfg = chunk_config();
    cfg.consume_policy = nexus::kActionChunkConsumeSwitch;
    cfg.switch_mode = nexus::kActionChunkSwitchState;
    cfg.state_dim = 1;
    nexus::ActionChunkMode mode(&runner, cfg);
    const float absolute[3] = {10.0f, 20.0f, 30.0f};
    const float measured[1] = {21.0f};
    probe.data = absolute;
    g_event_pending = 0;
    CHECK(mode.set_state(measured, 1) == CAP_OK, "state feed accepted");
    CHECK(mode.request() == CAP_OK &&
              mode.poll() == nexus::ActionChunkState::kReady &&
              mode.active_index() == 1,
          "absolute state switch picks the closest action index");

    nexus::ActionChunkConfig cfg_delta = cfg;
    cfg_delta.action_representation = nexus::kActionChunkReprDeltaCumulative;
    nexus::ActionChunkMode mode_delta(&runner, cfg_delta);
    const float deltas[3] = {1.0f, 2.0f, 3.0f};
    const float at_fire[1] = {10.0f};   /* estimates: 11, 13, 16 */
    const float at_ready[1] = {14.0f};  /* distances: 3, 1, 2    */
    probe.data = deltas;
    CHECK(mode_delta.set_state(at_fire, 1) == CAP_OK &&
              mode_delta.request() == CAP_OK,
          "delta fixture snapshots the fire-time state");
    CHECK(mode_delta.set_state(at_ready, 1) == CAP_OK &&
              mode_delta.poll() == nexus::ActionChunkState::kReady &&
              mode_delta.active_index() == 1,
          "cumulative-delta state switch integrates from the fire snapshot");
}

/* ---- consume = temporal_fusion -------------------------------------------- */

static void test_consume_temporal_fusion() {
    Fixture fx(true);
    FloatProbe probe;
    fx.model.self = &probe;
    fx.model.get_output = float_get_output;
    nexus::StageDagRunner runner(fx.ctx, &fx.model);
    prime_context(runner);

    nexus::ActionChunkConfig cfg = chunk_config();
    cfg.consume_policy = nexus::kActionChunkConsumeTemporalFusion;
    cfg.scalar_dtype = nexus::kActionChunkDtypeF32;
    cfg.ring_slots = 4;
    cfg.fusion_decay = 0.0;   /* uniform weights: fused = mean */
    cfg.fusion_max_chunks = 3;
    nexus::ActionChunkMode mode(&runner, cfg);
    CHECK(mode.weight_table()[0] == 1.0 && mode.weight_table()[2] == 1.0,
          "decay 0 builds a uniform weight table");

    const float chunk1[3] = {0.0f, 1.0f, 2.0f};
    const float chunk2[3] = {100.0f, 101.0f, 102.0f};
    const float chunk3[3] = {200.0f, 201.0f, 202.0f};
    float out = 0.0f;
    uint64_t written = 0;

    probe.data = chunk1;
    g_event_pending = 0;
    CHECK(mode.request() == CAP_OK &&
              mode.poll() == nexus::ActionChunkState::kReady &&
              mode.active_index() == 0,
          "first chunk fuses with itself and seats at index 0");
    CHECK(mode.next_action(&out, sizeof(out), &written) ==
                  nexus::ActionChunkState::kReady && out == 0.0f,
          "single-chunk fusion is the identity");
    /* Second emission triggers the prefetch at grid step 2. */
    probe.data = chunk2;
    CHECK(mode.next_action(&out, sizeof(out), &written) ==
                  nexus::ActionChunkState::kReady && out == 1.0f &&
              mode.in_flight(),
          "prefetch fires the second chunk at the horizon");
    CHECK(mode.poll() == nexus::ActionChunkState::kReady &&
              mode.active_index() == 0,
          "second chunk seats at index 0 (no steps elapsed while pending)");
    const float* fused =
        reinterpret_cast<const float*>(mode.fused_chunk());
    CHECK(fused[0] == 51.0f && fused[1] == 101.0f && fused[2] == 102.0f,
          "overlap fuses to the uniform mean, tail stays raw");
    CHECK(mode.next_action(&out, sizeof(out), &written) ==
                  nexus::ActionChunkState::kReady && out == 51.0f,
          "consumption reads the fused values");

    /* Third chunk at grid step 4: chunk1 (steps 0..2) expires. */
    probe.data = chunk3;
    CHECK(mode.next_action(&out, sizeof(out), &written) ==
                  nexus::ActionChunkState::kReady && out == 101.0f &&
              mode.in_flight(),
          "prefetch fires the third chunk");
    CHECK(mode.poll() == nexus::ActionChunkState::kReady &&
              mode.pruned_chunks() == 1,
          "an expired chunk is pruned before fusion");
    fused = reinterpret_cast<const float*>(mode.fused_chunk());
    CHECK(fused[0] == 151.0f && fused[1] == 201.0f && fused[2] == 202.0f,
          "fusion window slides over the retained chunks");
}

/* ---- prepare = projected_state -------------------------------------------- */

static void test_prepare_projected_state() {
    Fixture fx(true);
    FloatProbe probe;
    fx.model.self = &probe;
    fx.model.set_input = float_set_input;
    fx.model.get_output = float_get_output;
    nexus::StageDagRunner runner(fx.ctx, &fx.model);
    prime_context(runner);

    nexus::ActionChunkConfig cfg = chunk_config();
    cfg.prepare_policy = nexus::kActionChunkPrepareProjectedState;
    cfg.scalar_dtype = nexus::kActionChunkDtypeF32;
    cfg.action_representation = nexus::kActionChunkReprDeltaCumulative;
    cfg.state_dim = 1;
    cfg.lookahead_steps = 2;
    cfg.execute_horizon = 0;
    CHECK(nexus::ActionChunkMode::validate(cfg) == CAP_OK,
          "projected_state with plain consume and delta actions is accepted");
    nexus::ActionChunkConfig bad = cfg;
    bad.consume_policy = nexus::kActionChunkConsumeSwitch;
    CHECK(nexus::ActionChunkMode::validate(bad) == CAP_ERR_ARG,
          "projected_state with switch consume is double compensation");
    bad = cfg;
    bad.action_representation = nexus::kActionChunkReprAbsolute;
    CHECK(nexus::ActionChunkMode::validate(bad) == CAP_ERR_ARG,
          "projected_state requires delta actions");
    bad = cfg;
    bad.state_input_port = 1;
    CHECK(nexus::ActionChunkMode::validate(bad) == CAP_OK &&
              nexus::ActionChunkMode::validate_model_ports(&runner, bad) ==
                  CAP_ERR_ARG,
          "projected state rejects a non-state producer port at setup");
    cfg.state_input_port = 2;
    CHECK(nexus::ActionChunkMode::validate_model_ports(&runner, cfg) == CAP_OK,
          "projected state accepts a matching STATE/F32/STAGED port");

    nexus::ActionChunkMode mode(&runner, cfg);
    CHECK(mode.begin_request() == CAP_ERR_ARG,
          "projection without a state feed is a defined error");

    const float measured0[1] = {10.0f};
    const float deltas_a[3] = {1.0f, 2.0f, 3.0f};
    float proj[1] = {0.0f};
    uint32_t dims = 0;
    float out = 0.0f;
    uint64_t written = 0;
    g_event_pending = 0;
    probe.data = deltas_a;
    CHECK(mode.set_state(measured0, 1) == CAP_OK &&
              mode.begin_request() == CAP_OK &&
              mode.projected_state(proj, 1, &dims) == CAP_OK &&
              dims == 1 && proj[0] == 10.0f && mode.projected_count() == 0 &&
              probe.state_writes == 1 && probe.last_state == 10.0f,
          "first projection is staged into the producer state port");
    CHECK(mode.commit_request() == CAP_OK &&
              mode.poll() == nexus::ActionChunkState::kReady &&
              mode.active_index() == 0,
          "first chunk seats at index 0 (d == k == 0)");
    CHECK(mode.next_action(&out, sizeof(out), &written) ==
                  nexus::ActionChunkState::kReady && out == 1.0f,
          "first chunk serves from its start step");

    /* Projection from the active chunk: index 1, lookahead 2 -> 2 + 3. */
    const float measured1[1] = {20.0f};
    const float deltas_b[3] = {10.0f, 20.0f, 30.0f};
    probe.data = deltas_b;
    CHECK(mode.set_state(measured1, 1) == CAP_OK &&
              mode.begin_request() == CAP_OK &&
              mode.projected_state(proj, 1, &dims) == CAP_OK &&
              proj[0] == 25.0f && mode.projected_count() == 2 &&
              probe.state_writes == 2 && probe.last_state == 25.0f,
          "projection integrates deltas and stages the producer input");
    /* d == k: fire at step 1, start = 3, land at step 3. */
    CHECK(mode.commit_request() == CAP_OK &&
              mode.advance_step() == CAP_OK && mode.advance_step() == CAP_OK,
          "the projected steps elapse during inference");
    CHECK(mode.poll() == nexus::ActionChunkState::kReady &&
              !mode.seated_waiting() && mode.active_index() == 0 &&
              mode.active_start_step() == 3,
          "d == k seats the projected chunk at index 0");
    CHECK(mode.next_action(&out, sizeof(out), &written) ==
                  nexus::ActionChunkState::kReady && out == 10.0f,
          "projected chunk serves from index 0");
}

static void test_projected_state_preserves_unmapped_dimension() {
    Fixture fx(true);
    FloatProbe probe;
    fx.model.self = &probe;
    fx.model.set_input = float_set_input;
    fx.model.get_output = float_get_output;
    nexus::StageDagRunner runner(fx.ctx, &fx.model);
    prime_context(runner);

    nexus::ActionChunkConfig cfg = chunk_config();
    cfg.prepare_policy = nexus::kActionChunkPrepareProjectedState;
    cfg.scalar_dtype = nexus::kActionChunkDtypeF32;
    cfg.action_representation = nexus::kActionChunkReprDeltaCumulative;
    cfg.state_dim = 2;
    cfg.lookahead_steps = 2;
    cfg.execute_horizon = 0;
    nexus::ActionChunkMode mode(&runner, cfg);
    const uint32_t map[2] = {0, UINT32_MAX};
    CHECK(mode.set_state_action_indices(map, 2) == CAP_OK,
          "state-action map accepts an unprojected state dimension");

    const float measured0[2] = {10.0f, 99.0f};
    const float deltas[3] = {1.0f, 2.0f, 3.0f};
    probe.data = deltas;
    g_event_pending = 0;
    CHECK(mode.set_state(measured0, 2) == CAP_OK &&
              mode.request() == CAP_OK &&
              mode.poll() == nexus::ActionChunkState::kReady,
          "projection fixture seats the first chunk");

    const float measured1[2] = {20.0f, 88.0f};
    float projected[2] = {};
    uint32_t written = 0;
    CHECK(mode.set_state(measured1, 2) == CAP_OK &&
              mode.begin_request() == CAP_OK &&
              mode.projected_state(projected, 2, &written) == CAP_OK &&
              written == 2 && projected[0] == 23.0f &&
              projected[1] == 88.0f,
          "projection integrates mapped deltas and preserves unmapped state");
}

static void test_projected_seating_late_and_wait() {
    Fixture fx(true);
    FloatProbe probe;
    fx.model.self = &probe;
    fx.model.get_output = float_get_output;
    nexus::StageDagRunner runner(fx.ctx, &fx.model);
    prime_context(runner);

    nexus::ActionChunkConfig cfg = chunk_config();
    cfg.prepare_policy = nexus::kActionChunkPrepareProjectedState;
    cfg.scalar_dtype = nexus::kActionChunkDtypeF32;
    cfg.action_representation = nexus::kActionChunkReprDeltaCumulative;
    cfg.state_dim = 1;
    cfg.lookahead_steps = 2;
    cfg.execute_horizon = 0;

    const float measured[1] = {0.0f};
    const float deltas_a[3] = {1.0f, 2.0f, 3.0f};
    const float deltas_b[3] = {10.0f, 20.0f, 30.0f};
    float out = 0.0f;
    uint64_t written = 0;

    /* Late landing: d > k skips the stale prefix. */
    nexus::ActionChunkMode late(&runner, cfg);
    g_event_pending = 0;
    probe.data = deltas_a;
    (void)late.set_state(measured, 1);
    CHECK(late.request() == CAP_OK &&
              late.poll() == nexus::ActionChunkState::kReady &&
              late.next_action(&out, sizeof(out), &written) ==
                  nexus::ActionChunkState::kReady,
          "late fixture serves its first chunk");
    probe.data = deltas_b;
    CHECK(late.request() == CAP_OK, "late fixture fires with k = 2");
    for (int i = 0; i < 4; ++i) (void)late.advance_step();
    CHECK(late.poll() == nexus::ActionChunkState::kReady &&
              late.active_index() == 2 && !late.seated_waiting(),
          "d > k seats at index d - k");

    /* Early landing: d < k waits; consumption switches exactly at the
     * start step, never earlier. */
    nexus::ActionChunkMode wait(&runner, cfg);
    probe.data = deltas_a;
    (void)wait.set_state(measured, 1);
    CHECK(wait.request() == CAP_OK &&
              wait.poll() == nexus::ActionChunkState::kReady &&
              wait.next_action(&out, sizeof(out), &written) ==
                  nexus::ActionChunkState::kReady && out == 1.0f,
          "wait fixture consumes one action of the first chunk");
    probe.data = deltas_b;
    CHECK(wait.request() == CAP_OK && wait.advance_step() == CAP_OK,
          "wait fixture fires with k = 2, one step elapses");
    CHECK(wait.poll() == nexus::ActionChunkState::kReady &&
              wait.seated_waiting() && wait.has_active_chunk(),
          "d < k parks the chunk as waiting; the old chunk keeps serving");
    CHECK(wait.request() == CAP_ERR_ARG,
          "no new fire while a seated chunk waits");
    CHECK(wait.next_action(&out, sizeof(out), &written) ==
                  nexus::ActionChunkState::kReady && out == 2.0f &&
              wait.seated_waiting(),
          "the old chunk serves the steps before the projected start");
    CHECK(wait.next_action(&out, sizeof(out), &written) ==
                  nexus::ActionChunkState::kReady && out == 10.0f &&
              !wait.seated_waiting(),
          "consumption switches to the projected chunk exactly at its start");

    /* Waiting overshoot: the grid may pass the start step via advance_step;
     * promotion then clips into the chunk instead of switching early. */
    nexus::ActionChunkMode over(&runner, cfg);
    probe.data = deltas_a;
    (void)over.set_state(measured, 1);
    CHECK(over.request() == CAP_OK &&
              over.poll() == nexus::ActionChunkState::kReady &&
              over.next_action(&out, sizeof(out), &written) ==
                  nexus::ActionChunkState::kReady,
          "overshoot fixture consumes one action");
    probe.data = deltas_b;
    CHECK(over.request() == CAP_OK &&
              over.poll() == nexus::ActionChunkState::kReady &&
              over.seated_waiting(),
          "overshoot fixture parks a waiting chunk (d = 0 < k)");
    for (int i = 0; i < 4; ++i) (void)over.advance_step();
    CHECK(over.next_action(&out, sizeof(out), &written) ==
                  nexus::ActionChunkState::kReady && out == 30.0f &&
              !over.seated_waiting(),
          "promotion after overshoot clips into the projected chunk");
}

/* ---- prepare = prev_chunk_prefix ------------------------------------------ */

struct DualProbe {
    const float* actions = nullptr;       /* port 0: 3 x f32 robot actions */
};

static int dual_get_output(void* p, uint32_t port, void* out,
                           uint64_t capacity, uint64_t* written, int) {
    if (port != 0) return CAP_ERR_ARG;    /* the raw port is a SWAP window */
    auto* probe = static_cast<DualProbe*>(p);
    if (written) *written = 12;
    if (capacity < 12) return CAP_ERR_ARG;
    std::memcpy(out, probe->actions, 12);
    return CAP_OK;
}

static void test_prepare_prev_chunk_prefix() {
    Fixture fx(true);
    DualProbe probe;
    int64_t raw_shape[2] = {3, 1};
    unsigned char raw_window[6] = {};     /* the SWAP window the graph fills */
    cap_buffer raw_buf = fx.be.buffer_wrap(fx.be.self, "actions_raw",
                                           raw_window, sizeof(raw_window), 0);
    cap_model_port ports2[2]{};
    ports2[0] = fx.ports[0];
    ports2[1].name = "actions_raw";
    ports2[1].direction = 1;
    ports2[1].update = 0;                 /* SWAP: window read, no verb */
    ports2[1].shape = raw_shape;
    ports2[1].rank = 2;
    ports2[1].buffer = raw_buf;
    ports2[1].bytes = sizeof(raw_window);
    fx.model.ports = ports2;
    fx.model.n_ports = 2;
    fx.model.self = &probe;
    fx.model.get_output = dual_get_output;
    nexus::StageDagRunner runner(fx.ctx, &fx.model);
    prime_context(runner);

    nexus::ActionChunkConfig cfg = chunk_config();
    cfg.prepare_policy = nexus::kActionChunkPreparePrevChunkPrefix;
    cfg.consume_policy = nexus::kActionChunkConsumeSwitch;
    cfg.execute_horizon = 0;
    cfg.prefix_len = 2;
    cfg.raw_out_port = 2;        /* +1 encoded: port index 1 */
    cfg.raw_action_bytes = 2;
    CHECK(nexus::ActionChunkMode::validate(cfg) == CAP_OK,
          "prev_chunk_prefix with latency switch is accepted");
    nexus::ActionChunkConfig bad = cfg;
    bad.consume_policy = nexus::kActionChunkConsumePlain;
    CHECK(nexus::ActionChunkMode::validate(bad) == CAP_ERR_ARG,
          "prev_chunk_prefix requires the latency switch consume");
    bad = cfg;
    bad.prefix_len = 4;
    CHECK(nexus::ActionChunkMode::validate(bad) == CAP_ERR_ARG,
          "prefix_len beyond the chunk length is rejected");
    bad = cfg;
    bad.raw_out_port = 0;
    CHECK(nexus::ActionChunkMode::validate(bad) == CAP_ERR_ARG,
          "the raw output port is required");
    bad = cfg;
    bad.prev_chunk_port = 1;
    CHECK(nexus::ActionChunkMode::validate(bad) == CAP_ERR_ARG,
          "SWAP prev-chunk transport is not implemented yet and is rejected");

    nexus::ActionChunkMode mode(&runner, cfg);
    const float robot[3] = {1.0f, 2.0f, 3.0f};
    const unsigned char raw_a[6] = {0xA1, 0xA2, 0xB1, 0xB2, 0xC1, 0xC2};
    const unsigned char raw_b[6] = {0x11, 0x12, 0x21, 0x22, 0x31, 0x32};
    unsigned char staged[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    const unsigned char zeros[6] = {0, 0, 0, 0, 0, 0};
    uint64_t staged_bytes = 0;
    float out = 0.0f;
    uint64_t written = 0;

    probe.actions = robot;
    std::memcpy(raw_window, raw_a, sizeof(raw_window));
    g_event_pending = 0;
    CHECK(mode.begin_request() == CAP_OK &&
              mode.prev_chunk_staged(staged, sizeof(staged),
                                     &staged_bytes) == CAP_OK &&
              staged_bytes == 6 && std::memcmp(staged, zeros, 6) == 0,
          "cold start stages a zero prefix");
    CHECK(mode.commit_request() == CAP_OK &&
              mode.poll() == nexus::ActionChunkState::kReady &&
              mode.active_index() == 0,
          "first chunk seats at the latency index and retains its raw twin");
    CHECK(mode.next_action(&out, sizeof(out), &written) ==
                  nexus::ActionChunkState::kReady && out == 1.0f,
          "prefix fixture consumes one action");

    const unsigned char expected_j1[6] = {0xB1, 0xB2, 0xC1, 0xC2, 0xC1, 0xC2};
    std::memcpy(raw_window, raw_b, sizeof(raw_window));
    CHECK(mode.begin_request() == CAP_OK &&
              mode.prev_chunk_staged(staged, sizeof(staged),
                                     &staged_bytes) == CAP_OK &&
              std::memcmp(staged, expected_j1, 6) == 0,
          "staging re-indexes the raw chunk by the consumption position");
    CHECK(mode.commit_request() == CAP_OK && mode.advance_step() == CAP_OK &&
              mode.poll() == nexus::ActionChunkState::kReady &&
              mode.active_index() == 1,
          "the prefixed chunk seats at the elapsed-step index");

    /* Exhaust the chunk: staging then repeats the last raw row. */
    while (mode.has_active_chunk())
        (void)mode.next_action(&out, sizeof(out), &written);
    const unsigned char expected_end[6] = {0x31, 0x32, 0x31, 0x32, 0x31, 0x32};
    CHECK(mode.begin_request() == CAP_OK &&
              mode.prev_chunk_staged(staged, sizeof(staged),
                                     &staged_bytes) == CAP_OK &&
              std::memcmp(staged, expected_end, 6) == 0,
          "staging after exhaustion repeats the last raw action");
}

/* ---- experimental: projected_state + temporal_fusion ---------------------- */

static void test_composed_projected_fusion() {
    Fixture fx(true);
    FloatProbe probe;
    fx.model.self = &probe;
    fx.model.get_output = float_get_output;
    nexus::StageDagRunner runner(fx.ctx, &fx.model);
    prime_context(runner);

    nexus::ActionChunkConfig cfg = chunk_config();
    cfg.prepare_policy = nexus::kActionChunkPrepareProjectedState;
    cfg.consume_policy = nexus::kActionChunkConsumeTemporalFusion;
    cfg.scalar_dtype = nexus::kActionChunkDtypeF32;
    cfg.action_representation = nexus::kActionChunkReprDeltaCumulative;
    cfg.state_dim = 1;
    cfg.lookahead_steps = 1;
    cfg.ring_slots = 4;
    cfg.fusion_decay = 0.0;
    cfg.fusion_max_chunks = 3;
    cfg.execute_horizon = 0;
    CHECK(nexus::ActionChunkMode::validate(cfg) == CAP_ERR_ARG,
          "the composed pairing is rejected without the experimental flag");
    cfg.experimental = 1;
    nexus::ActionChunkConfig bad = cfg;
    bad.switch_mode = nexus::kActionChunkSwitchState;
    CHECK(nexus::ActionChunkMode::validate(bad) == CAP_ERR_ARG,
          "the composed pairing allows only the latency switch");
    CHECK(nexus::ActionChunkMode::validate(cfg) == CAP_OK,
          "the experimental flag unlocks projected_state + fusion");

    const float measured0[1] = {10.0f};
    const float measured1[1] = {20.0f};
    const float deltas_a[3] = {1.0f, 2.0f, 3.0f};
    const float deltas_b[3] = {10.0f, 20.0f, 30.0f};
    float proj[1] = {0.0f};
    uint32_t dims = 0;
    float out = 0.0f;
    uint64_t written = 0;

    /* d == k: fuse at ready, seat at index d - k = 0. */
    nexus::ActionChunkMode mode(&runner, cfg);
    g_event_pending = 0;
    probe.data = deltas_a;
    CHECK(mode.set_state(measured0, 1) == CAP_OK &&
              mode.request() == CAP_OK &&
              mode.poll() == nexus::ActionChunkState::kReady &&
              mode.active_index() == 0,
          "composed first chunk fuses with itself and seats at 0");
    CHECK(mode.next_action(&out, sizeof(out), &written) ==
                  nexus::ActionChunkState::kReady && out == 1.0f,
          "composed fixture consumes one fused action");
    CHECK(mode.set_state(measured1, 1) == CAP_OK &&
              mode.begin_request() == CAP_OK &&
              mode.projected_state(proj, 1, &dims) == CAP_OK &&
              proj[0] == 22.0f && mode.projected_count() == 1,
          "composed projection integrates the fused values");
    probe.data = deltas_b;
    CHECK(mode.commit_request() == CAP_OK && mode.advance_step() == CAP_OK &&
              mode.poll() == nexus::ActionChunkState::kReady &&
              !mode.seated_waiting() && mode.active_index() == 0,
          "composed d == k fuses at ready and seats at index 0");
    const float* fused = reinterpret_cast<const float*>(mode.fused_chunk());
    CHECK(fused[0] == 6.5f && fused[1] == 20.0f && fused[2] == 30.0f,
          "the projected anchor drives the fusion window");

    /* d < k: fusion is deferred; the old fused chunk serves the wait. */
    nexus::ActionChunkMode wait(&runner, cfg);
    probe.data = deltas_a;
    CHECK(wait.set_state(measured0, 1) == CAP_OK &&
              wait.request() == CAP_OK &&
              wait.poll() == nexus::ActionChunkState::kReady &&
              wait.next_action(&out, sizeof(out), &written) ==
                  nexus::ActionChunkState::kReady && out == 1.0f,
          "composed wait fixture consumes one action");
    CHECK(wait.set_state(measured1, 1) == CAP_OK &&
              wait.request() == CAP_OK,
          "composed wait fixture fires with k = 1");
    probe.data = deltas_b;
    CHECK(wait.poll() == nexus::ActionChunkState::kReady &&
              wait.seated_waiting(),
          "an early landing parks without fusing");
    CHECK(wait.next_action(&out, sizeof(out), &written) ==
                  nexus::ActionChunkState::kReady && out == 2.0f &&
              wait.seated_waiting(),
          "the previous fused chunk serves the waiting window intact");
    CHECK(wait.next_action(&out, sizeof(out), &written) ==
                  nexus::ActionChunkState::kReady && out == 6.5f &&
              !wait.seated_waiting(),
          "deferred fusion runs exactly at the start step");
}

/* ---- determinism / replay ------------------------------------------------ */

struct ScriptResult {
    std::string bytes;
    uint64_t action_step = 0;
    uint64_t emitted = 0;
    uint64_t completed = 0;
    int fallbacks = 0;
    std::vector<int> states;
};

static ScriptResult run_script() {
    Fixture fx(true);
    nexus::StageDagRunner runner(fx.ctx, &fx.model);
    nexus::ActionChunkConfig cfg = chunk_config();
    cfg.deadline_steps = 2;
    nexus::ActionChunkMode mode(&runner, cfg);
    prime_context(runner);
    ScriptResult r;
    unsigned char out[4] = {};
    uint64_t written = 0;
    auto poll = [&]() { r.states.push_back(static_cast<int>(mode.poll())); };
    auto next = [&]() {
        nexus::ActionChunkState s = mode.next_action(out, sizeof(out),
                                                     &written);
        r.states.push_back(static_cast<int>(s));
        if (s == nexus::ActionChunkState::kReady && mode.has_active_chunk())
            r.bytes.append(reinterpret_cast<char*>(out), sizeof(out));
    };
    g_event_pending = 2;
    (void)mode.request();
    poll(); poll(); poll();          /* pending, pending, ready */
    g_event_pending = 3;
    next(); next(); next();          /* consume + prefetch at horizon */
    (void)mode.advance_step();
    poll(); poll();
    next();
    poll(); poll();
    r.action_step = mode.action_step();
    r.emitted = mode.emitted_actions();
    r.completed = mode.completed_chunks();
    r.fallbacks = mode.fallbacks();
    return r;
}

static void test_determinism_replay() {
    ScriptResult a = run_script();
    ScriptResult b = run_script();
    CHECK(a.bytes == b.bytes && a.states == b.states &&
              a.action_step == b.action_step && a.emitted == b.emitted &&
              a.completed == b.completed && a.fallbacks == b.fallbacks,
          "identical verb sequences replay to identical outputs and counters");
    CHECK(a.emitted >= 3 && a.completed >= 1,
          "replay script exercises emission and completion");
}

/* ---- C ABI verb smoke ----------------------------------------------------- */

static void test_c_abi_new_verbs() {
    Fixture fx(false);
    nexus_stage_dag* dag = nullptr;
    CHECK(nexus_stage_dag_create(fx.ctx, &fx.model, &dag) == CAP_OK && dag,
          "C ABI fixture creates a stage DAG");
    CHECK(nexus_stage_dag_fire(dag, 0) == CAP_OK,
          "C ABI fixture fires the context stage");
    nexus_action_chunk_config cfg{};
    cfg.struct_size = sizeof(cfg);
    cfg.action_stage = 1;
    cfg.output_port = 0;
    cfg.chunk_length = 3;
    cfg.action_bytes = 4;
    cfg.ring_slots = 2;
    cfg.execute_horizon = 1;
    cfg.poll_budget = -1;
    cfg.deadline_steps = -1;
    cfg.state_dim = 2;
    nexus_action_chunk* h = nullptr;
    CHECK(nexus_action_chunk_create(dag, &cfg, &h) == CAP_OK && h,
          "C ABI creates a v2-configured mode");
    const float st[2] = {1.0f, 2.0f};
    CHECK(nexus_action_chunk_set_state(h, st, 2) == CAP_OK &&
              nexus_action_chunk_state_updates(h) == 1,
          "C ABI state feed works");
    CHECK(nexus_action_chunk_begin_request(h) == CAP_OK &&
              nexus_action_chunk_commit_request(h) == CAP_OK &&
              nexus_action_chunk_prepared_requests(h) == 1,
          "C ABI two-phase request works");
    CHECK(nexus_action_chunk_poll(h) == NEXUS_AC_READY,
          "C ABI prepared fire completes");
    CHECK(nexus_action_chunk_advance_step(h) == CAP_OK &&
              nexus_action_chunk_action_step(h) == 1,
          "C ABI grid verbs work");
    nexus_action_chunk_destroy(h);
    nexus_stage_dag_destroy(dag);
}

int main() {
    test_config_versioning();
    test_two_phase_request();
    test_dual_clocks();
    test_hold_last();
    test_sync_next_chunk();
    test_state_feed();
    test_policy_validation();
    test_consume_switch_latency();
    test_consume_switch_state();
    test_consume_temporal_fusion();
    test_prepare_projected_state();
    test_projected_state_preserves_unmapped_dimension();
    test_projected_seating_late_and_wait();
    test_prepare_prev_chunk_prefix();
    test_composed_projected_fusion();
    test_determinism_replay();
    test_c_abi_new_verbs();
    std::printf(g_fail ? "\n== ACTION CHUNK TEST FAILED ==\n"
                       : "\n== ACTION CHUNK TEST PASSED ==\n");
    return g_fail;
}
