# Contributing to FlashRT Nexus

FlashRT Nexus is a **state-first serving substrate for physical AI**: one execution-state control layer
that connects replayable edge backends, cloud serving engines, robot runtimes, and agent workflows.
Its core is the **capsule** execution-state mechanism — a sovereign, zero-dependency C ABI.

This document is the contract every change is held to. It is short on purpose: if a change needs a
longer justification than the rules below, it is probably in the wrong layer.

---

## 1. Layering — where code goes

```
core/             L1  the capsule core: zero-dependency C ABI + reference impl   [frozen at 1.0]
  include/capsule/capsule.h     the public core ABI (the protocol boundary)
  src/                          reference implementation
backends/         L0  capsule backends (implement the seam); one subdir per backend
  stub/                         host-memory reference backend (tests/CI, no GPU)
  flashrt/                      FlashRT exec backend (libflashrt_exec + CUDA)
  <future>/                     raw-cuda · cpu-edge · cloud (vLLM / SGLang) ...
nexus/            L2  the framework (grows here): schedulers · modes · state-services
  schedulers/                   robot-async · multi-model · multi-hardware
  modes/                        agent · rollout · handoff · duplex
  transport/                    L3 adapters: openai · grpc · ros2 · shared-mem
  include/nexus/                the framework API (nexus_* symbols)
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
   Real dependencies (CUDA, frameworks, transports) live in L0 backends or L2/L3 adapters — never in `core/`.
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

**PR checklist:**
- [ ] The change lives in the correct layer (§1) and adds no policy to `core/` (§2.1).
- [ ] `core/` gained no third-party dependency; hot-path verbs still allocate nothing and take no lock.
- [ ] If the C ABI changed, it is additive and follows §4 (and is justified).
- [ ] Builds and tests pass (§5); correctness is preserved (bit/token/cosine-exact where applicable).
- [ ] Commit & file hygiene (§6).

---

## 3. Naming conventions

| Thing | Convention | Example |
|---|---|---|
| **Core / mechanism** | `capsule` — C ABI symbols `cap_*` (functions/types), `CAP_*` (constants/enums); header `capsule/capsule.h`; lib `capsule_core` | `cap_snapshot`, `CAP_TIER_GPU` |
| **Framework / product** | `nexus` / **FlashRT Nexus** — symbols `nexus_*`, headers `nexus/*.h`, config `NEXUS_*` (as L2 lands); brand string "FlashRT Nexus" | `nexus_scheduler_*` |
| **Backend** | implements the capsule seam: entrypoints `<name>_backend_init/fini`, lib `capsule_backend_<name>`, dir `backends/<name>/` | `flashrt_backend_init`, `capsule_backend_flashrt` |
| **Backend build toggle** | `CAPSULE_BUILD_<NAME>_BACKEND` (selects a capsule backend) | `CAPSULE_BUILD_FLASHRT_BACKEND` |
| **Tests** | `test_<area>` | `test_core`, `test_flashrt_gpu` |

Rule of thumb: **`cap_` / `CAP_` / `capsule` name the L1 core** (the stable mechanism); **`nexus` / `NEXUS_`
name the product and the L2 framework.** Backends are *capsule backends* and use the `capsule_backend_*`
namespace. The short `cap_` prefix is the capsule C-ABI symbol namespace — do not rename it.

### Backend classes (capability honesty)

Not every backend offers the same fidelity. Be honest about which class a backend is, and **never claim
a capability a backend does not actually implement**:

- **Native replay / capsule backend** (e.g. FlashRT, raw-CUDA): replayable graphs over named state, with
  real `snapshot` / `restore` / `restore_into` and stream/event semantics. This is the high-fidelity class
  the design targets — it can checkpoint and move live execution state.
- **Managed engine backend** (e.g. vLLM / SGLang): a remote/throughput serving engine. It may offer submit,
  prefix-cache, batching, health, and cancel — but it does **not** provide graph-bound execution-state
  checkpoint/restore. It is wired as a routing/fallback target, **not** as a capsule backend, and must not
  be presented as one.

When the second class lands, capabilities will be declared explicitly (a backend-advertised capability set)
rather than inferred — that field is intentionally **not** frozen now (one real backend today; adding it
pre-1.0 is additive). Until then, this honesty rule is the contract.

---

## 4. ABI stability

- The project follows **semantic versioning**. Pre-`1.0` (the current `0.x`) the core C ABI may still
  change as it stabilizes.
- From **`1.0` the core C ABI is frozen** and evolves **additive-only**: append new functions, append new
  enum values, bump `CAP_ABI_VERSION` and the backend vtable `struct_size`. Never reorder, remove, or
  repurpose an existing field.
- The backend vtable is **versioned** (`abi_version` + `struct_size`): `cap_ctx_create` validates them, so
  an older core rejects a newer/larger vtable gracefully. Every backend MUST set both.
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
```

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
