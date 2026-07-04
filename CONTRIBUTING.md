# Contributing to FlashRT Nexus

FlashRT Nexus is a **state-first serving substrate for physical AI**: one execution-state control layer
over FlashRT replay, robot runtimes, and agent workflows.
Its core is the **capsule** execution-state mechanism — a sovereign, zero-dependency C ABI.

This document is the contract every change is held to. It is short on purpose: if a change needs a
longer justification than the rules below, it is probably in the wrong layer.

---

## 1. Layering — where code goes

```
core/             L1  the capsule core: zero-dependency C ABI + reference impl   [frozen at 1.0]
  include/capsule/capsule.h     the public core ABI (the protocol boundary)
  src/                          reference implementation
backends/         L0  capsule backend implementations
  stub/                         host-memory reference backend (tests/CI, no GPU)
  flashrt/                      FlashRT exec backend + runtime/model adapters (libflashrt_exec + CUDA)
host/             L2  the first framework surface: the standard model-runtime face
  include/capsule/model_runtime.h   cap_model_runtime (ports · stage DAG · regions · verbs)
  src/                          lookups, alloc-free tick, FFI accessors — mechanism-thin
nexus/            L2  the framework (grows here): schedulers · modes · state-services
  schedulers/                   reusable stage-DAG runners and scheduler primitives
  embedded/                     C/C++ no-HTTP session ABI for same-process control loops
  state/                        GraphStore / CapsuleStore policy over backend mechanisms
  modes/                        interaction state machines, ONE DIRECTORY PER MODE
    rtc_action_chunk/           embodied async inference (action chunking)
    (next: spec_session/ — LLM speculative-decode sessions)
serve/            L3  product shell and transports
  manifest / deployment         deployment file parsing + lifecycle opener
  transports/                   HTTP or other process-bound transports
  producer_plugins/             thin bundled examples; production plugins may live with producers
examples/         single-concept runnable demos (thin runner + README each)
scenarios/        L3  complete interaction serving assemblies (VLA hosts, LLM
                  planners, planner+actor coexistence, world models). Created
                  with its first member; an example graduates here when it
                  becomes a production-shaped host.
tests/            acceptance + smoke
internal/         design drafts — NOT public spec; gitignored
```

A change belongs to the **lowest** layer that fully contains it. If a feature wants to add a field to
a lower layer to satisfy a higher one, that field is policy and belongs higher up.

---

## 2. The red line (project invariants — non-negotiable)

1. **Mechanism, not policy.** The core encodes no scheduler, KV/paged-memory manager, protocol,
   batching, or eviction policy. Those are L2/L3 or live in a downstream product.
2. **Zero-dependency core.** L1 depends only on the C++ runtime and the abstract backend vtable.
   Real dependencies (CUDA, frameworks, transports) live outside `core/`.
3. **The core owns no loop and no thread, and takes no lock.** The loop is always a scheduler (L2).
   Hot-path verbs (`cap_fire` / `cap_swap` / `cap_sync` / `cap_capsule_ready`) perform no allocation.
   One `cap_ctx` is driven by one thread.
4. **Kernel-agnostic.** The core never sees a kernel. Any replayable unit works (a captured/adopted CUDA
   graph, or eager re-launch); a step that must return to the host becomes its own scheduler-fired stage.
5. **State is backend-owned.** The core holds handles and copies named regions; it owns no device memory
   except the capsule backing it allocated. Every capsule carries a backend fingerprint; `restore`
   refuses on mismatch.
6. **Additive-only at seams** (see §4). Existing ABI, backend entrypoints, and a backend's upstream code
   are not modified in place; new behavior is new files / methods / flags.
7. **The model-runtime face stays data-first.** `cap_model_runtime` is capsule types only — no frt
   includes above `backends/`, no model names, no scenario fields. Ports carry the update class as a
   promise: SWAP = the host writes the wired window, STAGED = the producer's verb accepts hot updates
   (advertise-and-refuse is a bug), SETUP = illegal inside a tick. The hot-input contract is testable:
   updating ports between ticks never recaptures, never allocates, never rebinds; `cap_model_tick`
   allocates nothing (pre-created events). Graph-cache MECHANISM is the backend pass-through
   (`flashrt_graph_evict*` / `variant_count`); eviction/budget POLICY is an L2 store, and eviction
   happens only at safe points (never while a variant may be in flight).
