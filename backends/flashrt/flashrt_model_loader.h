#ifndef CAPSULE_FLASHRT_MODEL_LOADER_H
#define CAPSULE_FLASHRT_MODEL_LOADER_H

#include "capsule/model_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct flashrt_loaded_model flashrt_loaded_model;

/* Open one producer DSO through its standard v1 factory and adopt the result.
 * The returned loader owns the adopted model and keeps the DSO loaded until
 * flashrt_loaded_model_close. The model pointer is borrowed from the loader. */
int flashrt_loaded_model_open(const char* provider_dso,
                              const char* config_json,
                              flashrt_loaded_model** out_loader,
                              cap_model_runtime** out_model);
void flashrt_loaded_model_close(flashrt_loaded_model* loader);

/* Thread-local detail for the most recent loader failure. */
const char* flashrt_model_loader_last_error(void);

#ifdef __cplusplus
}
#endif

#endif
