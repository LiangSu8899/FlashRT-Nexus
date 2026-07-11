# FlashRT Nexus

**A state-first serving substrate for physical AI.**

FlashRT Nexus connects FlashRT replay, robot runtimes, and agent workflows through one
**execution-state control layer** — so a system can checkpoint, restore, fork, and move
*live inference state* across schedulers and runtime loops.

<p align="center">
  | <a href="https://arxiv.org/abs/2606.20537"><b>Paper: Execution-State Capsules</b></a>
  | <a href="https://github.com/flashrt-project/FlashRT"><b>FlashRT</b></a> |
</p>

---

## Why Nexus

Most serving stacks are built around a **request pipeline** — parse a request, enqueue it, run a
model, return a result. **Physical AI is a different workload:** low-latency, small-batch,
high-frequency, stateful, often on-device, and interruptible.

Nexus is the connecting layer that workload needs. Its through-line is **state-first execution**:

> Inference is replay of a captured graph over named state buffers. The state is a first-class
> object you can **snapshot, restore, fork, and move**. An imperative loop decides which graph fires,
> on which stream, when — and can interrupt at graph boundaries.

State becomes the unit of control and the unit of mobility. Nexus is the engineering realization of
the paper *Execution-State Capsules* (arXiv:2606.20537); inside the system, that mechanism is named
**capsule**.

## What It Connects

```
        APPLICATIONS     robot control · rollout · planner/actor · agent workflows
            ▲
            │  modes · host loops · transport
   ┌────────┴───────────────────────────────────────────────────────────────┐
   │  FlashRT NEXUS  —  state-first execution-state control layer             │
   │    capsule (snapshot/restore/fork/move) · imperative Drive · Schedule    │
   │    schedulers: robot-async · multi-model · interruption-aware loops      │
   └────────┬───────────────────────────────────────────────────────────────┘
            │  FlashRT backend seam
            ▼
        FlashRT     replayable graphs · named buffers · streams/events
```

- **One execution-state control layer over FlashRT.** The FlashRT backend implements the seam Nexus
  needs: buffers, graphs, streams, and events. The core then drives replay and capsules through stable
  handles.
- **One layer for runtime loops.** Robot control, rollout, planner/actor hand-off, and agent workflows
  plug in above via modes and host loops; they do not need to know the model's internal graph or buffer
  layout.
- **State-first, everywhere.** The same capsule serves an LLM agent's warm start, a VLA policy's
  episode reset, a planner→actor hand-off, and a voice barge-in.

## The capsule — one mechanism, four verbs

A **capsule** is the full, restorable execution state at a committed boundary — a fixed set of named
buffers (KV / recurrent / conv state / diffusion seed / scales / metadata), frozen as an object.

**snapshot** freeze a boundary · **restore** warm-start / undo / episode-reset · **fork** one boundary
into N live sets · **move** serialize and ship to another node. Every capsule is stamped with a backend
fingerprint (`{weights, quant, kernel, arch}`); `restore` refuses on mismatch.

These verbs map to the core C ABI as: snapshot → `cap_snapshot`; restore → `cap_restore`; fork →
`cap_restore_into` (into N caller-supplied live sets); move → `cap_serialize` / `cap_load` (+ `cap_tier_move`
for GPU↔host↔disk). `fork` and `move` are compositions over those primitives, not distinct ABI calls.

## Architecture — the core executes, upper layers decide

- **The core depends on nothing** — not on a GPU runtime, not on Python. FlashRT connects through the
  backend seam. The core owns **no loop and no thread**; the loop is always a scheduler. Hot-path verbs
  do not allocate and do not lock.
- **Kernel-agnostic.** The core never sees a kernel. A FlashRT graph may mix HF kernels, FlashInfer,
  TileLang, cuBLAS, or hand-written CUDA — the only requirement is that the unit is replayable. A step
  that must return to the host (sampling, accept/reject, dynamic routing) becomes its own
  imperatively-fired stage.
- **Pluggable schedulers.** Nexus keeps schedulers above the core. The core never depends on them.

The authoritative spec is the C ABI header
[`core/include/capsule/capsule.h`](core/include/capsule/capsule.h). Rationale is in the paper above.