8. **RTC is an L2 mode, not a runtime feature.** The runtime producer declares ports/stages and
   hot verbs; Nexus decides when to fire, poll, splice, fallback, interrupt, or overlap.
9. **Transports are adapters, not lifecycle owners.** HTTP, ROS2, shm, and embedded loops call the
   same deployment/session semantics. They must not parse model internals, assume graph names, or own
   capture. The C embedded ABI starts from an adopted `cap_model_runtime*`; producer setup stays with
   FlashRT.
10. **No-HTTP hot paths stay data-shaped.** Embedded APIs return buffers, arrays, or POD views. JSON,
    base64, Python lists, and protocol-specific objects belong only at transport boundaries.

**PR checklist:**
- [ ] The change lives in the correct layer (§1) and adds no policy to `core/` (§2.1).
- [ ] `core/` gained no third-party dependency; hot-path verbs still allocate nothing and take no lock.
- [ ] If the C ABI changed, it is additive and follows §4 (and is justified).
- [ ] `host/` stayed capsule-typed and data-first (§2.7): no frt includes, no model names, no policy;
      hot verbs and the tick still allocate nothing; STAGED ports actually accept hot updates.
- [ ] RTC / async-loop behavior lives under `nexus/modes` or `nexus/schedulers`, never in `core/`,
      `host/`, or the FlashRT runtime producer.
- [ ] Transport changes call the common session/embedded surface and do not duplicate model-specific
      loops, graph-name assumptions, or capsule persistence policy.
- [ ] Embedded changes preserve direct buffer semantics: no JSON/base64/list conversion on the no-HTTP
      path, and one session cannot drive one `cap_ctx` concurrently.
- [ ] Persistent capsule changes preserve path filtering, malformed-blob skipping, atomic write
      semantics, and the documented live-capture bit-exact boundary.
- [ ] Builds and tests pass (§5); correctness is preserved (bit/token/cosine-exact where applicable).
- [ ] Commit & file hygiene (§6).

---

## 3. Naming conventions

| Thing | Convention | Example |
|---|---|---|
| **Core / mechanism** | `capsule` — C ABI symbols `cap_*` (functions/types), `CAP_*` (constants/enums); header `capsule/capsule.h`; lib `capsule_core` | `cap_snapshot`, `CAP_TIER_GPU` |
| **Framework / product** | `nexus` / **FlashRT Nexus** — symbols `nexus_*`, headers `nexus/*.h`, config `NEXUS_*` (as L2 lands); brand string "FlashRT Nexus" | `nexus_scheduler_*` |
| **Embedded session** | C ABI symbols `nexus_embedded_*`, header `nexus/embedded/session.h` | `nexus_embedded_step` |
| **FlashRT backend** | implements the capsule seam for FlashRT: entrypoints `flashrt_backend_init/fini`, lib `capsule_backend_flashrt`, dir `backends/flashrt/` | `flashrt_backend_init`, `capsule_backend_flashrt` |
| **FlashRT backend build toggle** | `CAPSULE_BUILD_FLASHRT_BACKEND` | `CAPSULE_BUILD_FLASHRT_BACKEND` |
| **Tests** | `test_<area>` | `test_core`, `test_flashrt_gpu` |

Rule of thumb: **`cap_` / `CAP_` / `capsule` name the L1 core** (the stable mechanism); **`nexus` / `NEXUS_`
name the product and the L2 framework.** The FlashRT backend uses the `capsule_backend_flashrt`
namespace. The short `cap_` prefix is the capsule C-ABI symbol namespace — do not rename it.

### Backend Capability Honesty

The public backend in this repository is the FlashRT backend. It provides replayable graphs over named
state, with real `snapshot` / `restore` / `restore_into` and stream/event semantics.

