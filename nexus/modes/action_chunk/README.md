# action_chunk — embodied async inference

The generic VLA action-chunk mode: a robot control loop that never
blocks on inference. `request()` fires the action stage asynchronously;
the loop keeps consuming `next_action()` from the active chunk ring
while the next chunk is in flight; a missed deadline is a state
(`kFallback`), not an exception.

The mode is model-agnostic: chunk geometry is derived from any ACTION
output port shape (`(10, 7)` and `(50, 7)` are just two declarations of
the same contract). Pi0.5 is the first real-model instance.

## One mechanism, two policy slots

The mechanism owns the async lifecycle: the chunk ring, the two-phase
request (`begin_request` prepares, `commit_request` fires), the
controller-step grid (`action_step`, advanced only by consumption or
`advance_step`), two independent deadline clocks (`deadline_steps` on
the grid; `poll_budget` as a pending-poll liveness watchdog), the state
feed, and plain-counter telemetry.

Every asynchronous chunking method is a decision on one of two seams,
selected by config and validated at create — never at tick:

| Slot | Policy | Decision |
|---|---|---|
| prepare (before fire) | `none` | nothing — the chunk is conditioned on fire-time inputs as-is |
| | `projected_state` | integrate the next `lookahead_steps` delta actions into the measured state and feed it to the model; the chunk anchors at `fire_step + k`, index 0 pinned to the projected step (early landings wait; consumption switches exactly at the start step) |
| | `prev_chunk_prefix` | retain the model-space raw chunk and stage it, re-indexed by the consumption position, for the producer's in-graph prefix freeze (`decode_rtc_prefix`-style hard inpainting) |
| consume (on ready / per step) | `plain` | seat immediately at index 0 |
| | `switch` | seat at the latency index `clip(d + switch_offset)` or at the state-distance argmin |
| | `temporal_fusion` | retain completed chunks on the step grid and fuse the newest chunk's window with `exp(-decay·Δ)` weights (table built at create; f64 accumulation, f32 output; raw chunks stay immutable) |

Pairings follow a compatibility matrix (see `validate()`):
`projected_state` pairs with `plain` (its d-vs-k seating IS the latency
compensation); `prev_chunk_prefix` pairs with `switch(latency)`;
`projected_state + temporal_fusion` is allowed behind the
`experimental` config flag (fusion of an early-landed chunk defers to
its start step). Everything else is rejected at create.

## Community-method aliases

| Config | Method lineage |
|---|---|
| `none + plain` | naive double-buffered swap |
| `none + switch(latency)` | latency-corrected async swap |
| `none + temporal_fusion` | temporal ensembling (ACT lineage) |
| `projected_state + plain` | projected-state async inference |
| `prev_chunk_prefix + switch` | real-time chunking, hard-prefix variant |
| `projected_state + temporal_fusion` | composition (experimental) |

## Choosing a policy

The output-side consume policies (`switch`, `temporal_fusion`) make no
assumption about the action parameterization — they operate on the chunk
the model returns. Under a controlled-latency LIBERO evaluation their
advantage over naive `plain` seating grows with the injected latency
(the seam a stale chunk creates gets larger, and skipping/fusing it away
matters more), while `plain` degrades. They are the default choice for
latency compensation. `prev_chunk_prefix` reaches the same robustness by
construction — the producer's in-graph freeze — wherever the prefix
graph is exposed.

`projected_state` compensates on the input side, and its applicability
depends on the action space. It integrates action deltas into the
measured state by cumulative addition (`delta_cumulative`), which is
exact only for dimensions that compose additively — translation,
velocity, and similar first-order commands. For action spaces that carry
orientation as non-additive parameters (axis-angle or quaternion pose
deltas), integrating rotation additively accumulates error that grows
with `lookahead_steps`; on such spaces the projection provides no benefit
over naive seating. Restrict the projection to the additively-composable
dimensions via `set_state_action_indices`; map a state dimension to
`UINT32_MAX` to preserve it without integrating an action column. Or compute
the projection in the embedder (proper composition) and inject it through host
transport. The mode integrates whatever deltas it is given; which
dimensions are projectable is a model/action-space property the embedder
owns. The `experimental` composition inherits the same applicability
bound.

## Transport grading

Policies that write producer inputs use `+1`-encoded transport fields
(`state_input_port`, `prev_chunk_port`): `0` = **host transport** — the
mode computes and exposes the value (`projected_state()`,
`prev_chunk_staged()`) between `begin_request` and `commit_request`,
and the embedder injects it through its model-specific path. Host
transport remains available for producers without a compatible port.

For `projected_state`, a nonzero `state_input_port` selects a producer-declared
`STATE/F32/STAGED` input. Its rank-1 shape must equal `state_dim`; the mode
validates that schema at create and calls the generic `set_input` verb during
`begin_request`. The producer owns state semantics and hot-path staging;
Nexus only computes the configured projection and moves its bytes. A schema
mismatch is rejected at setup, never after a fire. `prev_chunk_port` remains
host-transport-only. Output-side SWAP ports are served directly from their
declared device window (`StageDagRunner::read_output`).

- C++: `action_chunk.h` (`ActionChunkConfig`, `ActionChunkMode`) —
  config geometry can be derived from the ACTION output port shape via
  `config_from_output_port`.
- C ABI: `action_chunk_c.h` — opaque handle, `struct_size`-versioned
  config, verb-per-function; used by robot hosts and the ctypes gates.
- Deprecated: `rtc_action_chunk_compat.h` — pre-rename
  `nexus_rtc_action_chunk_*` aliases, kept for external consumers until
  the next 0.x breaking window. New code uses `action_chunk_c.h`.
- Telemetry: completed/emitted counters, fallbacks, late chunks,
  held actions, chunk switches, pending/ready tick histograms, grid and
  seating stamps.

## Verification

- `tests/test_action_chunk.cpp` — mechanism and policy properties on
  the stub backend (versioning, the two clocks, hold-last, seating,
  waiting, projected-state port staging, determinism replay).
- `tests/test_action_chunk_oracle.cpp` — golden-vector replay against
  the reference semantics (fusion bit-exact in f32, projection to one
  ulp); skips unless `NEXUS_AC_VECTORS` / `NEXUS_AC_PROJ_VECTORS` are
  set.
- Real-model gates, all numerically anchored: `gate_pi05_action_chunk`
  (plain, memcmp vs `cap_model_tick`; the regression anchor),
  `..._fusion` (raw vs baseline + fused vs reference),
  `..._projected` (projection vs reference + chunk vs baseline under
  the injected state), `..._prefix` (staged bytes vs independent
  recompute + chunk vs baseline), `..._composed` (all four seams), and
  `gate_pi05_native_projected_port` (native STATE/STAGED port vs manual
  staging, bit-exact actions).

Runnable assembly:
[`examples/pi05_action_chunk`](../../../examples/pi05_action_chunk/)
(Pi0.5 context/action split, numerically gated against the
`cap_model_tick` baseline in `tests/gate_pi05_action_chunk.py`).
