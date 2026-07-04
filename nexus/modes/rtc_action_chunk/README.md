# rtc_action_chunk — embodied async inference

The RTC action-chunk mode: a robot control loop that never blocks on
inference. `request()` fires the action stage asynchronously; the loop
keeps consuming `next_action()` from the active chunk ring while the
next chunk is in flight; a missed deadline is a state (`kFallback`),
not an exception.

- C++: `rtc_action_chunk.h` (`RtcActionChunkConfig`,
  `RtcActionChunkMode`) — config geometry can be derived from the
  ACTION output port shape via `config_from_output_port`.
- C ABI: `rtc_action_chunk_c.h` — opaque handle,
  `struct_size`-versioned config, verb-per-function; used by robot
  hosts and the ctypes gates.
- Telemetry: completed/emitted counters, fallbacks, late chunks,
  pending/ready tick histograms.

Runnable assembly: [`examples/pi05_rtc`](../../../examples/pi05_rtc/)
(Pi0.5 context/action split, numerically gated against the
`cap_model_tick` baseline in `tests/gate_pi05_rtc_action_chunk.py`).
