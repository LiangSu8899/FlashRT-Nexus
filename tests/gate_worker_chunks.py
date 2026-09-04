"""Exercise a real spawned worker through the C action-chunk consumer."""

import argparse
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from flashrt_nexus import ExecutionWorker, WorkerActionChunks


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--nexus", required=True)
    args = parser.parse_args()
    with ExecutionWorker("worker_fixture:build", {}) as worker:
        chunks = WorkerActionChunks(worker, nexus_lib=args.nexus,
                                    execute_horizon=1, deadline_steps=1)
        try:
            chunks.request_inputs({"value": 2})
            chunks.wait_ready(5)
            np.testing.assert_array_equal(chunks.next_action(), [2, 2])
            started = time.monotonic()
            chunks.request_inputs({"value": 3, "delay": 0.2})
            assert time.monotonic() - started < 0.15
            assert chunks.in_flight
            try:
                chunks.request_inputs({"value": 99})
            except RuntimeError:
                pass
            else:
                raise AssertionError("concurrent submission accepted")
            for _ in range(4):
                chunks.next_action()
            chunks.wait_ready(5)
            np.testing.assert_array_equal(chunks.next_action(), [3, 3])
            assert chunks.stats()["late_chunks"] == 1
            chunks.request_inputs({"value": 99, "delay": 0.05})
            chunks.reset()
            assert chunks.next_action() is None
            chunks.request_inputs({"value": 4})
            chunks.wait_ready(5)
            np.testing.assert_array_equal(chunks.next_action(), [4, 4])
            chunks.request_inputs({"fail": True})
            try:
                chunks.wait_ready(5)
            except RuntimeError as exc:
                assert "fixture failure" in str(exc.__cause__)
            else:
                raise AssertionError("provider error was lost")
        finally:
            chunks.reset()
            chunks.close()
    print("PASS - worker -> Nexus action chunks, deadline, reset, failure")


if __name__ == "__main__":
    main()
