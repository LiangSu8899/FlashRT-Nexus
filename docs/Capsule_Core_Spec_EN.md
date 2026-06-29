# Capsule — Core Spec (the sovereign, zero-dependency inference definition)

> Status: canonical spec · v2 · 2026-06-29 · the spine of the `capsule` repo
> Companions: `Capsule_Serving_Design_EN.md` (rationale), `Capsule_Serving_Integration_EN.md`
> (the FlashRT *backend* packaging). This document is authoritative on the **core**. The authoritative
> *form* of the protocol is the header `capsule/core/include/capsule/capsule.h`; this doc explains it.

---

## 0. The one idea

> Build one **sovereign, zero-dependency C++ core** that *defines* inference, and keep everything
> else — schedulers, protocols, modes, transport, even the GPU backend — as **pluggable upper layers**
> that adapt *into* the core. The core executes; the upper layers decide. The core holds; the framework
> lives above it. The core is small enough to fit in one header, depends on nothing, never learns a
> scenario, and **freezes** after v1 so the ecosystem can build on solid ground.

---

## 1. The new inference definition

> **Inference is not a request flowing through a scheduler.** Inference is: *replay a captured graph
> over named state buffers; the state is a first-class object you can snapshot, restore, branch, and
> move; an imperative loop decides which graph fires, on which stream, when — and can interrupt at
> graph boundaries.*

The old paradigm makes the **request pipeline** central (tokenizer → scheduler → batcher →
KV-block-manager → runner) and hides state inside a memory manager. The new paradigm makes **state**
central and the pipeline trivial (fire a replay). That inversion is the contribution.

---

## 2. Layering — core executes, upper layers decide

```
 L3  ECOSYSTEM / apps      agents · lerobot · PKU · omni · robot products        bring deps, adapt IN
                                 │  compose / speak protocol
 ───────────────────────────────▼──────────────────────────────────────────────────────────────────
 L2  FRAMEWORK  (pluggable "frontend" — DECOUPLED, swappable; the framework SHIPS these)
       schedulers:      robot-async · multi-model · multi-hardware   ◀── author/mutate a Schedule;
       state-services:  CapsuleStore (registry/tier/LRU) · Session/journal   own the decision loop
       adapters:        transport(openai/grpc/ros2/shm) · backends
       modes:           agent · rollout · handoff · duplex
 ───────────────────────────────┬──────────────────────────────────────────────────────────────────
 L1  capsule CORE  (C++ · C ABI · ZERO dependency · SOVEREIGN · FROZEN after v1)  ◀── inference definition
       Capsule (state)        ·   imperative Drive verbs (NO loop)
       Schedule (static data) ·   Backend seam (abstract)
 ───────────────────────────────▲──────────────────────────────────────────────────────────────────
 L0  BACKENDS             flashrt (first) · raw-cuda · cpu-edge · future            implement the seam
```

- **L1 depends on nothing.** Not on L0, not on L2, not on Python. It defines a backend seam and
  provides imperative verbs; it **owns no loop and no thread**.
- **L2 is where the framework's value lives** — fully pluggable. Two kinds of L2 component: **schedulers**
  (own the decision loop) and **state-services** (CapsuleStore registry/tier/LRU, Session/journal —
  the things demoted out of the core). The framework ships a set; integrators swap or write their own.
  L1 never imports L2.
- **L0 plugs in from below**; FlashRT is the first backend. **L3 composes from above.** All arrows
  point *into* L1.

---

## 3. The core C ABI (the protocol boundary)

The header is authoritative; this is the shape. ~35 functions; dependencies = `<stdint.h>`,
`<stddef.h>`. Nothing crosses the boundary except **opaque handles**, **POD structs**, **byte buffers**,
and the **backend vtable**. No C++ types, no STL, no exceptions cross the C ABI.

### 3.1 Backend seam — the only external thing the core touches

