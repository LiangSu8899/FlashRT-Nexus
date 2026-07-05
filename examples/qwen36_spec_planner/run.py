#!/usr/bin/env python3
"""Example — preemptible LLM planner beside a real-time actor.

See README.md in this directory. The planner is a FlashRT
speculative-decode session (Qwen3.6-27B DFlash) driven one cycle at a
time; the committed boundary between cycles is both the preemption
grid and the device-state set copied by the host snapshot.
"""
from __future__ import annotations

import argparse
import os
import sys
import time


def _add_flashrt_to_path() -> None:
    flashrt_dir = os.environ.get("FLASHRT_DIR")
    if not flashrt_dir or not os.path.isdir(flashrt_dir):
        raise SystemExit(
            "[qwen36_spec_planner] set FLASHRT_DIR=<FlashRT repo root>")
    if flashrt_dir not in sys.path:
        sys.path.insert(0, flashrt_dir)


PLAN_PROMPT = (
    "You are a robot task planner. Output a JSON action list with "
    "fields step, action, target, and params for: pick up the red "
    "cube, place it on the tray, then report status."
)


def snapshot_boundary(session):
    """Host-level snapshot: device boundary plus token journal."""
    import torch

    b = session.boundary()
    saved = {"cur_pos": b["cur_pos"],
             "tokens_generated": b["tokens_generated"],
             "tok": session.tok.clone(),
             "generated": list(session.generated)}
    for key in ("spec_attempts", "spec_accepts", "spec_full",
                "policy_in_think"):
        if key in b:
            saved[key] = bool(b[key]) if key == "policy_in_think" else int(b[key])
    for key in ("lin_state", "lin_conv_state", "drafter_shift_window",
                "drafter_window", "taps_row0"):
        if key in b:
            saved[key] = b[key].clone()
    if "drafter_window_valid" in b:
        saved["drafter_window_valid"] = b["drafter_window_valid"]
    if "fp8_k_cache" in b:
        end = b["cur_pos"] + 32
        saved["fp8_k_rows"] = b["fp8_k_cache"][:, :end].clone()
        saved["fp8_v_rows"] = b["fp8_v_cache"][:, :end].clone()
        saved["kv_end"] = end
    torch.cuda.synchronize()
    return saved


def restore_boundary(session, saved) -> None:
    import torch

    b = session.boundary()
    for key in ("lin_state", "lin_conv_state", "drafter_shift_window",
                "drafter_window", "taps_row0"):
        if key in saved:
            b[key].copy_(saved[key])
    if "drafter_window_valid" in saved:
        session.fe._dflash_buf["pt_valid"] = saved[
            "drafter_window_valid"]
    if "fp8_k_rows" in saved:
        end = saved["kv_end"]
        b["fp8_k_cache"][:, :end].copy_(saved["fp8_k_rows"])
        b["fp8_v_cache"][:, :end].copy_(saved["fp8_v_rows"])
    session.cur_pos = saved["cur_pos"]
    session.tok = saved["tok"].clone()
    session.generated = list(saved["generated"])
    for key, attr in (("spec_attempts", "_spec_attempts"),
                      ("spec_accepts", "_spec_accepts"),
                      ("spec_full", "_spec_full")):
        if key in saved:
            setattr(session.fe, attr, int(saved[key]))
    if "policy_in_think" in saved and hasattr(session.policy, "in_think"):
        session.policy.in_think = bool(saved["policy_in_think"])
    torch.cuda.synchronize()


