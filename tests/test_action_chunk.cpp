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
    cap_model_port ports[1]{};
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
        model.backend = &be;
        model.ports = ports;
        model.n_ports = 1;
        model.stages = stages;
        model.n_stages = 2;
        model.schedule = schedule;
        model.self = &chunks;
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

    const uint32_t dup[2] = {1, 1};
    const uint32_t idx[2] = {0, 2};
    CHECK(mode.set_state_action_indices(dup, 2) == CAP_ERR_ARG,
          "duplicate state-action indices are rejected");
    CHECK(mode.set_state_action_indices(idx, 2) == CAP_OK,
          "state-action indices are accepted before the first request");
    CHECK(mode.request() == CAP_OK, "state fixture fires");
    CHECK(mode.set_state_action_indices(idx, 2) == CAP_ERR_ARG,
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
    test_determinism_replay();
    test_c_abi_new_verbs();
    std::printf(g_fail ? "\n== ACTION CHUNK TEST FAILED ==\n"
                       : "\n== ACTION CHUNK TEST PASSED ==\n");
    return g_fail;
}
