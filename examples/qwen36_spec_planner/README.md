# Example — A Preemptible LLM Planner Beside a Real-Time Actor

Physical AI needs two inference workloads on one device at once: a
**real-time actor** (a VLA policy emitting action chunks on a hard
cadence) and a **planner** (an LLM producing task plans whenever the
control loop can spare the bandwidth). This example shows the
state-first way to make them coexist:

- the planner is a **speculative-decode session** whose cycle boundary
  (~90 ms on Jetson AGX Thor for Qwen3.6-27B DFlash) is the
  preemption grid — the control loop interrupts *between* cycles,
  never mid-kernel;
- the session's committed boundary exposes the **device-state set**
  (KV, recurrent/conv state, drafter feature window, cursor) that the
  host snapshot copies together with its token journal; the planner can
  be snapshotted on a task switch and resumed later with its speculation
  acceptance rate intact (no warm-up ramp, the drafter window comes
  back with it);
- the actor runs through the Nexus **RTC action-chunk mode**
  (`examples/pi05_rtc`) and never blocks on the planner.

```
control tick (fixed cadence)
  ├─ actor: rtc.poll() / next_action()      never late; FALLBACK is a state
  ├─ budget left before the next tick?
  │    └─ planner: session.step()           one spec cycle, ~90 ms bound
  └─ on task switch: save = snapshot(session.boundary())
                     ... later: restore(save); session keeps its AL
```

## Layout

- `run.py --planner-only` — the planner half on its own: FlashRT
  Qwen3.6 DFlash session driven at `step()` granularity against a
  simulated control cadence, with deadline-aware yielding,
  boundary snapshot/restore, and a warm-resume acceptance check.
  Needs only FlashRT and the Qwen3.6 checkpoints.
- `run.py --with-actor` — adds the Pi0.5 RTC actor through the Nexus
  C ABI in the same process (see `examples/pi05_rtc` for the actor's
  own prerequisites). The actor owns the tick; the planner fills the
  gaps.

## Prerequisites

- FlashRT built for your GPU with the Qwen3.6 NVFP4 path (see
  `docs/qwen36_dflash.md` in the FlashRT repo), plus the Qwen3.6-27B
  NVFP4 + FP8-MTP checkpoints and the z-lab DFlash drafter.
- For `--with-actor`: everything `examples/pi05_rtc` needs.

## Run

```bash
export FLASHRT_DIR=/path/to/FlashRT
export FLASHRT_QWEN36_MTP_CKPT_DIR=$QWEN36_MTP_CKPT
export FLASHRT_QWEN36_DFLASH_CKPT_DIR=$QWEN36_DFLASH_CKPT

python examples/qwen36_spec_planner/run.py \
  --checkpoint $QWEN36_NVFP4_CKPT \
  --planner-only --max-tokens 192
```

The telemetry shows the planner's realized accept length, cycle
times against the tick budget, interrupts taken, and — after the
mid-run snapshot/restore — whether the restored session reproduces
the original continuation token-for-token.

## Acceptance criteria

1. **Preemption bound**: no planner cycle overruns the tick deadline
   by more than one cycle time.
2. **Exact restore**: after `restore`, the resumed generation is
   token-identical to the original continuation, with the same
   per-cycle accept counts — greedy decoding from a fully restored
   boundary is deterministic, so anything less means state was lost
   (the drafter window travels with the boundary).
3. **Actor isolation** (`--with-actor`): the RTC chunk stream is
   numerically identical to running the actor alone with the same
   seed.

## Where this sits in the tree

The planner-side snapshot/restore here is deliberately host-level: it
clones the `boundary()` device-state set and preserves the token
journal/cursor in Python. The follow-up is mechanical: register the
same named buffers as capsule regions through the producer export so
`cap_boundary` / `cap_restore_into` / fork do the same job engine-side
while the host keeps request-level token state — see
[`docs/adaptation_map.md`](../../docs/adaptation_map.md) (producer
row) and [`docs/modes.md`](../../docs/modes.md) (a spec-session mode
is the natural second mode in `nexus/modes/`).
