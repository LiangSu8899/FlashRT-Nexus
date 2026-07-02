/* flashrt_model_adapter.h — construct the standard model-runtime face from a
 * FlashRT frt_model_runtime_v1.
 *
 * One call turns a producer-packaged tickable model into a cap_model_runtime:
 * the embedded export is adopted (backend init, streams, graphs, buffers,
 * regions — see flashrt_runtime_adapter.h), SWAP port windows are wired as
 * capsule buffers, the stage DAG becomes fire-ready cap_stage entries plus a
 * prepared cap_schedule, and the producer verbs pass through verbatim.
 *
 * Upper layers see only capsule/model_runtime.h afterwards — no frt types.
 */
#ifndef CAPSULE_FLASHRT_MODEL_ADAPTER_H
#define CAPSULE_FLASHRT_MODEL_ADAPTER_H

#include "capsule/model_runtime.h"
#include "flashrt_runtime_adapter.h"
#include "flashrt/model_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Adopt a model runtime. On success `*out` points at a fully wired
 * cap_model_runtime (the model retained, its export adopted) and the call
 * returns 0. Negative on failure:
 *   -1 bad args, -2 ABI version/size mismatch,
 *   -3 export adoption failed, -4 wiring failed.
 * Release with flashrt_model_close (never free `*out` yourself). */
int flashrt_adopt_model_runtime(const frt_model_runtime_v1* model,
                                cap_model_runtime** out);

void flashrt_model_close(cap_model_runtime*);

#ifdef __cplusplus
}
#endif

#endif /* CAPSULE_FLASHRT_MODEL_ADAPTER_H */
