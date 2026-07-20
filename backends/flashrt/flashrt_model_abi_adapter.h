#ifndef CAPSULE_FLASHRT_MODEL_ABI_ADAPTER_H
#define CAPSULE_FLASHRT_MODEL_ABI_ADAPTER_H

#include "capsule/model_runtime.h"
#include "flashrt/model_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Adopt metadata-only all-OPAQUE or step-only model runtimes without an exec
 * backend. Graph resources and SWAP windows are rejected. */
int flashrt_adopt_model_runtime_abi(const frt_model_runtime_v1* model,
                                    cap_model_runtime** out);
void flashrt_model_abi_close(cap_model_runtime* model);

#ifdef __cplusplus
}
#endif

#endif
