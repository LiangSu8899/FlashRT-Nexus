/* nexus/embedded/session.h — C ABI for same-process Nexus control loops.
 *
 * This is the no-HTTP host surface for robot/edge applications. A producer
 * still owns model setup/capture/export; the application passes the adopted
 * cap_model_runtime here and drives ports/stages through a resident session.
 *
 * The core owns no thread. One nexus_embedded_session serializes mutating
 * verbs internally so a threaded host cannot drive one cap_ctx concurrently.
 */
#ifndef NEXUS_EMBEDDED_SESSION_H
#define NEXUS_EMBEDDED_SESSION_H

#include "capsule/model_runtime.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nexus_embedded_session_s nexus_embedded_session;

typedef struct nexus_embedded_config {
    uint32_t struct_size;
    cap_model_runtime* model;      /* caller keeps model alive while session exists */
    uint32_t flags;                /* reserved, must be 0 for now */
} nexus_embedded_config;

typedef struct nexus_embedded_tick_result {
    uint32_t struct_size;
    uint64_t chunk_id;
    double latency_ms;
    uint64_t written;
} nexus_embedded_tick_result;

enum nexus_embedded_update {
    NEXUS_EMBEDDED_SET_INPUT = 0,  /* producer STAGED verb */
    NEXUS_EMBEDDED_SWAP      = 1   /* direct SWAP window write */
};

typedef struct nexus_embedded_input {
    uint32_t struct_size;
    const char* port;
    const void* data;
    uint64_t bytes;
    uint32_t update;              /* enum nexus_embedded_update */
    int stream;
} nexus_embedded_input;

typedef struct nexus_embedded_output {
    uint32_t struct_size;
    const char* port;
    void* data;
    uint64_t capacity;
    uint64_t written;
    int stream;
} nexus_embedded_output;

int  nexus_embedded_open(const nexus_embedded_config*,
                         nexus_embedded_session** out);
void nexus_embedded_close(nexus_embedded_session*);

cap_ctx            nexus_embedded_ctx(nexus_embedded_session*);
cap_model_runtime* nexus_embedded_model(nexus_embedded_session*);
uint64_t           nexus_embedded_fingerprint(nexus_embedded_session*);
const char*        nexus_embedded_identity(nexus_embedded_session*);
const char*        nexus_embedded_last_error(nexus_embedded_session*);

int nexus_embedded_find_port(nexus_embedded_session*, const char* name);
int nexus_embedded_set_input(nexus_embedded_session*, const char* port,
                             const void* data, uint64_t bytes, int stream);
int nexus_embedded_swap(nexus_embedded_session*, const char* port,
                        const void* data, uint64_t bytes, int stream);
int nexus_embedded_tick(nexus_embedded_session*,
                        nexus_embedded_tick_result* result);
int nexus_embedded_sync(nexus_embedded_session*, int stream);
int nexus_embedded_get_output(nexus_embedded_session*, const char* port,
                              void* out, uint64_t capacity,
                              uint64_t* written, int stream);

/* Convenience: optional input update, one tick, optional output read. The
 * input path uses set_input; for SWAP ports use nexus_embedded_swap + tick. */
int nexus_embedded_act(nexus_embedded_session*,
                       const char* input_port, const void* input,
                       uint64_t input_bytes,
                       const char* output_port, void* output,
                       uint64_t output_capacity,
                       nexus_embedded_tick_result* result);

/* One transport/control-loop step: apply all inputs, tick once, fill all
 * outputs. A ROS2/shm adapter should map incoming messages/ring slots into
 * these POD views and call this function once per control tick. */
int nexus_embedded_step(nexus_embedded_session*,
                        const nexus_embedded_input* inputs,
                        uint64_t n_inputs,
                        nexus_embedded_output* outputs,
                        uint64_t n_outputs,
                        nexus_embedded_tick_result* result);

int nexus_embedded_snapshot(nexus_embedded_session*, const char* name);
int nexus_embedded_restore(nexus_embedded_session*, const char* name);
int nexus_embedded_serialize(nexus_embedded_session*, const char* name,
                             void* out, size_t* len);
int nexus_embedded_load(nexus_embedded_session*, const char* name,
                        const void* blob, size_t len);
uint64_t nexus_embedded_capsule_count(nexus_embedded_session*);

#ifdef __cplusplus
}
#endif

#endif  /* NEXUS_EMBEDDED_SESSION_H */
