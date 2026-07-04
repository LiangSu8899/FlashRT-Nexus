# Adaptation Map — Where New Work Plugs In

Nexus has exactly four extension points. Everything in the tree is an
instance of one of them; nothing else is meant to be modified when you
bring a new model, a new device, a new interaction pattern, or a new
scheduling policy.

```
        APPLICATIONS      examples/ · robot hosts · agent workflows
            ▲
   (4) MODES              nexus/modes/        interaction state machines
   (3) SCHEDULERS         nexus/schedulers/   fire/query policy over the DAG
            ▲
        MODEL-RUNTIME FACE  host/include/capsule/model_runtime.h
            ▲
   (2) BACKENDS           backends/           capsule verbs for a runtime
   (1) PRODUCERS          (live in the model's own repo)  export the face
```

## The decision table

| You have | You write | You do NOT touch |
|---|---|---|
| A new **model or pipeline** (any framework that can capture CUDA graphs) | A **producer**: package graphs, buffers, streams, and restorable regions as `frt_runtime_export_v1` / `frt_model_runtime_v1`. Python pipelines use `flash_rt.runtime.export` (`BufferSpec` / `GraphSpec` / `RegionSpec` / `StreamSpec`); C++ pipelines fill the struct directly. | Nexus. Adoption erases model structure into the neutral stage array — Nexus schedules graph handles and dependency indices only. |
| A new **runtime / device substrate** | A **backend**: implement the capsule verbs (buffers, graphs, streams, events, snapshot/restore) — see `backends/stub` for the minimal shape and `backends/flashrt` for a real one. Pass `tests/backend_conformance.h`. | Modes, schedulers, or the face. They are backend-agnostic by construction. |
| A new **interaction pattern** (how an application consumes inference: action chunks, token streams, barge-in, …) | A **mode** under `nexus/modes/`: a state machine over `StageDagRunner`, plus a C ABI mirror for robot hosts and ctypes. Contract and authoring guide: [`modes.md`](modes.md). | The scheduler primitives. Modes compose `fire/query/sync`; they never talk to the backend directly. |
| A new **scheduling policy** (multi-rate, priority, preemption windows) | A **scheduler helper** under `nexus/schedulers/`, policy-thin like `stage_dag.*`: allocation at construction, hot path composed of capsule verbs only. | Modes (they consume your primitives) and the face. |

## Litmus tests

- If your change needs to know a model's port names or tensor layout,
  it belongs in a **producer** (or in the application code above the
  mode) — not in Nexus.
- If your change needs a new CUDA call, it belongs in a **backend**.
- If your change is "when do stages fire", it is a **scheduler**; if it
  is "what the application sees between fires", it is a **mode**.
- If a mode needs state the face does not carry, the fix is a richer
  export from the producer — not a side channel.

## Runnable references

- `examples/` — presentation-shaped entry points with READMEs; start
  here to see the layers assembled.
- `tests/gate_*.py` — acceptance gates. They double as end-to-end
  wiring references but their job is CI: assertions first, narration
  second. New features land with a gate; new showcases land with an
  example.
