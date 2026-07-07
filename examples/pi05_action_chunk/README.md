# Example — Pi0.5 Action Chunks over Nexus

The reference assembly of the four layers on a real model:

```
Pi0.5 FlashRT producer          exports the context_action model-runtime face
  -> backends/flashrt           adoption: graphs/buffers/regions -> capsule types
  -> nexus/schedulers/stage_dag fire/query primitives over the 2-stage DAG
  -> nexus/modes/action_chunk
                                the action-chunk state machine: fire the action
                                stage asynchronously, execute the previous
                                chunk until the new one is ready, fall back
                                on deadline overrun
```

The interaction pattern this demonstrates is asynchronous action
chunking: a robot control loop that never blocks on inference. The mode
keeps a ring of completed action chunks; the loop consumes
`next_action()` at its own rate while the next chunk is in flight, and
a missed deadline is a *state* (`FALLBACK`) the loop can act on — not
an exception.

## Prerequisites

- A CUDA GPU supported by the Pi0.5 FlashRT pipeline (developed on
  Jetson AGX Thor and RTX SM120).
- FlashRT built with its `exec/` and `runtime/` components, plus the
  Pi0.5 C++ verb library.
- Nexus built with `-DCAPSULE_BUILD_FLASHRT_BACKEND=ON`.
- A Pi0.5 checkpoint directory.

## Run

```bash
export FLASHRT_DIR=/path/to/FlashRT
export NEXUS_LIB=/path/to/FlashRT-Nexus/build/libcapsule_nexus_flashrt.so
# optional, defaults under FLASHRT_DIR:
# export PI05_LIB=/path/to/libflashrt_cpp_pi05_c.so

python examples/pi05_action_chunk/run.py --checkpoint /path/to/pi05_checkpoint
```

`run.py` validates the environment and delegates to the acceptance
gate (`tests/gate_pi05_action_chunk.py`), which builds the
producer, adopts the model, runs the RTC mode, and checks the emitted
chunk numerically against a plain `cap_model_tick` baseline. Add
`--bench-iters N` for a latency profile of the async path.

## What to look at

- The gate source is the wiring reference: producer export (a few
  lines on the model pipeline), `ctypes` adoption, `StageDAG` creation,
  and the four-verb drive loop. The policy gates
  (`gate_pi05_action_chunk_{fusion,projected,prefix,composed}.py`)
  extend the same wiring with one policy each.
- [`docs/modes.md`](../../docs/modes.md) explains the mode contract
  this example instantiates.
- [`docs/adaptation_map.md`](../../docs/adaptation_map.md) tells you
  which of the four layers your own work belongs in.