Do not document or imply capabilities that are not implemented by the FlashRT backend. In particular,
do not present request routing, batching, prefix-cache behavior, or remote service submission as capsule
backend features. If a capability is not in the public ABI and tests, it stays out of public docs.

---

## 4. ABI stability

- The project follows **semantic versioning**. Pre-`1.0` (the current `0.x`) the core C ABI may still
  change as it stabilizes.
- From **`1.0` the core C ABI is frozen** and evolves **additive-only**: append new functions, append new
  enum values, bump `CAP_ABI_VERSION` and the backend vtable `struct_size`. Never reorder, remove, or
  repurpose an existing field.
- The backend vtable is **versioned** (`abi_version` + `struct_size`): `cap_ctx_create` validates them, so
  an older core rejects a newer/larger vtable gracefully. Every backend vtable instance MUST set both.
- Handles crossing the ABI are **distinct opaque pointer types** (`cap_buffer`/`cap_graph`/`cap_event`),
  never `void*`. Only opaque handles, POD structs, byte buffers, and the vtable cross the boundary — no
  C++ types and no exceptions. Errors are `int` status codes.

---

## 5. Build & test (acceptance gates)

The core + stub build and test **with zero third-party dependency** (only a C++17 compiler):

```sh
cmake -S . -B build && cmake --build build -j
ctest --test-dir build --output-on-failure
```

The FlashRT backend is opt-in (needs CUDA + a built `libflashrt_exec`); its GPU smoke runs on a real GPU:

```sh
cmake -S . -B build -DCAPSULE_BUILD_FLASHRT_BACKEND=ON -DFLASHRT_EXEC_DIR=<FlashRT>/exec
cmake --build build -j && ./build/test_flashrt_gpu
./build/test_runtime_adopt      # runtime-export adoption + lifetime
./build/test_model_adopt        # model-runtime face + the hot-input contract
./build/test_nexus_l2           # L2 scheduler + RTC mode + graph store (stub backend, no GPU)
./build/test_embedded_session   # C embedded session ABI (stub backend, no GPU)
FLASHRT_DIR=<FlashRT> python tests/gate_python_producer.py   # cross-language seam
```

Changes that touch `host/` or `backends/flashrt` must keep `test_model_adopt` green — it pins the
hot-input contract (SWAP/STAGED updates between ticks, no recapture/alloc/rebind) and the adoption
lifetime. Changes under `nexus/` must keep `test_nexus_l2` green — it pins single in-flight per
stage, steady-state zero allocation, rate tables, deadline-fallback semantics, and the chunk-shape
contract. Changes under `nexus/embedded/` or no-HTTP session code must keep
`test_embedded_session` green — it pins SWAP/STAGED input, batched step, output readback,
single-session concurrency serialization, snapshot/restore, and serialize/load semantics.
The real-model gates (`tests/gate_pi05_model.py`,
`tests/gate_pi05_export.py`, `tests/gate_pi05_rtc_action_chunk.py`, `tests/gate_pi05_embedded.py`)
are the end-to-end reference; run them when the seam, the tick semantics, or the scheduling/embedded
layer change.

Gates a PR must meet: the zero-dep core build is green; all built tests pass; a new path produces output
identical to the path it replaces (bit-identical / token-exact / cosine ≥ 0.999, as applicable), with a
runnable command in the PR.

---

## 6. Commit & file hygiene

- **Clean history.** Imperative commit subjects; explain *why* in the body. **No AI/tool attribution or
  co-author trailers**, ever.
- **Consistent real author identity** on every commit.
- **English** in all public code, comments, and docs. **No local filesystem paths, machine/container/env
  names**, no secrets.
- **Never commit** build outputs, generated binaries, model checkpoints, or large artifacts (see
  `.gitignore`).
- **Additive at the seam.** Do not edit a backend's upstream kernels/bindings in place; add new files or
  functions. Changing a shared function that affects an existing path must be called out and shown to
  preserve that path's output.

---

The full design rationale is the paper *Execution-State Capsules* (arXiv:2606.20537) and the C ABI header
[`core/include/capsule/capsule.h`](core/include/capsule/capsule.h).
