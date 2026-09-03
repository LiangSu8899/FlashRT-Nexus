#include "flashrt_model_abi_adapter.h"

extern "C" int flashrt_adopt_model_runtime(
    const frt_model_runtime_v1* model, cap_model_runtime** out) {
    return flashrt_adopt_model_runtime_abi(model, out);
}

extern "C" void flashrt_model_close(cap_model_runtime* model) {
    flashrt_model_abi_close(model);
}