A backend fills a vtable; the core calls it. The vtable is **versioned** (`abi_version` + `struct_size`)
so a core can validate/reject a mismatched backend — a stability hinge. FlashRT's `libflashrt_exec`
maps onto this ~1:1.

```c
typedef struct cap_backend_s {
    uint32_t abi_version;   /* = CAP_ABI_VERSION (stability gate) */
    uint32_t struct_size;   /* = sizeof(cap_backend) */
    void*    self;          /* backend ctx, passed to every fn */

    /* buffers: named memory; state lives here, BACKEND-owned (dev or host) */
    cap_buffer (*buffer_alloc)(void* self, const char* name, size_t bytes, int space);
    cap_buffer (*buffer_wrap )(void* self, const char* name, void* ptr, size_t bytes, int space);
    void*      (*buffer_ptr  )(void* self, cap_buffer);
    size_t     (*buffer_bytes)(void* self, cap_buffer);
    int        (*buffer_copy )(void* self, cap_buffer dst, size_t doff,
                               cap_buffer src, size_t soff, size_t n, int stream);   /* D2D */
    int        (*buffer_upload  )(void* self, cap_buffer dst, size_t off, const void* src, size_t n, int stream); /* H2D */
    int        (*buffer_download)(void* self, cap_buffer src, size_t off, void* dst,       size_t n, int stream); /* D2H */
    void       (*buffer_free )(void* self, cap_buffer);
    /* graphs: a ShapeKey -> replayable variant the backend captured/adopted */
    int  (*graph_replay)(void* self, cap_graph, cap_shape_key, int stream);
    int  (*graph_has   )(void* self, cap_graph, cap_shape_key);
    int  (*graph_bind  )(void* self, cap_graph, const char* port, cap_buffer);  /* SETUP-TIME only */
    /* streams + events: the ONLY concurrency mechanism */
    int       (*stream)(void* self, int priority);
    cap_event (*event )(void* self);
    int  (*event_record)(void* self, cap_event, int stream);
    int  (*event_query )(void* self, cap_event);   /* 0=ready >0=pending <0=err; NON-BLOCKING */
    int  (*stream_wait )(void* self, int stream, cap_event);
    int  (*sync        )(void* self, int stream);
    void (*event_free  )(void* self, cap_event);
    /* identity: the capsule fingerprint = hash{weights,quant,kernel,arch} */
    uint64_t (*fingerprint)(void* self);
} cap_backend;
```

### 3.2 Capsule — the genuinely-new state primitive

The core copies *named regions*; it never interprets what KV / recurrent / conv state *means*.
Mechanism, not policy. All state ops take a `stream` and are **async** — completion is polled with
`cap_capsule_ready`, so state never stalls the control loop.

```c
typedef struct { cap_buffer buf; size_t off; size_t bytes; } cap_region;
typedef struct {
    const cap_region* regions; int n_regions;   /* the named state to freeze */
    const void* meta; size_t meta_len;            /* small metadata (pos, digest, ...) — opaque to the core */
} cap_boundary;
typedef struct { void* ptr; size_t bytes; } cap_region_view;   /* for zero-copy transport */

cap_capsule cap_snapshot   (cap_ctx, const cap_boundary*, int tier, int stream);
int         cap_capsule_ready(cap_ctx, cap_capsule);                 /* non-blocking */
int         cap_restore    (cap_ctx, cap_capsule, int stream);       /* into ORIGIN live buffers */
int         cap_restore_into(cap_ctx, cap_capsule, const cap_region* dst, int n, int stream); /* branch / recv */
int         cap_regions    (cap_ctx, cap_capsule, cap_region_view* out, int* n); /* zero-copy transport access */
int         cap_tier_move  (cap_ctx, cap_capsule, int to_tier, int stream);  /* GPU↔HOST↔DISK */
int         cap_serialize  (cap_ctx, cap_capsule, void* out, size_t* len);   /* out=NULL → size query */
cap_capsule cap_load       (cap_ctx, const void* blob, size_t len);          /* fingerprint-checked */
void        cap_capsule_drop(cap_ctx, cap_capsule);
```

