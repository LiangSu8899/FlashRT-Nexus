# Capsule — Integration & Packaging Addendum (how `capsule` references FlashRT, and carries the ecosystems)

> Status: design addendum · 2026-06-29 · companion to `Capsule_Serving_Design_EN.md`
> Answers the concrete seam questions: how the new `capsule` repo references FlashRT, what
> FlashRT must normalize, how build/hardware/selective-compilation are owned, and how the
> serving substrate connects to downstream ecosystems (lerobot, PKU, agents, omni).
>
> ⚠️ **Posture revised by `Capsule_Core_Spec_EN.md` (the canonical core).** The core is **C++,
> zero-dependency, sovereign**; FlashRT is **the first backend behind an abstract seam (L0), not a
> dependency of the core**. Read this addendum as **how the FlashRT *backend adapter* is packaged
> and selectively built** (arch × model, fingerprint, the three-way ownership) — all of which still
> holds for L0. Wherever this doc says "capsule is pure Python / depends on flash_rt," substitute:
> the core is C++ and depends on nothing; Python is one optional edge binding; the flash_rt
> dependency belongs to the `backends/flashrt` adapter, not the core.

---

## 0. Decision reversal — FlashRT stays frozen (supersedes main design §10)

The main design's §10 said "move serving infra out of FlashRT." **That is reversed.** The
revised, better decision:

> **FlashRT stays frozen and non-expanding.** It is the *library*: kernels + the exec contract +
> basic runtime + tests + a playground for adding models and validating demos. Its `serving/`
> examples **stay as demos, untouched.** **`capsule` is built fresh** as the ecosystem-carrying /
> definition layer and **references FlashRT as a dependency** — it does not cut code out of FlashRT.

The reusable cores (CapsuleStore, SessionRegistry, PrefixPlan, engine protocol) are
**re-implemented clean** in `capsule/core/`, using FlashRT's `serving/qwen36_agent` as *reference*,
not as donor code. Some conceptual duplication (FlashRT's demo keeps its own small SessionRegistry;
capsule has the production one) is intended: **zero churn in FlashRT, zero circular dependency.**

Why this is cleaner: FlashRT remains a self-contained backend + community playground; `capsule`
owns all serving evolution; the two never tangle.

---

## 1. Key insight — `capsule` has no compile of its own

This is what dissolves most of the "lots of prerequisite work" fear:

```
   ECOSYSTEMS ──(protocol | library | embed)──────────────┐
                                                           │
   capsule/  (PURE PYTHON — owns NO kernels, NO C++ build) │
     core/                ─ logical dep: the exec C ABI ───┼──┐
     adapters/engines/<m> ─ dep: flash_rt frontend+kernels │  │ (selective)
     adapters/transport/  ─ openai / grpc / ros2 / shm     │  │
   ────────────────────────────────────────────────────────┼──┼─ runtime load (ctypes / import)
   FlashRT  (frozen library — builds the artifacts)         │  │
     libflashrt_exec.so   ◀───────────────────────────────────┘  tiny, arch-light, ZERO csrc dep
     flash_rt[<model>]  frontends + csrc kernels  ◀──────────┘   selective build: arch × models
     serving/  (demos — stay, untouched)
```

Because the exec layer is **already** a sibling of `csrc/` with zero kernel dependency
(exec_contract §5), and `capsule` owns no kernels by red line, **`capsule` is just a pip-installable
Python package that loads FlashRT artifacts at runtime.** The heavy CUDA build is *entirely*
FlashRT's, done once per arch, selectively. `capsule` itself has nothing to compile.

---

## 2. How `capsule` references FlashRT (packaging — decided)

**Decision: depend on the `flash_rt` package with selective extras.** No separate wheel split in v1.

```
pip install flash_rt[qwen36,pi05]      # only those frontends
# FlashRT built once for the target arch:
#   cmake -B build -DGPU_ARCH=120 && cmake --build build -j --target <model targets>
pip install capsule                    # pure python; depends on a pinned flash_rt
```

- The exec `.so` ships **inside** the `flash_rt` wheel; `capsule/core/` reaches it via
  `flash_rt.runtime.exec` (ctypes). The *logical* dependency of `core/` is only the exec C ABI;
  the *physical* package is `flash_rt` for now.
- `capsule` **pins** a known `flash_rt` version/SHA and tests against it (frontend APIs are fluid;
  pinning + conformance is normal hygiene).

**The engine adapter is the coupling shock-absorber.** `capsule/adapters/engines/qwen36.py` is the
*only* place that touches FlashRT frontend internals (`snapshot_capsule`, `prefill_own_…`, graph /
buffer handles). When a FlashRT frontend changes, you update that one adapter — FlashRT stays
clean, and `capsule/core/` never sees a frontend method. **The unstable surface is absorbed in
`capsule`, not pushed onto FlashRT.**

**Deferred (only if a real need appears):** split a tiny **`flashrt-exec`** wheel
(`libflashrt_exec.so` + `exec.h` + the ctypes wrapper, zero kernels) so an *orchestration-only* node
or CI can run `capsule` core with no kernel build. The seam already exists (exec is independent);
build the split only when a pure-coordinator / edge node is real — not now.

---

## 3. Hardware / compile / selective — the three-way ownership split

This dissolves the "selective compilation, not full-library reference" worry cleanly:

