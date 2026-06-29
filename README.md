# capsule

A sovereign, **zero-dependency** C++ core that *defines* inference for physical AI:
**replay a captured graph over named state buffers; the state is a first-class object you can
snapshot, restore, branch, and move; an imperative loop decides which graph fires, on which stream,
when — and can interrupt at graph boundaries.**

Everything else — schedulers, protocols, modes, transport, and the GPU backend itself — is a
**pluggable upper layer that adapts *into* the core**. The core executes; the upper layers decide.
The core holds; the framework lives above it.

## Layering

```
 L3  ecosystem      agents · lerobot · PKU · omni · robot products      (compose / speak protocol)
 L2  framework      schedulers (robot-async / multi-model / multi-hardware) · state-services
                    (CapsuleStore / Session) · adapters (transport / backends) · modes   [pluggable]
 L1  capsule CORE   Capsule + imperative Drive verbs + Schedule-data + backend seam   [this repo; zero dep]
 L0  backends       flashrt (first) · raw-cuda · cpu-edge · stub (tests)              (implement the seam)
```

The core is the spec in [`core/include/capsule/capsule.h`](core/include/capsule/capsule.h) — the
authoritative protocol boundary. Design rationale lives in the `capsule_paper` repo
(`Capsule_Core_Spec`, `Capsule_Serving_Design`, `Capsule_Serving_Integration`).

## What is here (P0)

| path | what |
|---|---|
| `core/include/capsule/capsule.h` | the C ABI — the frozen-after-v1 protocol boundary |
| `core/src/capsule.cpp` | reference implementation of Capsule + Drive over the backend seam |
| `backends/stub/` | a host-memory backend (malloc + memcpy) so the core builds/tests with no GPU |
| `tests/test_core.cpp` | P0 acceptance: snapshot/restore/restore_into/regions/tier_move/serialize/load + fingerprint guard + fire/drive_tick/swap |

The real GPU backend (`backends/flashrt/`, implementing the seam against `libflashrt_exec`) and the
L2 schedulers/modes land in later phases — see the rollout in `Capsule_Core_Spec`.

## Build & test

```sh
cmake -S . -B build && cmake --build build -j
ctest --test-dir build --output-on-failure      # or: ./build/test_core
```

No third-party dependencies; needs only a C++17 compiler.

## The red line (constraints)

- The core depends on **nothing**; it links an abstract backend vtable, not a backend.
- The core owns **no loop and no thread**; the loop is always an upper layer (a scheduler).
- Hot-path verbs (`cap_fire` / `cap_swap` / `cap_sync` / `cap_capsule_ready`) **do not allocate and do
  not lock**. One `cap_ctx` is driven by one thread.
- The core encodes **no policy** — no scheduler, KV manager, protocol, batching, or eviction.
- Only opaque handles, POD structs, byte buffers, and the backend vtable cross the C ABI. No C++
  types, no exceptions.
- After v1 the ABI is **frozen**: additive only (append functions / enum values, bump
  `CAP_ABI_VERSION` + the vtable `struct_size`).
