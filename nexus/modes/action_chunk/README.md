# action_chunk — embodied async inference

The generic VLA action-chunk mode: a robot control loop that never
blocks on inference. `request()` fires the action stage asynchronously;
the loop keeps consuming `next_action()` from the active chunk ring
while the next chunk is in flight; a missed deadline is a state
(`kFallback`), not an exception.

The mode is model-agnostic: chunk geometry is derived from any ACTION
output port shape (`(10, 7)` and `(50, 7)` are just two declarations of
the same contract). Pi0.5 is the first real-model instance.

- C++: `action_chunk.h` (`ActionChunkConfig`, `ActionChunkMode`) —
  config geometry can be derived from the ACTION output port shape via
  `config_from_output_port`.
- C ABI: `action_chunk_c.h` — opaque handle, `struct_size`-versioned
  config, verb-per-function; used by robot hosts and the ctypes gates.
- Deprecated: `rtc_action_chunk_compat.h` — pre-rename
  `nexus_rtc_action_chunk_*` aliases, kept for external consumers until
  the next 0.x breaking window. New code uses `action_chunk_c.h`.
- Telemetry: completed/emitted counters, fallbacks, late chunks,
  pending/ready tick histograms.

Runnable assembly: [`examples/pi05_rtc`](../../../examples/pi05_rtc/)
(Pi0.5 context/action split, numerically gated against the
`cap_model_tick` baseline in `tests/gate_pi05_action_chunk.py`).
