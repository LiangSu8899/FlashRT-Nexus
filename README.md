# FlashRT Nexus

**A state-first serving substrate for physical AI.**

FlashRT Nexus connects replayable edge backends, cloud serving engines, robot runtimes, and agent
workflows through one **execution-state control layer** — so a system can checkpoint, restore, fork,
and move *live inference state* across models, schedulers, and hardware.

<p align="center">
  | <a href="https://arxiv.org/abs/2606.20537"><b>Paper: Execution-State Capsules</b></a>
  | <a href="https://github.com/LiangSu8899/FlashRT"><b>FlashRT (edge backend)</b></a> |
</p>

---

## Why Nexus

Mainstream LLM serving is built around a **request pipeline** — tokenize → scheduler → continuous
batcher → paged-KV manager → runner — optimal for many-tenant, high-throughput datacenter LLM serving.
**Physical AI is a different workload:** low-latency, small-batch, high-frequency, multi-model,
often on-device, interruptible, and increasingly **split across edge, cloud, and robot**.

Nexus is the connecting layer that workload needs. Its through-line is **state-first execution**:

> Inference is replay of a captured graph over named state buffers. The state is a first-class
> object you can **snapshot, restore, fork, and move**. An imperative loop decides which graph fires,
> on which stream, when — and can interrupt at graph boundaries.

State becomes the unit of control and the unit of mobility. That is the moat — and the engineering
realization of the paper *Execution-State Capsules* (arXiv:2606.20537). Inside Nexus, that mechanism
is named **capsule**.

## What it connects

```
        ECOSYSTEM        LeRobot · OpenPI · Isaac · agent workflows · omni        (compose / speak protocol)
            ▲
            │  modes · transport (OpenAI / gRPC / ROS2 / shared-mem)
   ┌────────┴───────────────────────────────────────────────────────────────┐
   │  FlashRT NEXUS  —  state-first execution-state control layer             │
   │    capsule (snapshot/restore/fork/move) · imperative Drive · Schedule    │
   │    schedulers: robot-async · multi-model · multi-backend / multi-hardware│
   └────────┬───────────────────────────────────────────────────────────────┘
            │  one backend seam (vtable) — backends adapt IN
            ▼
        BACKENDS    edge replayable: FlashRT   ·   cloud throughput: vLLM / SGLang
                    raw-CUDA · CPU-edge · future
```

- **One execution-state control layer, many backends.** A backend implements a small seam (buffers,
  graphs, streams, events). FlashRT — hand-written kernels in static CUDA graphs — is the first,
  wired and GPU-validated. Cloud throughput engines (vLLM / SGLang) and other runtimes are the kind of
  backend Nexus is built to connect next.
- **One layer, many ecosystems.** LeRobot, OpenPI, Isaac, and agent stacks plug in above via modes and
  transports; none of them has to know which backend or hardware is underneath.
- **State-first, everywhere.** The same capsule serves an LLM agent's warm start, a VLA policy's
  episode reset, a planner→actor hand-off, a voice barge-in, and a cloud→edge hand-off.

## The capsule — one mechanism, four verbs

A **capsule** is the full, restorable execution state at a committed boundary — a fixed set of named
buffers (KV / recurrent / conv state / diffusion seed / scales / metadata), frozen as an object.

```
   build once               snapshot                 restore (one copy)            fork
   ┌──────────────┐        ┌──────────┐             ┌────────────┐             ┌──────────┐
   │ prefix /     │  ────▶ │ CAPSULE  │── restore ─▶│ warm start │      ┌─────▶│ branch 1 │
   │ episode /    │        │ (frozen, │             └────────────┘      │      └──────────┘
   │ context      │        │  movable)│── restore ─▶│ another set│ ─────┤
   └──────────────┘        └──────────┘             └────────────┘      └─────▶│ branch 2 │
                                │ tier / ship                                  └──────────┘
                       GPU ↔ host RAM ↔ disk ↔ another node
```

**snapshot** freeze a boundary · **restore** warm-start / undo / episode-reset · **fork** one boundary
into N live sets · **move** serialize and ship to another node. Every capsule is stamped with a backend
fingerprint (`{weights, quant, kernel, arch}`); `restore` refuses on mismatch.

## Architecture — the core executes, upper layers decide

- **The core depends on nothing** — not on a GPU runtime, not on Python. It links an abstract backend
  seam, so backends adapt *into* it. It owns **no loop and no thread**; the loop is always a scheduler.
  Hot-path verbs do not allocate and do not lock.
- **Kernel-agnostic.** The core never sees a kernel. A backend's graph may mix HF kernels, FlashInfer,
  TileLang, cuBLAS, or hand-written CUDA — the only requirement is that the unit is replayable. A step
  that must return to the host (sampling, accept/reject, dynamic routing) becomes its own
  imperatively-fired stage.
- **Pluggable schedulers.** Nexus ships robot-async / multi-model / multi-hardware schedulers;
  integrators swap or write their own. The core never depends on them.

The authoritative spec is the C ABI header
[`core/include/capsule/capsule.h`](core/include/capsule/capsule.h). Rationale is in the paper above.

## Status

| phase | what | state |
|---|---|---|
| **P0** | zero-dependency core C ABI + reference impl + host stub backend + acceptance test | **done** — 28/28 checks, no third-party dep |
| **P1** | FlashRT backend (over `libflashrt_exec` + CUDA) | **done** — GPU smoke 12/12 (capture/replay + snapshot/restore/restore_into across tiers + fingerprint guard) |
| P2 | first scheduler + agent mode; warm-start over a real model | next |
| P3 | robot-async + multi-model schedulers (rollout / planner→actor) | planned |
| P4 | multi-backend (cloud engines) + multi-hardware + cloud-edge capsule ship | planned |

## Quickstart

Core + stub (no GPU, no third-party dependency — needs only a C++17 compiler):

```sh
cmake -S . -B build && cmake --build build -j
ctest --test-dir build --output-on-failure
```

FlashRT backend (needs CUDA + a built `libflashrt_exec`):

```sh
cmake -S . -B build -DCAPSULE_BUILD_FLASHRT_BACKEND=ON -DFLASHRT_EXEC_DIR=<FlashRT>/exec
cmake --build build -j && ./build/test_flashrt_gpu      # requires a GPU
```

## Relationship to FlashRT

[FlashRT](https://github.com/LiangSu8899/FlashRT) is the inference engine — hand-written kernels
composed into static CUDA graphs, exposing a minimal execution contract (`libflashrt_exec`). Nexus is
the serving substrate *above* it: it consumes FlashRT **unchanged** as its first backend and adds the
state + schedule + lifecycle control layer. FlashRT stays a backend; Nexus is backend-agnostic.

## Non-goals

Nexus's core is mechanism, not policy. It is **not** a scheduler, a KV/paged-memory manager, a compiler,
a protocol, or a batching engine — those are pluggable upper layers or live elsewhere. The core encodes
no policy, owns no GPU memory it didn't allocate for a capsule, spawns no thread, and (after v1) freezes
its ABI to additive-only.

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
