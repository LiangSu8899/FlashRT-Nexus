"""Deterministic resident executor for process and action-chunk gates."""

import os
import time

import numpy as np


class Provider:
    def __init__(self, config):
        self.config = config
        self.calls = 0

    def describe(self):
        return {"action_shape": [3, 2], "pid": os.getpid()}

    def execute(self, inputs):
        time.sleep(inputs.get("delay", 0))
        if inputs.get("fail"):
            raise ValueError("fixture failure")
        self.calls += 1
        return np.full((3, 2), inputs.get("value", self.calls), dtype=np.float32)

    def reset(self):
        self.calls = 0

    def close(self):
        pass


def build(config):
    return Provider(config)
