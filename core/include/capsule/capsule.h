/* capsule.h — the Capsule core C ABI (the protocol boundary).
 *
 * The sovereign, zero-dependency inference definition. See Capsule_Core_Spec.
 *
 * Rules of this boundary (constraints):
 *   - ONLY opaque handles, POD structs, byte buffers, and the backend vtable
 *     cross this ABI. No C++ types, no STL, no exceptions. Errors are int status.
 *   - The core depends on nothing but <stdint.h> / <stddef.h>.
 *   - The core owns no loop and no thread. The loop is always an upper layer.
 *   - Hot-path verbs (cap_fire / cap_swap / cap_sync / cap_capsule_ready) do not
 *     allocate and do not lock. One cap_ctx is driven by one thread.
 *   - After v1 this ABI is FROZEN: additive only (append fns / enum values,
 *     bump CAP_ABI_VERSION + the vtable struct_size). Never reorder/remove.
 */
#ifndef CAPSULE_CAPSULE_H
#define CAPSULE_CAPSULE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAP_ABI_VERSION 1u

/* ---- opaque handles ------------------------------------------------------ */
typedef struct cap_ctx_s*     cap_ctx;      /* core context: binds ONE backend       */
typedef struct cap_capsule_s* cap_capsule;  /* a frozen, restorable state object      */
/* Backend-minted handles. Distinct opaque pointer types (not void*) so the ABI is
 * type-safe: a buffer cannot be passed where a graph/event is expected. The backend
 * casts its own concrete type to/from these. */
typedef struct cap_buffer_s* cap_buffer;    /* backend-minted named memory            */
typedef struct cap_graph_s*  cap_graph;     /* backend-minted replayable graph         */
typedef struct cap_event_s*  cap_event;     /* backend-minted cross-stream event       */
typedef uint64_t cap_shape_key;             /* opaque (B, S, ...) variant key          */

/* ---- enums --------------------------------------------------------------- */
enum cap_space  { CAP_DEV = 0, CAP_HOST = 1 };
enum cap_tier   { CAP_TIER_GPU = 0, CAP_TIER_HOST = 1, CAP_TIER_DISK = 2 };
enum cap_trigger{ CAP_EVERY = 0, CAP_ON_EVENT = 1, CAP_ON_DEMAND = 2 };
enum cap_status {
    CAP_OK              =  0,
    CAP_ERR             = -1,
    CAP_ERR_FINGERPRINT = -2,   /* capsule stamp != bound backend fingerprint   */
    CAP_ERR_ARG         = -3,
    CAP_ERR_NOMEM       = -4,
    CAP_ERR_BACKEND     = -5,   /* a backend call reported failure              */
    CAP_ERR_VERSION     = -6,   /* backend abi_version / struct_size mismatch   */
    CAP_ERR_FORMAT      = -7    /* serialized blob is malformed                 */
};

/* ---- POD structs --------------------------------------------------------- */
typedef struct { cap_buffer buf; size_t off; size_t bytes; } cap_region;

typedef struct {
    const cap_region* regions; int n_regions;  /* the named state to freeze        */
    const void* meta; size_t meta_len;          /* opaque metadata (pos/digest/...) */
} cap_boundary;

typedef struct { void* ptr; size_t bytes; } cap_region_view;  /* for zero-copy transport */

typedef struct {
    cap_graph graph; cap_shape_key key;
    int stream; int priority;
    int cadence_num, cadence_den;   /* fire cadence_num per cadence_den ticks; 1/1 = every */
    int trigger;                    /* enum cap_trigger                                    */
} cap_stage;

typedef struct {
    const cap_stage* stages; int n_stages;
    const int (*deps)[2]; int n_deps;   /* {after, before} cross-stage event deps          */
} cap_schedule;

