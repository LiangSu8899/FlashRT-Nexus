/* test_nexus_l2.cpp — L2 scheduler/state/mode skeletons. */
#include "nexus/modes/action_chunk/action_chunk.h"
#include "nexus/modes/action_chunk/action_chunk_c.h"
#include "nexus/modes/action_chunk/rtc_action_chunk_compat.h"
#include "nexus/schedulers/stage_dag_c.h"
#include "nexus/state/graph_store.h"

#include "stub_backend.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::printf("FAIL: %s\n", (msg)); g_fail = 1; } \
    else { std::printf("ok  : %s\n", (msg)); } \
} while (0)

struct ReplayLog {
    std::vector<int> order;
    int context_value = 0;
    std::vector<int> action_seen;
};

static void fire0(void* p) {
    auto* log = static_cast<ReplayLog*>(p);
    log->order.push_back(0);
    ++log->context_value;
}
static void fire1(void* p) {
    auto* log = static_cast<ReplayLog*>(p);
    log->order.push_back(1);
    log->action_seen.push_back(log->context_value);
}

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

static cap_event async_event(void*) {
    return reinterpret_cast<cap_event>(new AsyncEvent());
}
static int async_event_record(void*, cap_event ev, int) {
    auto* e = reinterpret_cast<AsyncEvent*>(ev);
    e->remaining = g_event_pending;
    e->recorded = true;
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
static int async_sync(void*, int) { return CAP_OK; }
static void async_event_free(void*, cap_event ev) {
    delete reinterpret_cast<AsyncEvent*>(ev);
}

struct StoreProbe {
    std::map<cap_graph, size_t> counts;
    int evict_lru = 0;
    int evict_one = 0;
};

static StoreProbe g_store_probe;

static int evict_one(cap_backend*, cap_graph g, cap_shape_key) {
    ++g_store_probe.evict_one;
    if (g_store_probe.counts[g]) --g_store_probe.counts[g];
    return CAP_OK;
}

static int evict_lru(cap_backend*, cap_graph g) {
    ++g_store_probe.evict_lru;
    if (g_store_probe.counts[g]) --g_store_probe.counts[g];
    return CAP_OK;
}

static size_t variant_count(cap_backend*, cap_graph g) {
    return g_store_probe.counts[g];
}

int main() {
    cap_backend be{};
    stub_backend_init(&be, 0x1234);
    cap_ctx ctx = cap_ctx_create(&be);

    ReplayLog log;
    cap_graph g0 = stub_graph_make(fire0, &log);
    cap_graph g1 = stub_graph_make(fire1, &log);

    uint32_t after1[1] = {0};
    cap_model_stage model_stages[2]{};
    model_stages[0].name = "context";
    model_stages[0].graph = g0;
    model_stages[0].key = 0;
    model_stages[0].stream = 0;
    model_stages[1].name = "action";
    model_stages[1].graph = g1;
    model_stages[1].key = 0;
    model_stages[1].stream = 1;
    model_stages[1].after = after1;
    model_stages[1].n_after = 1;

    cap_stage schedule_stages[2]{};
    schedule_stages[0] = {g0, 0, 0, 0, 1, 1, CAP_EVERY};
    schedule_stages[1] = {g1, 0, 1, 0, 1, 1, CAP_EVERY};
    int deps[1][2] = {{1, 0}};
    cap_schedule schedule{schedule_stages, 2, deps, 1};

    cap_model_runtime model{};
    model.backend = &be;
    const int64_t action_shape[2] = {3, 1};
    cap_model_port model_ports[1]{};
    model_ports[0].name = "actions";
    model_ports[0].direction = CAP_MODEL_PORT_OUT;
    model_ports[0].update = CAP_MODEL_PORT_STAGED;
    model_ports[0].shape = action_shape;
    model_ports[0].rank = 2;
    model.ports = model_ports;
    model.n_ports = 1;
    model.stages = model_stages;
    model.n_stages = 2;
    model.schedule = schedule;
    ChunkProbe chunks;
    model.self = &chunks;
    model.get_output = chunk_get_output;

    nexus::StageDagRunner runner(ctx, &model);
    CHECK(runner.ok(), "StageDagRunner constructs and pre-creates events");
    CHECK(runner.run_once() == CAP_OK, "StageDagRunner runs a two-stage DAG");
    CHECK(log.order.size() == 2 && log.order[0] == 0 && log.order[1] == 1,
          "StageDagRunner preserves declared order");
    CHECK(runner.query(1) == 0, "StageDagRunner exposes non-blocking query");

    log.order.clear();
    log.action_seen.clear();
    CHECK(runner.run_mask(1ull << 1) == CAP_OK &&
              log.order.size() == 1 && log.order[0] == 1,
          "StageDagRunner runs an explicit stage mask");

    log.order.clear();
    log.action_seen.clear();
    log.context_value = 0;
    const uint32_t periods[2] = {4, 1};
    const uint32_t phases[2] = {0, 0};
    bool due_ok = true;
    for (uint64_t tick = 0; tick < 5; ++tick)
        due_ok = due_ok &&
                 runner.run_due(tick, periods, phases, 2) == CAP_OK;
    CHECK(due_ok && log.action_seen.size() == 5 &&
              log.action_seen[0] == 1 && log.action_seen[1] == 1 &&
              log.action_seen[2] == 1 && log.action_seen[3] == 1 &&
              log.action_seen[4] == 2,
          "StageDagRunner supports 1:4 stage frequency with stale context");

    log.order.clear();
    nexus::ActionChunkMode mode(&runner, 1, 0);
    CHECK(mode.request() == CAP_OK && mode.in_flight(),
          "action-chunk mode fires the action stage");
    CHECK(mode.poll() == nexus::ActionChunkState::kReady &&
              !mode.in_flight() && mode.completed_chunks() == 1,
          "action-chunk mode reports ready completion");
    CHECK(log.order.size() == 1 && log.order[0] == 1,
          "action-chunk mode does not run unrelated stages");

    nexus::ActionChunkConfig chunk_cfg{};
    CHECK(nexus::ActionChunkMode::config_from_output_port(
              &runner, 1, 0, 4, 2, 1, 4, &chunk_cfg) == CAP_OK &&
              chunk_cfg.chunk_length == 3 && chunk_cfg.action_bytes == 4,
          "action-chunk config infers action chunk shape from the output port");
    const int64_t groot_action_shape[2] = {50, 7};
    model_ports[0].shape = groot_action_shape;
    nexus::ActionChunkConfig groot_cfg{};
    CHECK(nexus::ActionChunkMode::config_from_output_port(
              &runner, 1, 0, 4, 2, 4, 100, &groot_cfg) == CAP_OK &&
              groot_cfg.chunk_length == 50 && groot_cfg.action_bytes == 28,
          "action-chunk config supports larger VLA chunks such as 50x7");
    model_ports[0].shape = action_shape;
    nexus::ActionChunkMode chunk_mode(&runner, chunk_cfg);
    CHECK(chunk_mode.request() == CAP_OK &&
              chunk_mode.poll() == nexus::ActionChunkState::kReady &&
              chunk_mode.has_active_chunk(),
          "action-chunk mode captures a completed action chunk");
    unsigned char action[4] = {};
    uint64_t written = 0;
    CHECK(chunk_mode.next_action(action, sizeof(action), &written) ==
              nexus::ActionChunkState::kReady &&
              written == sizeof(action) && action[0] == 'A' &&
              chunk_mode.remaining_actions() == 2,
          "action-chunk mode emits one action at a time");
    CHECK(chunk_mode.next_action(action, sizeof(action), &written) ==
              nexus::ActionChunkState::kReady &&
              chunk_mode.in_flight(),
          "action-chunk mode prefetches at the execute horizon");
    CHECK(chunk_mode.poll() == nexus::ActionChunkState::kReady &&
              chunk_mode.completed_chunks() == 2,
          "action-chunk mode accepts the prefetched chunk");

    nexus_stage_dag* c_runner = nullptr;
    CHECK(nexus_stage_dag_create(ctx, &model, &c_runner) == CAP_OK &&
              c_runner,
          "StageDAG C ABI creates a runner");
    CHECK(nexus_stage_dag_fire(c_runner, 0) == CAP_OK &&
              nexus_stage_dag_has_event(c_runner, 0) == 1,
          "StageDAG C ABI fires a producer-owned context stage");
    log.order.clear();
    CHECK(nexus_stage_dag_run_mask(c_runner, 1ull << 1) == CAP_OK &&
              log.order.size() == 1 && log.order[0] == 1,
          "StageDAG C ABI runs an explicit stage mask");
    log.order.clear();
    CHECK(nexus_stage_dag_run_due(c_runner, 8, periods, phases, 2) == CAP_OK &&
              log.order.size() == 2 && log.order[0] == 0 &&
              log.order[1] == 1,
          "StageDAG C ABI runs due stages from frequency tables");
    nexus_action_chunk* c_chunk = nullptr;
    CHECK(nexus_action_chunk_create_for_output_port(
              c_runner, 1, 0, 4, 2, 1, 4, &c_chunk) == CAP_OK && c_chunk,
          "action-chunk C ABI creates an action-chunk mode from an output port");
    CHECK(nexus_action_chunk_request(c_chunk) == CAP_OK &&
              nexus_action_chunk_poll(c_chunk) == NEXUS_AC_READY,
          "action-chunk C ABI requests and accepts a chunk");
    CHECK(nexus_action_chunk_next_action(c_chunk, action, sizeof(action),
                                             &written) == NEXUS_AC_READY &&
              written == sizeof(action) &&
              nexus_action_chunk_remaining(c_chunk) == 2,
          "action-chunk C ABI emits one action");
    nexus_action_chunk_destroy(c_chunk);

    /* Deprecated pre-rename aliases must drive the same mode end to end. */
    nexus_rtc_action_chunk* c_compat = nullptr;
    CHECK(nexus_rtc_action_chunk_create_for_output_port(
              c_runner, 1, 0, 4, 2, 1, 4, &c_compat) == CAP_OK && c_compat,
          "compat aliases create the renamed action-chunk mode");
    CHECK(nexus_rtc_action_chunk_request(c_compat) == CAP_OK &&
              nexus_rtc_action_chunk_poll(c_compat) == NEXUS_RTC_READY,
          "compat aliases drive request/poll");
    CHECK(nexus_rtc_action_chunk_next_action(c_compat, action, sizeof(action),
                                             &written) == NEXUS_RTC_READY &&
              written == sizeof(action) &&
              nexus_rtc_action_chunk_remaining(c_compat) == 2,
          "compat aliases emit one action");
    nexus_rtc_action_chunk_destroy(c_compat);
    nexus_stage_dag_destroy(c_runner);

    cap_backend be_async{};
    stub_backend_init(&be_async, 0x1234);
    be_async.event = async_event;
    be_async.event_record = async_event_record;
    be_async.event_query = async_event_query;
    be_async.stream_wait = async_stream_wait;
    be_async.sync = async_sync;
    be_async.event_free = async_event_free;
    cap_ctx ctx_async = cap_ctx_create(&be_async);
    cap_model_runtime async_model = model;
    async_model.backend = &be_async;
    nexus::StageDagRunner async_runner(ctx_async, &async_model);
    g_event_pending = 3;
    CHECK(async_runner.fire(0) == CAP_OK,
          "async runner can fire the context stage");
    nexus::ActionChunkConfig deadline_cfg{};
    deadline_cfg.action_stage = 1;
    deadline_cfg.poll_budget = 1;
    nexus::ActionChunkMode deadline_mode(&async_runner, deadline_cfg);
    CHECK(deadline_mode.request() == CAP_OK &&
              deadline_mode.poll() == nexus::ActionChunkState::kPending &&
              deadline_mode.poll() == nexus::ActionChunkState::kFallback &&
              deadline_mode.in_flight() && deadline_mode.fallbacks() == 1,
          "action-chunk deadline reports fallback without cancelling in-flight work");
    CHECK(deadline_mode.poll() == nexus::ActionChunkState::kReady &&
              !deadline_mode.in_flight() &&
              deadline_mode.late_chunks() == 1 &&
              deadline_mode.last_ready_ticks() == 2 &&
              deadline_mode.max_ready_ticks() == 2,
          "action-chunk mode accepts and accounts for a late chunk after fallback");
    cap_ctx_destroy(ctx_async);
    stub_backend_fini(&be_async);

    nexus::GraphStore::Ops ops{evict_one, evict_lru, variant_count};
    nexus::GraphStore store(&be, ops);
    g_store_probe.counts[g0] = 4;
    CHECK(store.add_graph("context", g0, 2) == CAP_OK,
          "GraphStore registers a graph budget");
    CHECK(store.variant_count(0) == 4, "GraphStore reads backend count");
    CHECK(store.enforce_budget(0) == CAP_OK && g_store_probe.counts[g0] == 2 &&
              g_store_probe.evict_lru == 2,
          "GraphStore evicts LRU until under budget");
    CHECK(store.evict(0, 7) == CAP_OK && g_store_probe.evict_one == 1,
          "GraphStore can evict an explicit key");

    stub_graph_free(g0);
    stub_graph_free(g1);
    cap_ctx_destroy(ctx);
    stub_backend_fini(&be);
    std::printf(g_fail ? "\n== NEXUS L2 TEST FAILED ==\n"
                       : "\n== NEXUS L2 TEST PASSED ==\n");
    return g_fail;
}
