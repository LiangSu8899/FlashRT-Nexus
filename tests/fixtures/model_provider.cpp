#include "flashrt/model_runtime.h"

#include <atomic>
#include <cstring>
#include <new>

namespace {

struct Owner {
    std::atomic<int> refs{1};
    int calls = 0;
    int64_t shape[1] = {1};
    frt_runtime_port_desc port{};
    frt_generic_stage_desc_v1 stage{};
    frt_generic_stage_plan_ext_v1 plan{};
    frt_runtime_export_v1 exp{};
    frt_model_runtime_v1 model{};
};

void retain(void* self) {
    static_cast<Owner*>(self)->refs.fetch_add(1, std::memory_order_relaxed);
}
void release(void* self) {
    auto* owner = static_cast<Owner*>(self);
    if (owner->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) delete owner;
}
int run_opaque(void* self, uint32_t ref) {
    if (ref != 37) return -2;
    ++static_cast<Owner*>(self)->calls;
    return 0;
}
int get_output(void* self, uint32_t port, void* out, uint64_t capacity,
               uint64_t* written, int) {
    if (written) *written = sizeof(int);
    if (port || !out || capacity < sizeof(int)) return -5;
    const int calls = static_cast<Owner*>(self)->calls;
    std::memcpy(out, &calls, sizeof(calls));
    return 0;
}
const char* last_error(void*) { return "fixture provider error"; }
int query(const frt_model_runtime_v1* runtime, uint64_t id,
          uint32_t min_version, const void** out) {
    if (!out) return -1;
    *out = nullptr;
    if (id != FRT_EXT_GENERIC_STAGE_PLAN_V1 || min_version > 1) return -3;
    *out = &static_cast<Owner*>(runtime->owner)->plan;
    return 0;
}

}  // namespace

extern "C" __attribute__((visibility("default")))
int frt_model_runtime_open_v1(const char* config_json,
                              frt_model_runtime_v1** out) {
    if (!out) return -1;
    *out = nullptr;
    if (!config_json || std::strcmp(config_json, "{\"fixture\":true}") != 0)
        return -1;
    auto* owner = new (std::nothrow) Owner();
    if (!owner) return -5;
    owner->port = {"calls", FRT_RT_MOD_TENSOR, FRT_RT_DTYPE_I32,
                   FRT_RT_LAYOUT_FLAT, FRT_RT_PORT_OUT, FRT_RT_PORT_STAGED,
                   0, owner->shape, 1, 0, nullptr, 0, 0};
    owner->stage = {"infer", FRT_GENERIC_STAGE_OPAQUE, 37, 0, nullptr};
    owner->plan = {FRT_GENERIC_STAGE_PLAN_ABI_VERSION, sizeof(owner->plan),
                   &owner->stage, 1, owner, run_opaque};
    owner->exp.abi_version = FRT_RUNTIME_ABI_VERSION;
    owner->exp.struct_size = sizeof(owner->exp);
    owner->exp.fingerprint = 0x148;
    owner->exp.identity = "fixture-provider";
    owner->exp.owner = owner;
    owner->exp.retain = retain;
    owner->exp.release = release;
    owner->model.abi_version = FRT_MODEL_RUNTIME_ABI_VERSION;
    owner->model.struct_size = sizeof(owner->model);
    owner->model.exp = &owner->exp;
    owner->model.ports = &owner->port;
    owner->model.n_ports = 1;
    owner->model.self = owner;
    owner->model.verbs.struct_size = sizeof(owner->model.verbs);
    owner->model.verbs.get_output = get_output;
    owner->model.verbs.last_error = last_error;
    owner->model.owner = owner;
    owner->model.retain = retain;
    owner->model.release = release;
    owner->model.query_extension = query;
    *out = &owner->model;
    return 0;
}
