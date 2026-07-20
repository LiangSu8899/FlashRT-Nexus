#ifndef CAPSULE_FLASHRT_MODEL_EXTENSION_INTERNAL_H
#define CAPSULE_FLASHRT_MODEL_EXTENSION_INTERNAL_H

#include "capsule/model_runtime.h"
#include "flashrt/model_runtime.h"

struct flashrt_model_authority {
    const frt_generic_stage_plan_ext_v1* plan = nullptr;
    bool step_only = false;
};

inline int flashrt_model_callback_status(int rc) {
    switch (rc) {
        case 0: return CAP_OK;
        case -1:
        case -3:
        case -4:
        case -7: return CAP_ERR_ARG;
        case -2: return CAP_ERR_FORMAT;
        case -5: return CAP_ERR_NOMEM;
        case -6: return CAP_ERR_BACKEND;
        default: return CAP_ERR;
    }
}

inline bool flashrt_model_metadata_export(const frt_runtime_export_v1* exp) {
    return exp && exp->abi_version == FRT_RUNTIME_ABI_VERSION &&
           exp->struct_size >= sizeof(frt_runtime_export_v1) &&
           !exp->ctx && !exp->n_streams && !exp->n_graphs &&
           !exp->n_buffers && !exp->n_capsule_regions &&
           !exp->streams && !exp->graphs && !exp->buffers &&
           !exp->capsule_regions && exp->identity &&
           exp->retain && exp->release;
}

inline int flashrt_model_query_authority(
        const frt_model_runtime_v1* model, bool allow_graph,
        flashrt_model_authority* authority) {
    if (!model || !authority) return -1;
    *authority = flashrt_model_authority{};

    const void* extension = nullptr;
    int query_rc = -3;
    if (model->struct_size >= FRT_MODEL_RUNTIME_V1_QUERY_EXTENSION_SIZE &&
        model->query_extension) {
        query_rc = model->query_extension(
            model, FRT_EXT_GENERIC_STAGE_PLAN_V1,
            FRT_GENERIC_STAGE_PLAN_ABI_VERSION, &extension);
    }
    if (query_rc != 0) {
        if (extension) return -1;
        if (model->n_stages) return 0;
        if (!model->verbs.step) return -1;
        authority->step_only = true;
        return 0;
    }
    if (!extension) return -1;
    if (model->n_stages) return -1;
    const auto* plan =
        static_cast<const frt_generic_stage_plan_ext_v1*>(extension);
    if (plan->abi_version != FRT_GENERIC_STAGE_PLAN_ABI_VERSION ||
        plan->struct_size < FRT_GENERIC_STAGE_PLAN_EXT_V1_SIZE ||
        !plan->stages || !plan->n_stages || !plan->run_opaque)
        return -1;
    bool has_opaque = false;
    for (uint64_t i = 0; i < plan->n_stages; ++i) {
        const auto& stage = plan->stages[i];
        if (!stage.name || !stage.name[0] ||
            stage.executor_kind > FRT_GENERIC_STAGE_OPAQUE ||
            (!allow_graph && stage.executor_kind !=
                              FRT_GENERIC_STAGE_OPAQUE) ||
            (stage.n_after && !stage.after)) return -1;
        if (stage.executor_kind == FRT_GENERIC_STAGE_GRAPH) {
            if (!model->exp || stage.executor_ref >= model->exp->n_graphs)
                return -1;
        } else {
            has_opaque = true;
        }
        for (uint32_t k = 0; k < stage.n_after; ++k)
            if (stage.after[k] >= i) return -1;
    }
    if (!has_opaque) return -1;
    authority->plan = plan;
    return 0;
}

#endif