def run_planner_only(args) -> int:
    import torch

    from flash_rt.frontends.torch.qwen36_thor import (
        Qwen36TorchFrontendThor,
    )

    cap = torch.cuda.get_device_capability()
    if cap != (11, 0):
        from flash_rt.frontends.torch.qwen36_rtx import (
            Qwen36TorchFrontendRtx as Frontend,
        )
    else:
        Frontend = Qwen36TorchFrontendThor

    print("[planner] loading model ...", flush=True)
    fe = Frontend(args.checkpoint, quant="nvfp4", max_seq=args.max_seq)
    fe.init_dflash_drafter()
    ids = fe._tokenizer.apply_chat_template(
        [{"role": "user", "content": PLAN_PROMPT}],
        add_generation_prompt=True, enable_thinking=True,
        return_tensors="pt")
    from flash_rt.frontends.torch.spec_session import as_input_ids_tensor
    ids = as_input_ids_tensor(ids, device=fe.device)

    session = fe.make_dflash_session(
        max_new_tokens=args.max_tokens, K=15)

    # Warm pass: one ungated generation populates the per-position
    # verify-graph cache (graph capture costs seconds per new
    # position; serving does the same at startup). The ticked pass
    # below then sees steady replay-only cycles.
    print("[planner] warm pass (graph capture) ...", flush=True)
    session.begin(ids)
    warm_ms = []
    while not session.done():
        t0 = time.perf_counter()
        session.step()
        torch.cuda.synchronize()
        warm_ms.append((time.perf_counter() - t0) * 1e3)
    warm_ms.sort()
    warm_cycle_ms = warm_ms[len(warm_ms) // 2]
    print(f"[planner] warm pass: {len(warm_ms)} cycles, median "
          f"{warm_cycle_ms:.1f} ms", flush=True)

    tick_s = 1.0 / args.tick_hz
    # Fixed budget from the warm-pass median; never adapted from live
    # cycles (a single slow cycle must not poison the gate).
    cycle_budget_s = min(args.cycle_budget_ms, warm_cycle_ms * 1.15) / 1e3
    if cycle_budget_s >= tick_s:
        raise SystemExit(
            f"[planner] a spec cycle ({warm_cycle_ms:.0f} ms) does not "
            f"fit the tick period ({tick_s * 1e3:.0f} ms) — lower "
            f"--tick-hz")
    interrupts = 0
    overruns = 0
    cycle_ms = []
    window_n = []
    saved = None
    snap_at = args.max_tokens // 2

    session.begin(ids)
    print(f"[planner] ticked pass, prompt={session.prompt_len} "
          f"tokens, budget {cycle_budget_s * 1e3:.0f} ms", flush=True)
    tick_deadline = time.perf_counter() + tick_s
    while not session.done():
        # actor placeholder: the tick itself. --with-actor replaces
        # this with rtc.poll()/next_action() (see main()).
        now = time.perf_counter()
        if now + cycle_budget_s > tick_deadline:
            interrupts += 1
            sleep = tick_deadline - now
            if sleep > 0:
                time.sleep(sleep)
            tick_deadline += tick_s
            continue
        t0 = time.perf_counter()
        n = session.step()
        torch.cuda.synchronize()
        dt = time.perf_counter() - t0
        cycle_ms.append(dt * 1e3)
        window_n.append(n)
        if dt > cycle_budget_s * (1 + args.overrun_tolerance):
            overruns += 1

        if saved is None and len(session.generated) >= snap_at:
            saved = snapshot_boundary(session)
            saved["post_ns"] = []
            print(f"[planner] snapshot at {len(session.generated)} "
                  f"tokens", flush=True)
        elif saved is not None and len(saved["post_ns"]) < 8:
            # the original continuation: the deterministic reference
            # the restored session must reproduce
            saved["post_ns"].append(n)

    pre_restore_text = fe._tokenizer.decode(
        [int(t.item()) for t in session.generated],
        skip_special_tokens=True)

    restore_exact = None
    if saved is not None:
        original = [int(t.item()) for t in session.generated]
        restore_boundary(session, saved)
        resumed = []
        while (not session.done()
               and len(resumed) < len(saved["post_ns"])):
            resumed.append(session.step())
        replayed = [int(t.item()) for t in session.generated]
        span = min(len(original), len(replayed))
        restore_exact = (replayed[:span] == original[:span]
                         and resumed == saved["post_ns"][:len(resumed)])
        print(f"[planner] restore replay: tokens "
              f"{'IDENTICAL' if restore_exact else 'DIVERGED'} over "
              f"{span} positions; per-cycle N resumed={resumed} "
              f"original={saved['post_ns'][:len(resumed)]}", flush=True)

    mean_cy = sum(cycle_ms) / max(1, len(cycle_ms))
    print(f"[planner] cycles={len(cycle_ms)} mean={mean_cy:.1f} ms "
          f"p_max={max(cycle_ms):.1f} ms interrupts={interrupts} "
          f"overruns={overruns}", flush=True)
    al = (len(session.generated)
          / max(1, fe._spec_attempts))
    print(f"[planner] tokens={len(session.generated)} AL={al:.2f}",
          flush=True)
    print(f"[planner] plan tail: {pre_restore_text[-200:]!r}",
          flush=True)

    ok = overruns == 0
    if restore_exact is not None:
        ok = ok and restore_exact
    print(f"[planner] ACCEPTANCE {'PASS' if ok else 'FAIL'}",
          flush=True)
    return 0 if ok else 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--checkpoint", required=True,
                    help="Qwen3.6-27B NVFP4 checkpoint directory")
    ap.add_argument("--planner-only", action="store_true")
    ap.add_argument("--with-actor", action="store_true",
                    help="drive the Pi0.5 RTC actor in the same "
                         "process (see examples/pi05_rtc)")
    ap.add_argument("--tick-hz", type=float, default=5.0)
    ap.add_argument("--cycle-budget-ms", type=float, default=150.0,
                    help="upper bound on the planner cycle budget; the "
                         "effective budget is min(this, 1.15x the "
                         "warm-pass median cycle)")
    ap.add_argument("--overrun-tolerance", type=float, default=0.5)
    ap.add_argument("--max-tokens", type=int, default=192)
    ap.add_argument("--max-seq", type=int, default=32768)
    args = ap.parse_args()

    _add_flashrt_to_path()
    if args.with_actor:
        raise SystemExit(
            "[qwen36_spec_planner] --with-actor wiring follows the "
            "pi05_rtc example (RTC C ABI in the tick slot); run "
            "--planner-only first to validate the planner half on "
            "this machine")
    return run_planner_only(args)


if __name__ == "__main__":
    raise SystemExit(main())
