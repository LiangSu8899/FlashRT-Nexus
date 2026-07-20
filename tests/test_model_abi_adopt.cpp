#include "capsule/model_runtime.h"
#include "flashrt_model_abi_adapter.h"
#include "model_executor_internal.h"
#include "nexus/embedded/session.h"
#include "nexus/schedulers/stage_dag.h"

#include <cstddef>
#include <cstdio>
#include <cstring>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::printf("FAIL: %s\n", (msg)); g_fail = 1; } \
    else { std::printf("ok  : %s\n", (msg)); } \
} while (0)

namespace {

struct Fixture {
    int retains = 0;
    int releases = 0;
    int step_calls = 0;
    int stage_calls = 0;
    int callback_rc = 0;
    uint32_t refs[4] = {};
    float input = 0.0f;
    float output = 0.0f;
    const frt_generic_stage_plan_ext_v1* plan = nullptr;
};

void retain(void* self) { static_cast<Fixture*>(self)->retains++; }
void release(void* self) { static_cast<Fixture*>(self)->releases++; }
int set_input(void* self, uint32_t port, const void* data, uint64_t bytes,
              int) {
    if (port != 0 || !data || bytes != sizeof(float)) return -1;
    std::memcpy(&static_cast<Fixture*>(self)->input, data, sizeof(float));
    return 0;
}
int get_output(void* self, uint32_t port, void* out, uint64_t capacity,
               uint64_t* written, int) {
    if (written) *written = sizeof(float);
    if (port != 1 || !out || capacity < sizeof(float)) return -5;
    std::memcpy(out, &static_cast<Fixture*>(self)->output, sizeof(float));
    return 0;
}
int step(void* self) {
    auto* fixture = static_cast<Fixture*>(self);
    fixture->step_calls++;
    fixture->output = fixture->input + 10.0f;
    return 0;
}
const char* last_error(void*) { return "fixture error"; }
int run_opaque(void* self, uint32_t ref) {
    auto* fixture = static_cast<Fixture*>(self);
    fixture->refs[fixture->stage_calls++] = ref;
    if (ref == 91) fixture->output = fixture->input + 20.0f;
    return fixture->callback_rc;
}
int query_extension(const frt_model_runtime_v1* runtime, uint64_t id,
                    uint32_t min_version, const void** out) {
    if (!out) return -1;
    *out = nullptr;
    auto* fixture = static_cast<Fixture*>(runtime->self);
    if (id != FRT_EXT_GENERIC_STAGE_PLAN_V1 || min_version > 1 ||
        !fixture->plan) return -3;
    *out = fixture->plan;
    return 0;
}

struct ModelFixture {
    Fixture state;
    int64_t shape[1] = {1};
    frt_runtime_port_desc ports[2]{};
    frt_runtime_export_v1 exp{};
    frt_model_runtime_v1 model{};

    ModelFixture() { reset(); }

    void reset() {
        state = Fixture{};
        shape[0] = 1;
        std::memset(ports, 0, sizeof(ports));
        exp = frt_runtime_export_v1{};
        model = frt_model_runtime_v1{};
        ports[0] = {"input", FRT_RT_MOD_TENSOR, FRT_RT_DTYPE_F32,
                    FRT_RT_LAYOUT_FLAT, FRT_RT_PORT_IN,
                    FRT_RT_PORT_STAGED, 1, shape, 1, 0, nullptr, 0, 0};
        ports[1] = {"output", FRT_RT_MOD_TENSOR, FRT_RT_DTYPE_F32,
                    FRT_RT_LAYOUT_FLAT, FRT_RT_PORT_OUT,
                    FRT_RT_PORT_STAGED, 0, shape, 1, 0, nullptr, 0, 0};
        exp.abi_version = FRT_RUNTIME_ABI_VERSION;
        exp.struct_size = sizeof(exp);
        exp.fingerprint = 0x148;
        exp.identity = "fake-provider";
        exp.owner = &state;
        exp.retain = retain;
        exp.release = release;
        model.abi_version = FRT_MODEL_RUNTIME_ABI_VERSION;
        model.struct_size = sizeof(model);
        model.exp = &exp;
        model.ports = ports;
        model.n_ports = 2;
        model.self = &state;
        model.verbs.struct_size = sizeof(model.verbs);
        model.verbs.set_input = set_input;
        model.verbs.get_output = get_output;
        model.verbs.step = step;
        model.verbs.last_error = last_error;
        model.owner = &state;
        model.retain = retain;
        model.release = release;
        model.query_extension = query_extension;
    }
};

uint32_t test_kind(void*, uint64_t) { return CAP_MODEL_EXECUTOR_OPAQUE; }
int test_execute(void*, uint64_t) { return CAP_OK; }

}  // namespace

