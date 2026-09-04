"""Synthetic graph contract gate; not model calibration or quality evidence."""

import argparse
from pathlib import Path
import sys

import torch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from flash_rt import structures
from flashrt_nexus import AdoptedRuntime


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--nexus", required=True)
    args = parser.parse_args()
    inputs = torch.ones((2, 3), device="cuda")
    output = torch.empty_like(inputs)

    def hot():
        output.copy_(inputs * 2)
        return output

    stage = structures.capture(hot, windows={"input": inputs, "output": output},
                               reference=lambda: inputs * 2, min_speedup=0)
    exported = stage.export(ports=[
        dict(name=name, modality="tensor", dtype="f32", layout="flat",
             direction=direction, update="swap", shape=(2, 3), window=name)
        for name, direction in (("input", "in"), ("output", "out"))])
    try:
        with AdoptedRuntime(exported, owners=(stage, inputs, output),
                            nexus_lib=args.nexus) as runtime:
            for value in (1, 3, 1):
                with torch.cuda.stream(stage.stream):
                    inputs.fill_(value)
                    runtime.step()
                assert torch.equal(output.cpu(), torch.full((2, 3), value * 2))
    finally:
        exported.release()
    print("PASS - Structures capture/export -> Nexus, dynamic input and replay")


if __name__ == "__main__":
    main()
