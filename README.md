# capsule

**A sovereign, zero-dependency execution-state runtime for physical-AI serving.**
Checkpoint, restore, fork, and move *live inference state* — and drive multi-graph, multi-model,
interruptible inference from an imperative loop — over any kernel backend.

<p align="center">
  | <a href="https://arxiv.org/abs/2606.20537"><b>Paper: Execution-State Capsules</b></a>
  | <a href="https://github.com/LiangSu8899/FlashRT"><b>Backend: FlashRT</b></a> |
</p>

---

## What this is

Mainstream LLM serving is built around a **request pipeline**: tokenize → scheduler → continuous
batcher → paged-KV manager → model runner. That shape is optimal for many-tenant, high-throughput
datacenter LLM serving — and a poor fit for **physical AI**, where the workload is **low-latency,
small-batch, high-frequency, multi-model, often on-device, and interruptible**.

`capsule` defines a different model of inference:

> **Inference is replay of a captured graph over named state buffers. The state is a first-class
> object you can `snapshot`, `restore`, `fork`, and `move`. An imperative loop decides which graph
> fires, on which stream, when — and can interrupt at graph boundaries.**

State becomes central; the pipeline becomes trivial (fire a replay). This is the engineering
realization of the paper *Execution-State Capsules* (arXiv:2606.20537).

## The capsule — one mechanism, four verbs

A **capsule** is the full, restorable execution state at a committed boundary — a fixed set of named
buffers (KV / recurrent / conv state / diffusion seed / scales / metadata), frozen as an object.

```
   cold once                snapshot                 restore (one copy)            fork
   ┌──────────────┐        ┌──────────┐             ┌────────────┐             ┌──────────┐
   │ build state  │  ────▶ │ CAPSULE  │── restore ─▶│ warm start │      ┌─────▶│ branch 1 │
   │ (prefix /    │        │ (frozen, │             └────────────┘      │      └──────────┘
   │  episode /   │        │  movable)│── restore ─▶│ another set│ ─────┤
   │  context)    │        └──────────┘             └────────────┘      └─────▶│ branch 2 │
   └──────────────┘             │ tier / ship                                  └──────────┘
                        GPU ↔ host RAM ↔ disk ↔ another node
```

- **snapshot** — freeze a boundary (to GPU, host RAM, or disk).
- **restore** — copy it back into live buffers and keep going (warm start, undo, episode reset).
- **fork** — restore one boundary into N live sets (tree-of-thought, best-of-N, parallel hypotheses).
- **move** — serialize and ship a capsule to another node (cloud-edge hand-off, migration).

The same mechanism serves an LLM agent's warm start, a VLA diffusion policy's episode reset, a
planner→actor hand-off, and a voice assistant's barge-in. Every capsule is stamped with a backend
fingerprint (`{weights, quant, kernel, arch}`); `restore` refuses on mismatch.

## Architecture — the core executes, upper layers decide

```
 L3  ecosystem     agents · lerobot · robotics · omni · edge products       (compose / speak protocol)
 L2  framework     schedulers (robot-async · multi-model · multi-hardware) · state-services
                   (capsule store · session) · transport · modes                       [pluggable]
 L1  capsule CORE  Capsule + imperative Drive verbs + Schedule-data + backend seam   [this repo · ZERO dep]
 L0  backends      FlashRT (first) · raw-CUDA · CPU-edge · future                  (implement the seam)
```

- **The core (L1) depends on nothing** — not on a GPU runtime, not on Python. It links an abstract
  backend seam (vtable), so backends adapt *into* it. It owns **no loop and no thread**; the loop is
  always a scheduler (L2). Hot-path verbs do not allocate and do not lock.
- **Kernel-agnostic.** The core never sees a kernel. A backend's graph may contain any mix of
  HF kernels, FlashInfer, TileLang, cuBLAS, or hand-written CUDA — the only requirement is that the
  unit is replayable (a captured/adopted CUDA graph, or eager re-launch). A step that must return to
  the host (sampling, accept/reject, dynamic routing) simply becomes its own imperatively-fired stage.
- **Schedulers are pluggable.** The framework ships robot-async / multi-model / multi-hardware
  schedulers; integrators swap or write their own. The core never depends on them.

The authoritative spec is the C ABI header [`core/include/capsule/capsule.h`](core/include/capsule/capsule.h)
and [`docs/Capsule_Core_Spec_EN.md`](docs/Capsule_Core_Spec_EN.md) ([中文](docs/Capsule_Core_Spec_ZH.md)).
Design rationale: [`docs/Capsule_Serving_Design_EN.md`](docs/Capsule_Serving_Design_EN.md); how it
references FlashRT: [`docs/Capsule_Serving_Integration_EN.md`](docs/Capsule_Serving_Integration_EN.md).

## Status

| phase | what | state |
|---|---|---|
| **P0** | zero-dependency core C ABI + reference impl + host stub backend + acceptance test | **done** — 28/28 checks, builds with no third-party dep |
| **P1** | FlashRT backend (`backends/flashrt/`, over `libflashrt_exec` + CUDA) | **done** — GPU smoke 12/12 (capture/replay + capsule snapshot/restore/restore_into across tiers + fingerprint guard) |
| P2 | first L2 scheduler + agent mode; warm-start over a real model | next |
| P3 | robot-async + multi-model schedulers (rollout / planner→actor) | planned |
| P4 | multi-hardware scheduler + cloud-edge capsule ship | planned |

## Quickstart

The core + stub (no GPU, no third-party dependency — needs only a C++17 compiler):

```sh
cmake -S . -B build && cmake --build build -j
ctest --test-dir build --output-on-failure
```

The FlashRT backend (needs CUDA + a built `libflashrt_exec`):

```sh
cmake -S . -B build -DCAPSULE_BUILD_FLASHRT_BACKEND=ON -DFLASHRT_EXEC_DIR=<FlashRT>/exec
cmake --build build -j && ./build/test_flashrt_gpu      # requires a GPU
```

## Relationship to FlashRT

[FlashRT](https://github.com/LiangSu8899/FlashRT) is the inference engine — hand-written kernels
composed into static CUDA graphs, with a minimal execution contract (`libflashrt_exec`). `capsule`
is the serving layer *above* it: it consumes FlashRT **unchanged** as its first backend and adds the
state + schedule + lifecycle runtime. FlashRT stays a backend; `capsule` is backend-agnostic.

## Non-goals

`capsule` is mechanism, not policy. It is **not** a scheduler, a KV/paged-memory manager, a compiler,
a protocol, or a batching engine — those are pluggable upper layers or live elsewhere. The core
encodes no policy, owns no GPU memory it didn't allocate for a capsule, spawns no thread, and (after
v1) freezes its ABI to additive-only. See the non-goals manifesto in
[`docs/Capsule_Core_Spec_EN.md`](docs/Capsule_Core_Spec_EN.md#6-what-we-dont-do-non-goals--the-negative-space).

## Citation

```bibtex
@misc{su2026executionstatecapsules,
  title={Execution-State Capsules: Graph-Bound Execution-State Checkpoint and Restore for Low-Latency, Small-Batch, On-Device Physical-AI Serving},
  author={Liang Su},
  year={2026},
  eprint={2606.20537},
  archivePrefix={arXiv},
  primaryClass={cs.LG},
  doi={10.48550/arXiv.2606.20537},
  url={https://arxiv.org/abs/2606.20537},
}
```

## License

Apache License 2.0 — see [LICENSE](LICENSE).