## Quickstart

### HTTP demo

Start a Pi0.5 action endpoint from a deployment manifest:

```sh
export FLASHRT_DIR=/path/to/FlashRT
export PI05_CHECKPOINT=/path/to/pi05_checkpoint
export PI05_LIB=$FLASHRT_DIR/cpp/build/libflashrt_cpp_pi05_c.so
export NEXUS_LIB=./build/libcapsule_nexus_flashrt.so

bin/nexus serve examples/pi05_libero.yaml
curl http://127.0.0.1:8080/healthz
```

The HTTP route is for demos, remote debugging, and simple integration. It
exposes Act API requests, state inspection, and session snapshot/reset. Full
usage: [`docs/serve.md`](docs/serve.md).

### No-HTTP embedded

Use the same manifest inside a local control loop without an HTTP server:

```sh
python examples/pi05_embedded.py examples/pi05_libero.yaml
python tests/gate_pi05_embedded.py --iters 32
```

The Python embedded API is [`serve.embedded.EmbeddedSession`](docs/embedded.md):
it accepts image/state arrays directly and returns action arrays without
JSON/base64/socket/list-conversion overhead.

For C++ robot loops and transport adapters, use
[`nexus/embedded/session.h`](nexus/embedded/session.h), documented in
[`docs/cpp_embedded.md`](docs/cpp_embedded.md). A ROS2/shm/camera-SDK adapter
maps incoming buffers to `nexus_embedded_input[]`, maps action destinations to
`nexus_embedded_output[]`, and calls `nexus_embedded_step()` once per control
tick.

## Documentation

- [`docs/changes.md`](docs/changes.md): compatibility and migration notes.
- [`docs/serve.md`](docs/serve.md): HTTP transport, Act API, manifest, demo deployment.
- [`docs/embedded.md`](docs/embedded.md): no-HTTP Python entry for same-process loops.
- [`docs/cpp_embedded.md`](docs/cpp_embedded.md): C/C++ session ABI for ROS2/shm/local control loops.
- [`docs/mindon_pi05_embedded.md`](docs/mindon_pi05_embedded.md): Mindon Pi0.5 C++ runner integration lanes and Nexus boundary.
- [`docs/model_runtime.md`](docs/model_runtime.md): ports, stages, hot-input contract, cache discipline.
- [`docs/modes.md`](docs/modes.md): mode authoring contract.
- [`docs/adaptation_map.md`](docs/adaptation_map.md): where new models, transports, schedulers, and modes plug in.

## Development Gates

Core + stub (no GPU, no third-party dependency — needs only a C++17 compiler):

```sh
cmake -S . -B build && cmake --build build -j
ctest --test-dir build --output-on-failure
```

FlashRT backend (needs CUDA + a built `libflashrt_exec`):

```sh
cmake -S . -B build -DCAPSULE_BUILD_FLASHRT_BACKEND=ON -DFLASHRT_EXEC_DIR=<FlashRT>/exec
cmake --build build -j && ./build/test_flashrt_gpu      # requires a GPU
./build/test_runtime_adopt                              # runtime-export adoption
FLASHRT_DIR=<FlashRT> python tests/gate_python_producer.py   # cross-language seam
```

## Adopting a model: the runtime export

A FlashRT model runtime hands Nexus one struct — `frt_runtime_export_v1`
(FlashRT `runtime/include/flashrt/runtime.h`): context, streams, graphs,
buffers, restorable state regions, and a producer-computed identity
fingerprint. One call wires it:

```c
flashrt_runtime_binding rb;
flashrt_adopt_runtime_export(exp, &rb);        /* validates ABI, retains, wires */
cap_ctx c = cap_ctx_create(&rb.backend);       /* capsules now stamped with the
                                                  producer's fingerprint */
cap_stage st = { flashrt_runtime_graph(&rb, "infer"), key,
                 flashrt_runtime_stream(&rb, "main"), 0, 1, 1, CAP_EVERY };
cap_fire(c, &st);
cap_boundary bnd = { rb.regions, (int)rb.n_regions, NULL, 0 };
cap_capsule cap = cap_snapshot(c, &bnd, CAP_TIER_HOST, 0);
```