/* ---- backend seam (the other side of the boundary; a backend fills this) -- */
typedef struct cap_backend_s {
    uint32_t abi_version;   /* = CAP_ABI_VERSION   (stability gate) */
    uint32_t struct_size;   /* = sizeof(cap_backend)                */
    void*    self;          /* backend context, passed to every fn  */

    /* buffers: named memory; STATE lives here, backend-owned. buffer_copy is
     * space-agnostic (backend resolves D2D/D2H/H2D between any two buffers).   */
    cap_buffer (*buffer_alloc)(void* self, const char* name, size_t bytes, int space);
    cap_buffer (*buffer_wrap )(void* self, const char* name, void* ptr, size_t bytes, int space);
    void*      (*buffer_ptr  )(void* self, cap_buffer);
    size_t     (*buffer_bytes)(void* self, cap_buffer);
    int        (*buffer_copy )(void* self, cap_buffer dst, size_t doff,
                               cap_buffer src, size_t soff, size_t n, int stream);
    int        (*buffer_upload  )(void* self, cap_buffer dst, size_t off, const void* src, size_t n, int stream); /* host->buf */
    int        (*buffer_download)(void* self, cap_buffer src, size_t off, void* dst,       size_t n, int stream); /* buf->host */
    void       (*buffer_free )(void* self, cap_buffer);

    /* graphs: a ShapeKey -> replayable variant the backend captured/adopted.   */
    int  (*graph_replay)(void* self, cap_graph, cap_shape_key, int stream);
    int  (*graph_has   )(void* self, cap_graph, cap_shape_key);
    int  (*graph_bind  )(void* self, cap_graph, const char* port, cap_buffer);  /* SETUP-TIME only */

    /* streams + events: the only concurrency mechanism.                        */
    int       (*stream      )(void* self, int priority);
    cap_event (*event       )(void* self);
    int       (*event_record)(void* self, cap_event, int stream);
    int       (*event_query )(void* self, cap_event);   /* 0=ready >0=pending <0=err; NON-BLOCKING */
    int       (*stream_wait )(void* self, int stream, cap_event);
    int       (*sync        )(void* self, int stream);
    void      (*event_free  )(void* self, cap_event);

    /* identity: capsule fingerprint = hash{weights,quant,kernel,arch}.         */
    uint64_t (*fingerprint)(void* self);
} cap_backend;

/* ---- context ------------------------------------------------------------- */
cap_ctx  cap_ctx_create (const cap_backend* backend);  /* validates abi_version/struct_size */
void     cap_ctx_destroy(cap_ctx);
uint64_t cap_ctx_fingerprint(cap_ctx);

/* ---- Capsule: state as a first-class, movable object --------------------- *
 * All ops take a stream and are async; poll completion with cap_capsule_ready. */
cap_capsule cap_snapshot     (cap_ctx, const cap_boundary*, int tier, int stream);
int         cap_capsule_ready(cap_ctx, cap_capsule);                  /* non-blocking: 0 ready, >0 pending, <0 err */
int         cap_restore      (cap_ctx, cap_capsule, int stream);      /* into ORIGIN live buffers                  */
int         cap_restore_into (cap_ctx, cap_capsule, const cap_region* dst, int n, int stream); /* branch / receive */
int         cap_regions      (cap_ctx, cap_capsule, cap_region_view* out, int* n); /* out=NULL -> count only       */
int         cap_tier_move    (cap_ctx, cap_capsule, int to_tier, int stream);
int         cap_serialize    (cap_ctx, cap_capsule, void* out, size_t* len);  /* out=NULL -> size query           */
cap_capsule cap_load         (cap_ctx, const void* blob, size_t len);         /* fingerprint-checked; restore_into */
void        cap_capsule_drop (cap_ctx, cap_capsule);

/* ---- Drive: imperative verbs (the LOOP is an upper layer) ---------------- */
int cap_fire      (cap_ctx, const cap_stage*);                       /* one replay (alloc-free) */
int cap_drive_tick(cap_ctx, const cap_schedule*, uint64_t clock, int* failed_stage); /* thin helper */
int cap_swap      (cap_ctx, cap_buffer dst, const void* src, size_t n, int stream);  /* µs CONTENT overwrite */
int cap_sync      (cap_ctx, int stream);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CAPSULE_CAPSULE_H */
