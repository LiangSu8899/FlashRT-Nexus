#ifndef CAPSULE_MODEL_EXECUTOR_INTERNAL_H
#define CAPSULE_MODEL_EXECUTOR_INTERNAL_H

#include <cstddef>
#include <cstdint>

constexpr uint64_t CAP_MODEL_EXECUTOR_OPS_MAGIC =
    UINT64_C(0x4341504558454331);  /* "CAPEXEC1" */
constexpr uint32_t CAP_MODEL_EXECUTOR_OPS_VERSION = 1;

struct cap_model_executor_ops_v1 {
    uint64_t magic;
    uint32_t version;
    uint32_t struct_size;
    void* payload;
    uint32_t (*stage_kind)(void* payload, uint64_t stage_index);
    int (*execute)(void* payload, uint64_t stage_index);
};

constexpr uint32_t CAP_MODEL_EXECUTOR_OPS_V1_SIZE =
    static_cast<uint32_t>(offsetof(cap_model_executor_ops_v1, execute) +
                          sizeof(((cap_model_executor_ops_v1*)0)->execute));

#endif