Nexus never learns what the model is, who captured it, or in which language.
The export may come from a resident Python setup process or from a native
FlashRT model runtime; this side does not change. The adapter lives in
[`backends/flashrt/flashrt_runtime_adapter.h`](backends/flashrt/flashrt_runtime_adapter.h).

## The tickable model: ports, stages, hot inputs

A production tick also needs dynamic inputs — that is the **standard
model-runtime face**, [`host/include/capsule/model_runtime.h`](host/include/capsule/model_runtime.h)
(the first L2 surface; schedulers and host loops code against it in capsule
types only):

```c
cap_model_runtime* m;
flashrt_adopt_model_runtime(model, &m);        /* frt_model_runtime_v1 in   */
cap_ctx c = cap_ctx_create(cap_model_backend(m));

int obs = cap_model_find_port(m, "obs");       /* SWAP port = wired buffer  */
cap_swap(c, m->ports[obs].buffer, frame, n, m->stages[0].stream);  /* µs    */
cap_model_set_input(m, prompt, text, len, -1); /* STAGED port = producer verb */
cap_model_tick(c, m);                          /* or fire stages yourself   */
```

Ports carry the **update class** — `SWAP` windows the host writes directly
(the microsecond lane, zero model code), `STAGED` transforms behind the
producer's verb — and the stage DAG makes subgraphs (vision/encoder/action,
VLM+AE splits) schedulable objects: fire them per stage across streams, or
let `cap_model_tick` run the declared order. The hot contract is pinned by
tests: updating ports between ticks never recaptures, never allocates, never
rebinds — replay output tracks buffer contents. Warm-phase shape-bucket
capture goes through `prepare`, never inside a tick.

Stage-plan names and graph-cut choices live with the FlashRT producer. Nexus
does not parse names like `vit`, `dit`, `prefill`, or `decode`; after adoption
it sees only `cap_model_runtime.stages[]` — graph handles, streams, keys, and
dependency indices. That keeps customer/model cut policies reviewable on the
producer side while Nexus stays a generic scheduler.

Interface reference and host-layer norms: [`docs/model_runtime.md`](docs/model_runtime.md).
Where new models, backends, modes, and schedulers plug in:
[`docs/adaptation_map.md`](docs/adaptation_map.md). Mode contract and
authoring guide: [`docs/modes.md`](docs/modes.md). Runnable assemblies
with READMEs live under [`examples/`](examples/).

## Relationship to FlashRT

[FlashRT](https://github.com/flashrt-project/FlashRT) is the inference engine — hand-written kernels
composed into static CUDA graphs, exposing the runtime/model-runtime export contracts that Nexus adopts.
Nexus is the serving substrate *above* it: it consumes FlashRT **unchanged** through the FlashRT backend
and adds the state + schedule + lifecycle control layer.

The RTC action-chunk mode keeps the robot loop supplied with actions while the next chunk is pending,
then splices at chunk boundaries. In this repository, RTC is kept as an L2 scheduling mode over
producer-declared model-runtime stages and ports; model-internal guided denoising remains a FlashRT
producer concern.

## Non-goals

Nexus's core is mechanism, not policy. It is **not** a scheduler, a KV/paged-memory manager, a compiler,
a protocol, or a batching system — those are upper layers. The core encodes no policy, owns no GPU
memory it didn't allocate for a capsule, spawns no thread, and (after v1) freezes its ABI to
additive-only.

## Contributing

The project invariants (the red line), layering, naming conventions, ABI-stability policy, and build
gates are in [CONTRIBUTING.md](CONTRIBUTING.md). It is the contract every change is held to.

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

## References

- [FlashRT](https://github.com/flashrt-project/FlashRT) — the FlashRT inference engine and runtime/model-runtime producer surface.
- [Pi real-time chunking reference](https://github.com/Physical-Intelligence/real-time-chunking-kinetix) — external RTC action-chunk semantics referenced by the Nexus RTC mode.

## License

Apache License 2.0 — see [LICENSE](LICENSE).
