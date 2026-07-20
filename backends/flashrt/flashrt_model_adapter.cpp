/* flashrt_model_adapter.cpp — frt_model_runtime_v1 -> cap_model_runtime. */
#include "flashrt_model_adapter.h"
#include "flashrt_model_extension_internal.h"

#include "model_executor_internal.h"

#include <cstring>
#include <new>
#include <string>
#include <vector>

namespace {

#define ASSERT_SCHEMA_VALUE(cap_value, frt_value) \
    static_assert(static_cast<uint32_t>(cap_value) == \
                  static_cast<uint32_t>(frt_value), \
                  #cap_value " must mirror " #frt_value)

ASSERT_SCHEMA_VALUE(CAP_MODEL_MOD_TENSOR, FRT_RT_MOD_TENSOR);
ASSERT_SCHEMA_VALUE(CAP_MODEL_MOD_IMAGE, FRT_RT_MOD_IMAGE);
ASSERT_SCHEMA_VALUE(CAP_MODEL_MOD_TEXT, FRT_RT_MOD_TEXT);
ASSERT_SCHEMA_VALUE(CAP_MODEL_MOD_STATE, FRT_RT_MOD_STATE);
ASSERT_SCHEMA_VALUE(CAP_MODEL_MOD_ACTION, FRT_RT_MOD_ACTION);
ASSERT_SCHEMA_VALUE(CAP_MODEL_MOD_AUDIO, FRT_RT_MOD_AUDIO);
ASSERT_SCHEMA_VALUE(CAP_MODEL_MOD_DEPTH, FRT_RT_MOD_DEPTH);
ASSERT_SCHEMA_VALUE(CAP_MODEL_MOD_FORCE, FRT_RT_MOD_FORCE);
ASSERT_SCHEMA_VALUE(CAP_MODEL_DTYPE_U8, FRT_RT_DTYPE_U8);
ASSERT_SCHEMA_VALUE(CAP_MODEL_DTYPE_F32, FRT_RT_DTYPE_F32);
ASSERT_SCHEMA_VALUE(CAP_MODEL_DTYPE_F16, FRT_RT_DTYPE_F16);
ASSERT_SCHEMA_VALUE(CAP_MODEL_DTYPE_BF16, FRT_RT_DTYPE_BF16);
ASSERT_SCHEMA_VALUE(CAP_MODEL_DTYPE_I32, FRT_RT_DTYPE_I32);
ASSERT_SCHEMA_VALUE(CAP_MODEL_DTYPE_I64, FRT_RT_DTYPE_I64);
ASSERT_SCHEMA_VALUE(CAP_MODEL_PORT_IN, FRT_RT_PORT_IN);
ASSERT_SCHEMA_VALUE(CAP_MODEL_PORT_OUT, FRT_RT_PORT_OUT);
ASSERT_SCHEMA_VALUE(CAP_MODEL_PORT_SWAP, FRT_RT_PORT_SWAP);
ASSERT_SCHEMA_VALUE(CAP_MODEL_PORT_STAGED, FRT_RT_PORT_STAGED);
ASSERT_SCHEMA_VALUE(CAP_MODEL_PORT_SETUP, FRT_RT_PORT_SETUP);

#undef ASSERT_SCHEMA_VALUE

struct Impl {
    cap_model_executor_ops_v1 ops{};  /* must remain first: public sidecar */
    const frt_model_runtime_v1* model = nullptr;  /* retained */
    const frt_generic_stage_plan_ext_v1* plan = nullptr;
    flashrt_runtime_binding rb{};
    bool runtime_adopted = false;

    std::vector<cap_model_port>  ports;
    std::vector<cap_model_stage> stages;
    std::vector<uint32_t> after_store;            /* flattened after edges */
    std::vector<cap_stage> cap_stages;            /* schedule storage      */
    std::vector<int>       dep_pairs;             /* flattened {a,b} pairs */
    std::vector<cap_event> stage_events;          /* alloc-free tick       */

    cap_model_runtime pub{};
};

uint32_t stage_kind(void* payload, uint64_t stage_index) {
    const auto* im = static_cast<const Impl*>(payload);
    if (!im || !im->plan || stage_index >= im->plan->n_stages)
        return UINT32_MAX;
    return im->plan->stages[stage_index].executor_kind;
}

int execute(void* payload, uint64_t stage_index) {
    const auto* im = static_cast<const Impl*>(payload);
    if (!im || !im->plan || stage_index >= im->plan->n_stages)
        return CAP_ERR_ARG;
    const auto& stage = im->plan->stages[stage_index];
    if (stage.executor_kind != FRT_GENERIC_STAGE_OPAQUE)
        return CAP_ERR_FORMAT;
    return flashrt_model_callback_status(im->plan->run_opaque(
        im->plan->stage_self, stage.executor_ref));
}

/* Resolve an frt stream id to the capsule stream index the export adoption
 * produced (export stream order == rb.streams order). */
int resolve_stream(const Impl* im, int frt_stream_id) {
    const frt_runtime_export_v1* exp = im->model->exp;
    for (uint64_t i = 0; i < exp->n_streams; ++i)
        if (exp->streams[i].stream_id == frt_stream_id)
            return im->rb.streams[i];
    return 0;  /* the backend's own working stream */
}

bool all_opaque(const frt_generic_stage_plan_ext_v1* plan) {
    if (!plan) return false;
    for (uint64_t i = 0; i < plan->n_stages; ++i)
        if (plan->stages[i].executor_kind != FRT_GENERIC_STAGE_OPAQUE)
            return false;
    return true;
}

}  // namespace

extern "C" int flashrt_adopt_model_runtime(const frt_model_runtime_v1* model,
                                           cap_model_runtime** out) {
    if (!model || !out) return -1;
    *out = nullptr;
    if (model->abi_version != FRT_MODEL_RUNTIME_ABI_VERSION ||
        model->struct_size < FRT_MODEL_RUNTIME_V1_BASE_SIZE) return -2;
    if (!model->exp || !model->retain || !model->release) return -1;
    if ((model->n_ports && !model->ports) ||
        (model->n_stages && !model->stages)) return -1;

    flashrt_model_authority authority;
    if (flashrt_model_query_authority(model, true, &authority) != 0)
        return -4;
    const bool metadata_only = authority.step_only || all_opaque(authority.plan);
    if (metadata_only && !flashrt_model_metadata_export(model->exp)) return -4;

    auto* im = new (std::nothrow) Impl();
    if (!im) return -4;

    if (!metadata_only) {
        int rc = flashrt_adopt_runtime_export(model->exp, &im->rb);
        if (rc != 0) { delete im; return -3; }
        im->runtime_adopted = true;
    }
    model->retain(model->owner);
    im->model = model;
    im->plan = authority.plan;
    im->ops = {CAP_MODEL_EXECUTOR_OPS_MAGIC,
               CAP_MODEL_EXECUTOR_OPS_VERSION, sizeof(im->ops), im,
               stage_kind, execute};
    im->pub.impl = &im->ops;  /* set first so close() cleans up failures */

    const frt_runtime_export_v1* exp = model->exp;

    /* ports: SWAP/OUT windows become capsule buffers on the same backend */
    im->ports.reserve(model->n_ports);
    for (uint64_t i = 0; i < model->n_ports; ++i) {
        const frt_runtime_port_desc& p = model->ports[i];
        cap_model_port d{};
        d.name = p.name;
        d.modality = p.modality; d.dtype = p.dtype; d.layout = p.layout;
        d.direction = p.direction; d.update = p.update; d.required = p.required;
        d.shape = p.shape; d.rank = p.rank;
        d.cadence_hint_hz = p.cadence_hint_hz;
        d.buffer = nullptr;
        d.offset = p.offset; d.bytes = p.bytes;
        if (p.buffer && im->runtime_adopted) {
            d.buffer = flashrt_wrap_buffer(&im->rb.backend, p.buffer);
            if (!d.buffer) { flashrt_model_close(&im->pub); return -4; }
        } else if (p.buffer) {
            flashrt_model_close(&im->pub); return -4;
        }
        im->ports.push_back(d);
    }

    /* stages: DAG entries + a prepared schedule for cap_drive_tick */
    const uint64_t n_stages = im->plan ? im->plan->n_stages : model->n_stages;
    im->stages.reserve(n_stages);
    im->cap_stages.reserve(n_stages);
    size_t n_after_total = 0;
    for (uint64_t i = 0; i < n_stages; ++i)
        n_after_total += im->plan ? im->plan->stages[i].n_after
                                  : model->stages[i].n_after;
    im->after_store.reserve(n_after_total);
    im->dep_pairs.reserve(n_after_total * 2);

    for (uint64_t i = 0; i < n_stages; ++i) {
        const uint32_t executor_kind = im->plan
            ? im->plan->stages[i].executor_kind
            : static_cast<uint32_t>(FRT_GENERIC_STAGE_GRAPH);
        const uint32_t executor_ref = im->plan
            ? im->plan->stages[i].executor_ref : model->stages[i].graph;
        const uint32_t* after = im->plan
            ? im->plan->stages[i].after : model->stages[i].after;
        const uint32_t n_after = im->plan
            ? im->plan->stages[i].n_after : model->stages[i].n_after;

        cap_model_stage d{};
        d.name = im->plan ? im->plan->stages[i].name : nullptr;
        d.stream = -1;
        if (executor_kind == FRT_GENERIC_STAGE_GRAPH) {
            if (!im->runtime_adopted || executor_ref >= exp->n_graphs) {
                flashrt_model_close(&im->pub); return -4;
            }
            const frt_runtime_graph_desc& g = exp->graphs[executor_ref];
            if (!d.name) d.name = g.name;
            d.graph = im->rb.graphs[executor_ref];
            d.key = (cap_shape_key)g.default_key;
            d.stream = resolve_stream(im, g.stream_id);
        }
        const size_t off = im->after_store.size();
        for (uint32_t k = 0; k < n_after; ++k) {
            if (after[k] >= i) { flashrt_model_close(&im->pub); return -4; }
            im->after_store.push_back(after[k]);
            im->dep_pairs.push_back((int)after[k]);   /* after  */
            im->dep_pairs.push_back((int)i);            /* before */
        }
        d.after = n_after ? &im->after_store[off] : nullptr;
        d.n_after = n_after;
        im->stages.push_back(d);

        cap_stage st{};
        st.graph = d.graph; st.key = d.key; st.stream = d.stream;
        st.priority = 0; st.cadence_num = 1; st.cadence_den = 1;
        st.trigger = CAP_EVERY;
        im->cap_stages.push_back(st);
    }

    /* pre-create one event per depended-upon stage: cap_model_tick then
     * runs allocation-free, tick after tick */
    im->stage_events.assign(n_stages, nullptr);
    for (uint64_t i = 0; i < n_stages; ++i)
        for (uint32_t k = 0; k < im->stages[i].n_after; ++k) {
            const uint32_t dep = im->stages[i].after[k];
            if (im->stages[i].graph && im->stages[dep].graph &&
                (!im->plan ||
                 im->stages[i].stream != im->stages[dep].stream) &&
                !im->stage_events[dep]) {
                cap_event ev = im->rb.backend.event(im->rb.backend.self);
                if (!ev) { flashrt_model_close(&im->pub); return -4; }
                im->stage_events[dep] = ev;
            }
        }

    cap_model_runtime& m = im->pub;
    m.backend = im->runtime_adopted ? &im->rb.backend : nullptr;
    m.ports = im->ports.data();   m.n_ports = im->ports.size();
    m.stages = im->stages.data(); m.n_stages = im->stages.size();
    m.regions = im->runtime_adopted ? im->rb.regions : nullptr;
    m.n_regions = im->runtime_adopted ? im->rb.n_regions : 0;
    m.schedule.stages = im->cap_stages.data();
    m.schedule.n_stages = (int)im->cap_stages.size();
    m.schedule.deps = im->dep_pairs.empty()
                          ? nullptr
                          : reinterpret_cast<const int(*)[2]>(im->dep_pairs.data());
    m.schedule.n_deps = (int)(im->dep_pairs.size() / 2);
    m.stage_events = im->runtime_adopted ? im->stage_events.data() : nullptr;
    m.fingerprint = exp->fingerprint;
    m.identity = exp->identity;
    m.self = model->self;
    m.set_input = model->verbs.set_input;
    m.get_output = model->verbs.get_output;
    m.prepare = model->verbs.prepare;
    m.step = model->verbs.step;
    m.last_error = model->verbs.last_error;
    m.impl = &im->ops;

    *out = &im->pub;
    return 0;
}

extern "C" void flashrt_model_close(cap_model_runtime* m) {
    if (!m || !m->impl) return;
    auto* ops = static_cast<cap_model_executor_ops_v1*>(m->impl);
    Impl* im = static_cast<Impl*>(ops->payload);
    for (cap_event ev : im->stage_events)             /* before the backend dies */
        if (ev) im->rb.backend.event_free(im->rb.backend.self, ev);
    if (im->runtime_adopted)
        flashrt_runtime_binding_fini(&im->rb);        /* backend + export */
    if (im->model) im->model->release(im->model->owner);
    delete im;
}