- **`restore`** copies frozen regions back into the *origin* live buffers (same-process warm start /
  undo). **`restore_into`** copies into a *caller-supplied* live set — this is the mechanism for **fork**
  (branch into N engine instances) and for **receiving a shipped capsule** (a `cap_load`ed capsule has no
  origin binding, so it is `restore_into`-only).
- **fork / time-travel are L2 sugar** over `snapshot` + `restore_into` — not core verbs.

### 3.3 Schedule — a static description (data, NOT a scheduler)

```c
typedef struct {
    cap_graph graph; cap_shape_key key;
    int stream; int priority;
    int cadence_num, cadence_den;   /* fire cadence_num per cadence_den ticks; 1/1 = every tick */
    int trigger;                    /* CAP_EVERY | CAP_ON_EVENT | CAP_ON_DEMAND */
} cap_stage;
typedef struct {
    const cap_stage* stages; int n_stages;
    const int (*deps)[2]; int n_deps;   /* {after, before} cross-stage event deps */
} cap_schedule;
```

### 3.4 Drive — imperative verbs (the LOOP is L2)

The core provides verbs; **it never owns a `while` loop or a thread.** The loop is always a scheduler
(L2). `cap_fire`/`cap_swap`/`cap_sync` are the irreducible, **allocation-free** hot-path verbs;
`cap_drive_tick` is a *thin optional helper* (a pure function of `schedule × clock`) that a scheduler
may use or replace.

```c
cap_ctx cap_ctx_create (const cap_backend*);   /* validates abi_version; binds ONE backend */
void    cap_ctx_destroy(cap_ctx);

int cap_fire      (cap_ctx, const cap_stage*);                       /* one replay (alloc-free) */
int cap_drive_tick(cap_ctx, const cap_schedule*, uint64_t clock, int* failed_stage); /* helper; failed_stage localizes a fault */
int cap_swap      (cap_ctx, cap_buffer dst, const void* src, size_t n, int stream);  /* µs CONTENT overwrite: subgoal/obs */
int cap_sync      (cap_ctx, int stream);
```

**Capsule + the Drive verbs are the only content genuinely new over the FlashRT exec contract;**
Schedule ≈ a Plan + cadence, and the Backend seam ≈ the exec primitives, restated so the core owns them.

---

## 4. Core vs scheduler — the precise split

| | Core (L1) | Scheduler (L2, pluggable) |
|---|---|---|
| Role | **executes** verbs / a Schedule description | **authors/mutates** the description; **owns** the loop |
| Knows | streams, events, replays, regions | cadence, priority, placement, interrupt, backpressure |
| Owns a loop / thread? | **no** | **yes — it *is* the loop** |
| Depends on | nothing | the core (calls its C ABI) |

- **Interrupt** is not a core feature — it is the scheduler *choosing not to issue the next tick* and
  firing a high-priority stage instead. The core only guarantees **graph-boundary granularity** (a replay
  is atomic; the loop re-decides between replays). Mid-replay cancellation is impossible by construction
  (keep graphs short).
