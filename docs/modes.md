# Modes — Contract and Authoring Guide

A **mode** is the layer between a scheduler and an application: an
interaction state machine that decides what the application observes
between stage fires. RTC action chunks, token streaming, and
speculative-decode sessions are all modes. This page fixes the contract
so every mode in `nexus/modes/` has the same shape, and a new one can
be written without reverse-engineering the last one.

## The contract

A mode:

1. **is constructed over a `StageDagRunner`** (never over the backend,
   never over raw capsule handles). Its whole power budget is
   `fire / query / sync / run_*` plus `cap_model_get_output` /
   `cap_swap` through the adopted face.
2. **owns an explicit state enum** and exposes it from a non-blocking
   `poll()`. Applications drive the mode with at most three verbs:
   `request()` (start work), `poll()` (advance the state machine), and
   a consumption verb (`next_action()`, `next_tokens()`, …).
3. **allocates at construction only.** Ring slots, staging copies,
   pinned readbacks — everything is sized from a `Config` struct up
   front; `request/poll/consume` are allocation-free.
4. **carries its own telemetry** as plain counters (completions,
   fallbacks, late events, tick histograms). Schedulers and hosts read
   counters; they do not instrument the mode from outside.
5. **ships a C ABI mirror** (`<mode>_c.h/.cpp`): an opaque handle, a
   `struct_size`-versioned config, `create / destroy` plus the same
   verbs, so robot hosts and ctypes gates drive it without C++.
6. **never blocks the application thread** except in an explicit
   `sync`-style verb. Deadlines are expressed in ticks through the
   config (`deadline_ticks`-style), and missing one is a *state*
   (`kFallback`), not an exception.

What a mode is **not**: it does not know modalities beyond the ports it
was configured with, does not own scheduling policy (periods, phases,
masks belong to the scheduler helpers), and does not reach past the
face into producer internals.

## Anatomy, using action chunks as the exemplar

[`nexus/modes/action_chunk/action_chunk.h`](../nexus/modes/action_chunk/action_chunk.h)
is the reference implementation of the contract — the generic VLA
action-chunk mode (Pi0.5 is its first gated instance):

| Contract clause | Where the action-chunk mode does it |
|---|---|
| Config struct, sized up front | `ActionChunkConfig` (stage, port, chunk geometry, `ring_slots`, `execute_horizon`, `deadline_ticks`); `config_from_output_port` derives geometry from the port shape |
| State enum + non-blocking poll | `ActionChunkState {Idle, Pending, Ready, Fallback, Error}`; `poll()` advances, `next_action()` consumes |
| Allocation at construction | the ring `storage_` is sized in the constructor; the hot path is `fire/query` + one output copy into a pre-sized slot |
| Telemetry counters | `fallbacks / late_chunks / pending_ticks / *_ready_ticks / completed_chunks / emitted_actions` |
| C ABI mirror | [`action_chunk_c.h`](../nexus/modes/action_chunk/action_chunk_c.h): opaque handle, `struct_size`-checked config, verb-per-function |
| Deadline as state | `deadline_ticks < 0` disables; overrun marks `kFallback`, the robot keeps executing the previous chunk |

Read it top to bottom once; every future mode should feel like a
re-instantiation of that file with a different state machine.

## Layout

One directory per mode: `nexus/modes/<name>/` holds the C++ class, the
C ABI mirror, and the mode's README (what interaction pattern it
serves, its config, its telemetry). Family sub-grouping (e.g. several
action-chunk variants) is introduced only when a family reaches two
members — no empty taxonomy.

## Authoring checklist

- [ ] `nexus/modes/<name>/<name>.h/.cpp` — C++ class per the contract.
- [ ] `nexus/modes/<name>/<name>_c.h/.cpp` — C ABI mirror with a
      `struct_size`-versioned config.
- [ ] `nexus/modes/<name>/README.md` — one page: pattern, config,
      state machine, telemetry; add the mode to
      `nexus/modes/README.md`.
- [ ] A conformance-style unit test under `tests/` driving the state
      machine against the stub backend (no GPU needed for the state
      logic).
- [ ] A gate under `tests/gate_*.py` proving the real-model path, with
      a numerical acceptance criterion (RTC's: chunk output equals the
      `cap_model_tick` baseline).
- [ ] An entry in [`adaptation_map.md`](adaptation_map.md) if the mode
      introduces a new consumption verb family.
- [ ] If it is presentation-worthy, a runner + README under
      `examples/` — gates are CI, examples are for humans.

## Modes and capsules

A mode's quiescent points (RTC: `Ready` with no fire in flight; a
speculative session: the committed boundary between cycles) are exactly
where capsule verbs are legal. A well-shaped mode therefore doubles as
the interruption grid for schedulers: pause at a quiescent state,
snapshot or fork the model's regions, resume later — without the mode
needing any capsule-specific code.
