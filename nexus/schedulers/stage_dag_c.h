/* nexus/schedulers/stage_dag_c.h — C ABI for the StageDagRunner helper.
 *
 * This is a thin L2 host surface for C/ctypes robot loops. It owns no model,
 * no context, and no thread; the caller keeps cap_ctx and cap_model_runtime
 * alive while the runner exists.
 */
#ifndef NEXUS_SCHEDULERS_STAGE_DAG_C_H
#define NEXUS_SCHEDULERS_STAGE_DAG_C_H

#include "capsule/model_runtime.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nexus_stage_dag_s nexus_stage_dag;

int  nexus_stage_dag_create(cap_ctx, cap_model_runtime*, nexus_stage_dag** out);
void nexus_stage_dag_destroy(nexus_stage_dag*);

int  nexus_stage_dag_fire(nexus_stage_dag*, uint64_t stage_index);
int  nexus_stage_dag_run_once(nexus_stage_dag*);
int  nexus_stage_dag_run_mask(nexus_stage_dag*, uint64_t stage_mask);
int  nexus_stage_dag_run_due(nexus_stage_dag*, uint64_t tick,
                             const uint32_t* periods,
                             const uint32_t* phases,
                             uint64_t n_periods);
int  nexus_stage_dag_query(nexus_stage_dag*, uint64_t stage_index);
int  nexus_stage_dag_sync(nexus_stage_dag*, uint64_t stage_index);
int  nexus_stage_dag_in_flight(nexus_stage_dag*, uint64_t stage_index);
int  nexus_stage_dag_has_event(nexus_stage_dag*, uint64_t stage_index);
int  nexus_stage_dag_last_error(nexus_stage_dag*);

#ifdef __cplusplus
}
#endif

#endif  /* NEXUS_SCHEDULERS_STAGE_DAG_C_H */