- **Conditional subgraph routing** (run B only if A's output meets a condition) = the scheduler reads a
  small result (`buffer_download` a few bytes) and calls `cap_fire` for B or C. A deliberate host-return
  seam with a known latency cost — not a core gap.
- **External/host triggers** (message arrived, sensor ready) = the scheduler calls `cap_fire`
  imperatively. `CAP_ON_EVENT` refers only to a GPU event (cross-stream); the core knows no host signals.

**The three schedulers the framework ships (all L2, all pluggable):** **robot-async** (action cadence;
vision/ASR side streams; interrupt = `cap_swap` a subgoal; reset = `cap_restore`), **multi-model**
(co-host N engines; cadence/concurrency/gap-fill-vs-p99), **multi-hardware** (placement; bind backends;
ship a capsule over a Link for cloud-edge). Each is a few hundred lines over the C ABI, independently
testable, swappable, and **invisible to L1**.

---

## 5. Stability & protocol-boundary contract (constraints)

The core is infrastructure others build on, so its boundary and guarantees are contractual.

### 5.1 What crosses the boundary — nothing else
Only **opaque handles** (`cap_ctx/cap_capsule/cap_buffer/cap_graph/cap_event`), **POD structs**
(`cap_region/cap_boundary/cap_stage/cap_schedule/cap_region_view`), **byte buffers** (`serialize/load`),
and the **backend vtable**. No C++ objects, STL types, ownership-via-smart-pointer, or exceptions ever
cross. Errors are `int` status codes (`CAP_OK=0`, negatives in `enum cap_status`).

### 5.2 Core guarantees (what L2/L0 may rely on)
- **No hidden allocation in hot-path verbs.** `cap_fire/cap_swap/cap_sync/cap_capsule_ready` allocate
  nothing. (`snapshot`/`tier_move`/`load` allocate — they are setup/boundary ops, not the control loop.)
- **No locks, ever.** The core has no internal mutex. (See 5.4.)
- **Fingerprint refusal.** `restore`/`restore_into`/`load` refuse with `CAP_ERR_FINGERPRINT` when the
  capsule's stamp ≠ the bound backend's `fingerprint()`. Restore is never silently wrong.
- **Fault localization.** `cap_drive_tick` reports the failing stage index; a backend error never
  corrupts core state.

### 5.3 Backend contract (what a backend MUST guarantee)
Deterministic `graph_replay`; stream-ordered `buffer_copy/upload/download`; **non-blocking**
`event_query`; a **stable** `fingerprint` for a given build; handles valid until freed; **never throw
across the ABI** (return status). Sets `abi_version`/`struct_size`.

### 5.4 Threading model — zero-lock, one ctx per thread
A `cap_ctx` is driven by **one thread**; the core is **not internally synchronized** (no hot-path
mutex — that is the price of zero overhead). Multiple devices / models = multiple `cap_ctx` on multiple
threads. Cross-thread / cross-device / cross-node coordination is **L2** (events within a ctx; transport
between ctxs/nodes). This must be honored, not assumed away.

### 5.5 Runtime mutation discipline
`cap_swap` overwrites buffer **content** (µs). `graph_bind` is **setup-time only** — never rebind a
graph's buffer *pointer* at runtime (captured graphs bake absolute pointers; a rebind means recapture).
Anything mutable at interrupt cadence (subgoal, pose, mode flag) is a bound buffer overwritten by
`cap_swap`, never baked into a graph.

### 5.6 ABI stability — freeze, then additive-only
After v1 the C ABI **freezes**. Evolution is **additive only**: append new functions, append new enum
values, bump `CAP_ABI_VERSION` and the vtable `struct_size`. Never reorder/remove/repurpose. Old cores
reject newer-but-larger vtables gracefully via the version/size fields. This is what lets the ecosystem
build on the core without churn.

---

## 6. What we DON'T do (non-goals — the negative space)

1. **The core builds no scheduler.** It executes a Schedule description; it owns no loop. Schedulers are
   pluggable L2.
2. **We don't compile model logic.** No engine-as-artifact (anti-TensorRT). Models = captured graphs
   (data) + named state; zero model-specific code in the core.
3. **We don't manage GPU memory or compute.** No KV manager, paged/block allocator, radix tree, or pool.
   State = backend-owned named buffers; the core holds handles + copies regions.
4. **The core depends on nothing.** No Python in the loop, no framework, no third-party lib. Backends
   (L0) and adapters (L2) carry deps at the edge.
5. **The core encodes no policy.** No protocol, agent loop, robot cadence, cache-eviction, batching, or
   backpressure policy. Mechanism-only, recursively.
6. **We don't layer for abstraction's sake.** No deep hierarchies, plugin/DI frameworks, config-driven
   indirection, reflection. Fewest objects, each concrete, all auditable.
7. **We don't optimize for datacenter throughput / multi-tenant.** Latency-first, small-batch,
   high-frequency, single/few-session, edge-capable.
8. **We assume no deployment shape.** Not request/response-, stateless-worker-, big-GPU/cloud-, or
   single-model-centric.
9. **We don't reinvent the execution/kernel layer.** No graph capture, kernels, or CUDA-context
   ownership in the core — that is behind the backend seam. We define *serving*, not *execution*.
10. **We don't make capsules portable across deployments.** A capsule is a fingerprinted binary state
    blob bound to {weights, quant, kernel, arch}. No portable cross-version format.
11. **No locks, no hidden allocation, no callbacks in the hot path.** Poll (`event_query`), never
    callback; the core never spawns a thread.

The repo's shape *is* this list inverted: a tiny C++ core, with everything on the "don't" list living
above it or nowhere.

---

## 7. Dependency posture & languages

- **Deployment hot loop is C++.** No GIL, no GC, no runtime in the path that runs sub-ms control. The
  Drive verbs, Capsule ops, and backend seam are all C/C++ over a C ABI.
- **The core depends on nothing** (dependency inversion): it links an abstract backend vtable, not a
  backend. FlashRT's `libflashrt_exec` is the *first* implementation; raw-CUDA or CPU-edge backends can
  implement the same seam later. This is the "future ecosystem adapts in" capability — built into the
  shape, not added on.
- **Python is one optional binding at the edge** — prototyping/setup/tests; never in the deployment loop.
- **FlashRT stays a frozen backend** (`Capsule_Serving_Integration_EN.md` describes the FlashRT *backend
  adapter* packaging at L0).

---

## 8. Capsule compatibility guard

Every capsule carries the backend's `fingerprint()`; `restore`/`restore_into`/`load` refuse on mismatch.
This makes the paper's honest boundary ("a capsule is a deployment-bound blob") an *enforced invariant* —
essential once builds are selective per arch and capsules ship between nodes.

---

## 9. Rollout — core-first

- **P0 — core C ABI + reference impl (~1 wk):** the header (§3) + a reference implementation of Capsule
  + Drive over the backend seam, plus a **stub backend (host memcpy)**. Gate: **the core builds and tests
  with zero dependency**, proving snapshot/restore/restore_into/swap/fire/fingerprint-refusal. (This is
  the contents of the local `capsule/` repo started now.)
- **P1 — flashrt backend (~1 wk):** a `backends/flashrt` adapter implementing the seam against
  `libflashrt_exec`. Gate: **drive one real model E2E through the core, token/cos exact.**
- **P2 — first scheduler + mode (~1–2 wk):** `agent` mode + one L2 scheduler; warm-start capsule restore.
  Gate: **token-exact; the core C ABI gained no field.**
- **P3 — robot-async + multi-model schedulers (~1–2 wk):** rollout/handoff on the same core. Gate:
  **episode-reset cosine 1.0; multi-rate handoff byte-equal; no core field added.**
- **P4 — multi-hardware + cloud-edge (later):** placement + capsule ship over a Link. Gate: **capsule on
  node A restored on node B, fingerprint-checked, exact.**

The core C ABI **freezes after P0**; only its backends/schedulers/modes grow.

---

## 10. One-line summary

> A sovereign, zero-dependency, frozen-after-v1 C++ core (**Capsule + imperative Drive verbs +
> Schedule-data + backend seam**) defines inference as **replay over movable state, driven imperatively**.
> Schedulers, state-services, protocols, modes, and the GPU backend are all **pluggable upper layers that
> adapt into the core** — the framework ships a set, integrators swap freely, and the core never depends
> on, locks for, or loops on behalf of any of them.