| Concern | Owner | Mechanism |
|---|---|---|
| **arch** (sm_120/121/89) | **FlashRT build** | `-DGPU_ARCH=` + per-model build targets (already exists) |
| **which models** | **capsule deployment manifest** | declarative `{models × arch × precision}` → drives FlashRT's selective build / picks a prebuilt wheel |
| **device / placement** (which GPU, edge vs cloud) | **capsule placement** | `Node` = a process bound to a device, hosting engines |
| **compatibility** (does the loaded `.so` match?) | **capsule load-time check** | build-fingerprint guard (§5) |

So: **arch → FlashRT; device → capsule placement; selection → capsule manifest; compat → capsule
load check.** `capsule` compiles nothing; it *selects* and *verifies*. The deployment manifest is the
single declarative knob:

```yaml
# capsule deployment manifest (example)
arch: sm_120
engines:
  - model: qwen36   precision: nvfp4
  - model: pi05     precision: fp8
# → resolves to the minimal flash_rt[...] install + GPU_ARCH build, nothing more
```

---

## 4. What FlashRT must "normalize" — packaging hygiene only (decided: zero-touch)

The required scope is near-zero; FlashRT stays frozen.

**Required (packaging hygiene):**
- `flash_rt` installs cleanly as a dependency; the exec `.so` is bundled and loadable via
  `flash_rt.runtime.exec`; a **version is queryable** (so `capsule` can pin + check at load).
- Selective build documented: `GPU_ARCH=<sm>` × the model→target map (mostly already in CMake —
  just written down).

**Explicitly NOT done (deferred / out of scope):**
- The optional "servable frontend descriptor" (a declared capability/graph/buffer manifest +
  conformance test) is **deferred.** Without it, `capsule`'s per-model engine adapter absorbs all
  frontend variation **by hand** — which keeps FlashRT *completely* frozen, matching the
  non-expansion preference. Revisit only if hand-writing adapters becomes a community bottleneck.
- No serving/ migration, no restructure, no new abstractions in FlashRT.

Net: the only "prerequisite work" on FlashRT is confirming it installs as a clean dependency — most
of which is already true.

---

## 5. The capsule compatibility guard (the safety net for selective/multi-arch builds)

A serialized Capsule (state blob) is bound to exact `{weights, quant, kernel version, arch, graph
bucketing}`. So every Capsule is **stamped with a build fingerprint** (derived from the engine
adapter's reported identity + exec ABI version + arch), and `restore` **refuses on mismatch**.

This turns the paper's honest boundary (§8: "a capsule is a binary blob bound to a deployment") from
a footgun into an enforced invariant — essential once builds are selective per arch and capsules
ship between nodes (cloud-edge).

---

## 6. How `capsule` carries the ecosystems (the upward surface)

`capsule` is a **double-sided adapter**: a stable engine contract *below* (to FlashRT, via the
shock-absorber adapters), and three integration levels *above* (to ecosystems). A partner picks the
level by how tightly they need to couple:

```
  Level 1  PROTOCOL  ── talk to a capsule server (OpenAI/SSE · gRPC · ROS2 · shm-link)
           loosest      zero capsule code in the ecosystem
           → agent apps, omni, a remote robot brain

  Level 2  LIBRARY   ── import capsule, compose the 5 abstractions / use a Mode
           medium       custom serving logic, in-process
           → PKU (async + multi-subgraph + RL rollout + deterministic replay + fork),
             robot-company custom orchestration

  Level 3  EMBED     ── link the (future) C++ Runtime, no Python in the loop
           tightest     fully-embedded on-robot / edge
           → lerobot real-time actor, on-device deployment
```

Mapping the actual partners:

| Ecosystem | Level | What capsule provides | What the partner brings |
|---|---|---|---|
| **lerobot / robot-company deploy** | 1 (ROS2/shm) or 3 (embed) | episode-reset-as-restore · multi-rate planner→actor handoff · interrupt | sensors · drivers · action cadence · device |
| **PKU (async, multi-subgraph, training)** | 2 (library) | `Schedule` (async multi-subgraph) · `CapsuleStore` (fork + deterministic RL-data replay) · `Runtime` (async) | training loop · data pipeline · algorithm |
| **agent apps / workstation / omni** | 1 (transport) or 2 (embed) | warm-start capsules · session/context · barge-in · streaming | UX · tools · product logic |

The contract in both directions is small and stable: **below**, one engine adapter per model family
(the only place coupled to FlashRT internals); **above**, the Modes + transport adapters + a thin
SDK. `capsule`'s whole job is to keep both contracts thin and let everything else compose.

---

## 7. The bounded v1 path (the prerequisite work, scoped)

1. `capsule` repo skeleton: `core/` (5 abstractions, fresh) + `adapters/engines/qwen36.py` (first
   shock-absorber) + `pyproject.toml` depending on a **pinned `flash_rt`**.
2. FlashRT packaging hygiene only: confirm `flash_rt` installs as a dep, exec `.so` loads, version
   queryable. (Likely ~90% true already.)
3. `capsule` **deployment manifest** + **build-fingerprint guard**.
4. Prove one mode (`agent`) E2E through `capsule → flash_rt`, **token-exact**. Then add the `pi05`
   adapter + `rollout` / `handoff` modes.

No FlashRT restructuring, no `capsule` CUDA build, no full-library reference. The selective story is
exactly: **manifest → FlashRT selective build → capsule loads + verifies.**

---

## 8. One-line summary

> `capsule` is a **pure-Python ecosystem layer** that pip-depends on a pinned, selectively-built
> `flash_rt`, reaches its exec C ABI through one thin per-model adapter (the coupling shock-absorber),
> stamps every Capsule with a build fingerprint for safe restore, and exposes the ecosystems three
> ways (protocol / library / embed) — while FlashRT stays a frozen backend and demo playground.
