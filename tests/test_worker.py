import os

import numpy as np
import pytest

from flashrt_nexus import ExecutionWorker


def test_factory_error_is_preserved():
    with pytest.raises(AttributeError, match="missing_factory"):
        ExecutionWorker("worker_fixture:missing_factory", {})


def test_resident_worker_reset_and_failure():
    with ExecutionWorker("worker_fixture:build", {}) as worker:
        assert worker.description["pid"] != os.getpid()
        worker.submit({"delay": 0.05})
        with pytest.raises(RuntimeError, match="already has a request"):
            worker.submit({})
        np.testing.assert_array_equal(worker.result(), np.ones((3, 2)))
        worker.submit({"delay": 0.05})
        worker.reset()
        with pytest.raises(RuntimeError, match="no request"):
            worker.result()
        worker.submit({})
        np.testing.assert_array_equal(worker.result(), np.ones((3, 2)))
        worker.submit({"fail": True})
        with pytest.raises(ValueError, match="fixture failure"):
            worker.result()
    worker.close()
    with pytest.raises(RuntimeError, match="closed"):
        worker.submit({})