int main() {
    static_assert(sizeof(cap_model_stage) == 48,
                  "cap_model_stage ABI stride changed");
    static_assert(sizeof(cap_model_runtime) == 168,
                  "cap_model_runtime ABI layout changed");
    static_assert(offsetof(cap_model_runtime, impl) == 160,
                  "cap_model_runtime ABI prefix changed");

    const uint32_t action_after[1] = {0};
    frt_generic_stage_desc_v1 stages[2] = {
        {"context", FRT_GENERIC_STAGE_OPAQUE, 4, 0, nullptr},
        {"action", FRT_GENERIC_STAGE_OPAQUE, 91, 1, action_after},
    };
    frt_generic_stage_plan_ext_v1 plan{
        FRT_GENERIC_STAGE_PLAN_ABI_VERSION,
        sizeof(frt_generic_stage_plan_ext_v1), stages, 2, nullptr, run_opaque};

    ModelFixture fixture;
    plan.stage_self = &fixture.state;
    fixture.state.plan = &plan;
    cap_model_runtime* model = nullptr;
    CHECK(flashrt_adopt_model_runtime_abi(&fixture.model, &model) == 0 && model,
          "ABI-only adopter accepts metadata all-OPAQUE runtime");
    CHECK(model && !model->backend && !model->schedule.stages &&
              !model->stage_events && model->n_stages == 2 &&
              !model->stages[0].graph && model->stages[0].stream == -1 &&
              model->stages[1].n_after == 1,
          "all-OPAQUE adoption creates no fake graph/backend/event state");
    CHECK(cap_model_stage_executor_kind(model, 0) ==
              CAP_MODEL_EXECUTOR_OPAQUE &&
              cap_model_fire(nullptr, model, 0) == CAP_ERR_ARG,
          "legacy fire rejects OPAQUE while kind accessor reports it");
    float input = 3.0f;
    CHECK(cap_model_set_input(model, 0, &input, sizeof(input), -1) == 0 &&
              cap_model_tick(nullptr, model) == CAP_OK &&
              fixture.state.stage_calls == 2 && fixture.state.refs[0] == 4 &&
              fixture.state.refs[1] == 91,
          "blocking OPAQUE tick executes selected DAG once in order");
    float output = 0.0f;
    uint64_t written = 0;
    CHECK(cap_model_get_output(model, 1, &output, sizeof(output), &written,
                               -1) == 0 && output == 23.0f,
          "staged output remains provider-owned");
    nexus::StageDagRunner runner(nullptr, model);
    CHECK(runner.ok() && runner.run_once() == CAP_OK &&
              runner.query(0) == CAP_OK && runner.sync(1) == CAP_OK &&
              !runner.in_flight(0) && runner.has_event(0),
          "generic scheduler treats blocking OPAQUE completion as ready");
    CHECK(cap_model_state_status(model) == CAP_ERR_ARG &&
              !cap_model_snapshot(nullptr, model, CAP_TIER_HOST, 0),
          "OPAQUE model state operations fail closed");

    nexus_embedded_config cfg{sizeof(cfg), model, 0};
    nexus_embedded_session* session = nullptr;
    CHECK(nexus_embedded_open(&cfg, &session) == CAP_OK && session &&
              nexus_embedded_ctx(session) == nullptr,
          "embedded host accepts all-OPAQUE runtime without cap_ctx");
    nexus_embedded_tick_result result{};
    result.struct_size = sizeof(result);
    CHECK(nexus_embedded_tick(session, &result) == CAP_OK &&
              result.chunk_id == 1 && nexus_embedded_sync(session, 0) == CAP_OK,
          "embedded OPAQUE tick is synchronously complete");
    CHECK(nexus_embedded_snapshot(session, "blocked") == CAP_ERR_ARG,
          "embedded state API honors model state seam");
    nexus_embedded_close(session);
    flashrt_model_abi_close(model);
    CHECK(fixture.state.retains == 1 && fixture.state.releases == 1,
          "ABI-only adoption retains and releases model exactly once");

    ModelFixture step_fixture;
    step_fixture.model.query_extension = nullptr;
    cap_model_runtime* step_model = nullptr;
    CHECK(flashrt_adopt_model_runtime_abi(
              &step_fixture.model, &step_model) == 0 && step_model &&
              step_model->n_stages == 0 &&
              cap_model_tick(nullptr, step_model) == CAP_OK &&
              step_fixture.state.step_calls == 1,
          "extension-absent empty DAG is classified as step-only");
    flashrt_model_abi_close(step_model);

    ModelFixture bad;
    cap_model_runtime* rejected = nullptr;
    bad.model.abi_version = 99;
    CHECK(flashrt_adopt_model_runtime_abi(&bad.model, &rejected) == -2,
          "wrong model ABI is rejected");
    bad.reset();
    bad.exp.ctx = reinterpret_cast<frt_ctx>(1);
    CHECK(flashrt_adopt_model_runtime_abi(&bad.model, &rejected) == -1,
          "metadata-only adopter rejects execution resources");
    bad.reset();
    bad.ports[0].update = FRT_RT_PORT_SWAP;
    CHECK(flashrt_adopt_model_runtime_abi(&bad.model, &rejected) == -4,
          "provider-only adopter rejects SWAP resources");
    bad.reset();
    frt_generic_stage_plan_ext_v1 empty_plan{
        1, sizeof(empty_plan), nullptr, 0, &bad.state, run_opaque};
    bad.state.plan = &empty_plan;
    CHECK(flashrt_adopt_model_runtime_abi(&bad.model, &rejected) == -4,
          "present-but-empty extension is rejected");
    bad.reset();
    frt_generic_stage_desc_v1 graph_stage{
        "graph", FRT_GENERIC_STAGE_GRAPH, 0, 0, nullptr};
    frt_generic_stage_plan_ext_v1 graph_plan{
        1, sizeof(graph_plan), &graph_stage, 1, &bad.state, run_opaque};
    bad.state.plan = &graph_plan;
    CHECK(flashrt_adopt_model_runtime_abi(&bad.model, &rejected) == -4,
          "provider-only adopter rejects GRAPH authority");
    bad.reset();
    const uint32_t invalid_after[1] = {1};
    frt_generic_stage_desc_v1 invalid_stage{
        "bad", FRT_GENERIC_STAGE_OPAQUE, 7, 1, invalid_after};
    frt_generic_stage_plan_ext_v1 invalid_plan{
        1, sizeof(invalid_plan), &invalid_stage, 1, &bad.state, run_opaque};
    bad.state.plan = &invalid_plan;
    CHECK(flashrt_adopt_model_runtime_abi(&bad.model, &rejected) == -4,
          "forward dependency is rejected");

    cap_model_stage opaque_stage{};
    opaque_stage.name = "opaque";
    cap_model_runtime malformed{};
    malformed.stages = &opaque_stage;
    malformed.n_stages = 1;
    CHECK(cap_model_execute_stage(nullptr, &malformed, 0) == CAP_ERR_ARG,
          "null sidecar returns argument error");
    cap_model_executor_ops_v1 ops{
        0, CAP_MODEL_EXECUTOR_OPS_VERSION, sizeof(ops), nullptr,
        test_kind, test_execute};
    malformed.impl = &ops;
    CHECK(cap_model_execute_stage(nullptr, &malformed, 0) == CAP_ERR_FORMAT,
          "bad sidecar magic returns format error");
    ops.magic = CAP_MODEL_EXECUTOR_OPS_MAGIC;
    ops.version++;
    CHECK(cap_model_execute_stage(nullptr, &malformed, 0) == CAP_ERR_VERSION,
          "unsupported sidecar version returns version error");
    ops.version = CAP_MODEL_EXECUTOR_OPS_VERSION;
    ops.struct_size = CAP_MODEL_EXECUTOR_OPS_V1_SIZE - 1;
    CHECK(cap_model_execute_stage(nullptr, &malformed, 0) == CAP_ERR_VERSION,
          "short sidecar prefix returns version error");

    std::printf(g_fail ? "\n== MODEL ABI ADOPT FAILED ==\n"
                       : "\n== MODEL ABI ADOPT PASSED ==\n");
    return g_fail;
}
