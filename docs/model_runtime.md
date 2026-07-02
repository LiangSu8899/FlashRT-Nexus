# The Standard Model-Runtime Face — Interface & Norms

Authoritative header: [`host/include/capsule/model_runtime.h`](../host/include/capsule/model_runtime.h).
This is the first **L2 (host/framework) surface**: schedulers, sessions, and
runtime loops code against it in capsule types only — never against the
FlashRT export or a model's pipeline.

## Adoption

```c
cap_model_runtime* m;
int rc = flashrt_adopt_model_runtime(model, &m);   /* frt_model_runtime_v1 in */
...
flashrt_model_close(m);                            /* releases everything     */
```

One call: validates the producer ABI, retains the model, adopts the embedded
FlashRT export (backend init, streams bridged across both namespaces,
graphs/buffers wrapped, regions materialized in contractual order), wires SWAP
port windows as capsule buffers, prepares the stage schedule, and pre-creates
the events that make the tick allocation-free.

## The struct (capsule types only)

| field | meaning |
|---|---|
| `backend` | initialized `cap_backend*` — pass to `cap_ctx_create`; capsules are stamped with the producer's fingerprint |
| `ports[]` | dynamic IO: name, modality/dtype/layout, direction, **update class** (`0 SWAP · 1 STAGED · 2 SETUP`), shape, and for SWAP the wired `cap_buffer` window |
| `stages[]` | the subgraph DAG as fire-ready entries: `cap_graph`, key, capsule stream index, `after[]` deps |
| `regions[]` | the restorable boundary, ready for `cap_boundary` / `cap_restore_into` (order is contractual) |
| `schedule` | prepared `cap_schedule` (cadence 1/1, `CAP_EVERY`) for `cap_drive_tick` users |
| `stage_events` | pre-created per depended-upon stage — what makes `cap_model_tick` allocation-free |
| `fingerprint` / `identity` | producer-computed; on a mismatch, print both identity strings to see why |
| `self` + verbs | producer pass-through: `set_input` / `get_output` (bytes) / `prepare` (warm) / `step` (sugar) / `last_error` |

## Driving a tick

```c
int obs = cap_model_find_port(m, "obs");
cap_swap(c, m->ports[obs].buffer, frame, n, m->stages[0].stream);  /* SWAP: µs   */
cap_model_set_input(m, prompt, text, len, -1);                     /* STAGED     */
cap_model_tick(c, m);                    /* whole DAG, allocation-free          */
/* — or schedule stages yourself: */
cap_model_fire(c, m, stage_index);       /* one stage; overlap across streams   */
```

`cap_model_tick` fires stages in declared order: cross-stream dependencies
wait on the pre-created events, same-stream dependencies ride FIFO order.
Hosts that overlap, interrupt, or re-order fire stages themselves — the DAG
is data, the loop is always yours.

Stage-plan registration is not a Nexus concern. Producers may maintain
model/customer plans such as `full`, `context_action`, `vlm_vit_dit_action`,
diffusion chunks, or `prefill_decode`, but adoption erases those labels into
the neutral stage array. Nexus schedules graph handles and dependency indices;
model structure remains producer-owned.

## L2 scheduling

The reusable L2 helpers live under `nexus/`, above this host face:

- `nexus/schedulers/stage_dag.*` wraps the adopted stage DAG as
  allocation-at-construction `fire/query/sync` primitives, plus explicit
  stage masks and per-stage period/phase frequency tables.
- `nexus/schedulers/stage_dag_c.h` exposes the same runner as a small C ABI
  for robot hosts and ctypes gates.
- `nexus/state/graph_store.*` owns graph-cache budget policy over backend
  evict/count verbs.
- `nexus/modes/rtc_action_chunk.*` is the RTC action-chunk mode:
  fire an action stage, poll its completion, copy completed output chunks,
  emit one action at a time, and let the robot loop execute the previous
  chunk or fallback while the new chunk is pending.
- `nexus/modes/rtc_action_chunk_c.h` exposes that mode as a C ABI. The caller
  keeps the `cap_ctx`, `cap_model_runtime`, and `nexus_stage_dag` alive.

For standard VLA action outputs, create the RTC mode from the producer's
`actions` output port instead of hard-coding a shape. The helper reads
`port.shape` as `(chunk_length, action_dims...)` and derives
`action_bytes = prod(action_dims) * scalar_bytes`; `(10, 7)` and `(50, 7)` are
just two declarations of the same contract. Manual `chunk_length/action_bytes`
configuration remains available for non-standard output verbs.

