#include "flashrt_model_abi_adapter.h"
#include "flashrt_model_extension_internal.h"

#include "model_executor_internal.h"

#include <cstring>
#include <new>
#include <vector>

namespace {

struct Impl {
    cap_model_executor_ops_v1 ops{};
    const frt_model_runtime_v1* model = nullptr;
    const frt_generic_stage_plan_ext_v1* plan = nullptr;
    std::vector<cap_model_port> ports;
    std::vector<cap_model_stage> stages;
    std::vector<uint32_t> after_store;
    cap_model_runtime pub{};
};

uint32_t stage_kind(void* payload, uint64_t stage_index) {
    const auto* im = static_cast<const Impl*>(payload);
    if (!im || !im->plan || stage_index >= im->plan->n_stages)
        return UINT32_MAX;
    return im->plan->stages[stage_index].executor_kind;
}

int execute(void* payload, uint64_t stage_index) {
    auto* im = static_cast<Impl*>(payload);
    if (!im || !im->plan || stage_index >= im->plan->n_stages ||
        !im->plan->run_opaque) return CAP_ERR_ARG;
    const auto& stage = im->plan->stages[stage_index];
    if (stage.executor_kind != FRT_GENERIC_STAGE_OPAQUE)
        return CAP_ERR_FORMAT;
    return flashrt_model_callback_status(im->plan->run_opaque(
        im->plan->stage_self, stage.executor_ref));
}

bool valid_port(const frt_model_runtime_v1* model,
                const frt_runtime_port_desc& port) {
    if (!port.name || !port.name[0] || (port.rank && !port.shape) ||
        port.update == FRT_RT_PORT_SWAP || port.buffer || port.offset ||
        port.bytes) return false;
    if (port.update == FRT_RT_PORT_STAGED) {
        if (port.direction == FRT_RT_PORT_IN && !model->verbs.set_input)
            return false;
        if (port.direction == FRT_RT_PORT_OUT && !model->verbs.get_output)
            return false;
    }
    return true;
}

}  // namespace

extern "C" int flashrt_adopt_model_runtime_abi(
        const frt_model_runtime_v1* model, cap_model_runtime** out) {
    if (!model || !out) return -1;
    *out = nullptr;
    if (model->abi_version != FRT_MODEL_RUNTIME_ABI_VERSION ||
        model->struct_size < FRT_MODEL_RUNTIME_V1_BASE_SIZE) return -2;
    if (!model->retain || !model->release ||
        !flashrt_model_metadata_export(model->exp) ||
        (model->n_ports && !model->ports) || model->n_stages || model->stages)
        return -1;

    flashrt_model_authority authority;
    if (flashrt_model_query_authority(model, false, &authority) != 0)
        return -4;
    const auto* plan = authority.plan;

    auto* im = new (std::nothrow) Impl();
    if (!im) return -4;
    im->model = model;
    im->plan = plan;
    im->ops.magic = CAP_MODEL_EXECUTOR_OPS_MAGIC;
    im->ops.version = CAP_MODEL_EXECUTOR_OPS_VERSION;
    im->ops.struct_size = sizeof(im->ops);
    im->ops.payload = im;
    im->ops.stage_kind = stage_kind;
    im->ops.execute = execute;

    im->ports.reserve(model->n_ports);
    for (uint64_t i = 0; i < model->n_ports; ++i) {
        const auto& p = model->ports[i];
        if (!valid_port(model, p)) {
            delete im;
            return -4;
        }
        cap_model_port d{};
        d.name = p.name;
        d.modality = p.modality;
        d.dtype = p.dtype;
        d.layout = p.layout;
        d.direction = p.direction;
        d.update = p.update;
        d.required = p.required;
        d.shape = p.shape;
        d.rank = p.rank;
        d.cadence_hint_hz = p.cadence_hint_hz;
        im->ports.push_back(d);
    }

    if (plan) {
        size_t n_after = 0;
        for (uint64_t i = 0; i < plan->n_stages; ++i)
            n_after += plan->stages[i].n_after;
        im->after_store.reserve(n_after);
        im->stages.reserve(plan->n_stages);
        for (uint64_t i = 0; i < plan->n_stages; ++i) {
            const auto& stage = plan->stages[i];
            cap_model_stage d{};
            d.name = stage.name;
            d.stream = -1;
            const size_t offset = im->after_store.size();
            for (uint32_t k = 0; k < stage.n_after; ++k)
                im->after_store.push_back(stage.after[k]);
            d.after = stage.n_after ? &im->after_store[offset] : nullptr;
            d.n_after = stage.n_after;
            im->stages.push_back(d);
        }
    }

    model->retain(model->owner);
    cap_model_runtime& pub = im->pub;
    pub.ports = im->ports.data();
    pub.n_ports = im->ports.size();
    pub.stages = im->stages.data();
    pub.n_stages = im->stages.size();
    pub.fingerprint = model->exp->fingerprint;
    pub.identity = model->exp->identity;
    pub.self = model->self;
    pub.set_input = model->verbs.set_input;
    pub.get_output = model->verbs.get_output;
    pub.prepare = model->verbs.prepare;
    pub.step = model->verbs.step;
    pub.last_error = model->verbs.last_error;
    pub.impl = &im->ops;
    *out = &pub;
    return 0;
}

extern "C" void flashrt_model_abi_close(cap_model_runtime* model) {
    if (!model || !model->impl) return;
    auto* im = reinterpret_cast<Impl*>(model->impl);
    if (im->ops.magic != CAP_MODEL_EXECUTOR_OPS_MAGIC) return;
    if (im->model) im->model->release(im->model->owner);
    delete im;
}