These are scheduler policy. They do not belong in FlashRT's runtime producer
or in the capsule core.

`StageDagRunner` enforces one in-flight replay per stage. `run_mask` fires an
explicit subset in producer-declared order; `run_due` lowers per-stage
period/phase tables to a mask. Dependencies may use the last event recorded by
an earlier stage, which permits a faster action stage to reuse a stale context
stage, for example a 1:4 context/action cadence. It does not make
producer-owned hand-off buffers magically multi-versioned: if a context stage
writes a buffer that an action stage still reads, cross-iteration overlap
requires the producer to export double-buffered stages.

The RTC action-chunk mode is deliberately L2-only. It does not add runtime
fields for horizon, delay, splice, or fallback. The producer exposes stages and
ports; Nexus decides when to request the next chunk, poll, accept a late chunk,
or keep executing the previous chunk after a deadline fallback.

The mode exposes scheduler statistics only: completed/emitted actions,
fallback count, late-after-deadline chunks, current pending ticks, and
last/max/total ready ticks. These counters are for host policy and telemetry;
they are not part of the model-runtime ABI. The real Pi0.5 gate can be run with
`--bench-iters N` to report synchronous tick p50, non-blocking request p50,
ready latency, poll count, ready ticks, and fallback/late counts.

Guided-denoise RTC, where a previous action chunk and guidance weights enter
each denoise step, is a producer/capture feature: those values must be declared
as hot ports and captured into the action graph. Nexus can schedule and splice
that graph once exported, but it does not synthesize model-internal guidance.
For the Pi0.5 reference producer, this is exposed as
`context_rtc_prefix_action`: the producer captures an action graph that reads a
fixed-length raw `prev_action_chunk` prefix and exports `actions_raw` so the
host can close the chunk loop. Nexus treats those as ordinary SWAP ports and
ordinary stages; `prefix_len` remains producer identity, not an L2 field.
The complete variant is `context_rtc_vjp_guided_action`, which requires the
producer to provide a real denoiser VJP/backward graph and exports additional
ordinary SWAP controls such as `prefix_weights` and `guidance_weight`. Nexus can
write those ports from measured delay/policy and schedule the action stage; it
does not own or verify the VJP math.

Performance expectation: splitting a graph and then firing the pieces
sequentially at the same cadence can be slightly slower than one full graph
because it adds another replay boundary. The reason to split is not same-order
speedup; it is to let L2 schedule stages at different rates, reuse stale
context, prefetch action chunks, interrupt at graph boundaries, or overlap
producer-declared double-buffered stages.

## The hot-input contract (pinned by `tests/test_model_adopt.cpp`)

Updating SWAP or STAGED ports **between ticks** never recaptures, never
allocates, never rebinds graph pointers — replay output tracks buffer
contents, round after round. Warm-phase shape-bucket capture goes through the
producer's `prepare`, never inside a tick.

## Graph-cache mechanism vs policy

Eviction and budget **policy** (an L2 graph store: per-model quotas, global
budgets, warm sets) is built on the backend pass-through:

```c
flashrt_graph_evict(be, g, key);        /* drop one variant                  */
flashrt_graph_evict_lru(be, g);         /* drop the least-recently replayed  */
flashrt_graph_variant_count(be, g);     /* budget accounting                 */
```

Discipline: evict only at a safe point — never while the variant may be in
flight on some stream (sync or wait its event first). Production graphs are
fixed-shape or bucket-keyed; hot-path misses fail loudly.

## FFI hosts

The flat accessors (`cap_model_backend`, `cap_model_find_port`,
`cap_model_port_buffer/bytes/update`, `cap_model_stage_stream`,
`cap_model_region_array/count`, `cap_model_set_input/get_output/last_error`)
let ctypes/dlopen hosts drive an adopted model without mirroring structs.
`tests/gate_pi05_model.py` is the reference: a real Pi0.5 policy re-seeded
per tick through its noise SWAP port, deterministic per seed, restore-exact.

## Threading & lifetime

One `cap_ctx` per thread (the core rule) applies unchanged. The consumer
holds one reference to the model runtime; the export reference is internal.
`flashrt_model_close` may run from any thread — a Python producer acquires
the GIL inside its release path.

## What does NOT belong here

No session, no cadence policy, no modality processing, no protocol. This
header is data + pass-through verbs + lookups; everything above it is
pluggable policy, everything below it is the frozen core and the backend
seam. See [`CONTRIBUTING.md`](../CONTRIBUTING.md) for the red lines.
