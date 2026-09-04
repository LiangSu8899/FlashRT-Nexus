# Resident execution workers

`ExecutionWorker` loads a user factory in one spawned Python process. The
factory returns an object implementing `describe()`, `execute(inputs)`,
`reset()`, and `close()`. Loading and device initialization happen in that
process. The provider owns preprocessing, inference, and postprocessing.

```python
from flashrt_nexus import ExecutionWorker, WorkerActionChunks

with ExecutionWorker("my_provider:build", config) as worker:
    chunks = WorkerActionChunks(worker, execute_horizon=4)
    try:
        chunks.request_inputs(observation)
        chunks.wait_ready(30)
        action = chunks.next_action()
    finally:
        chunks.close()
```

For action chunks, `describe()` includes `action_shape: [length, dimensions]`
and `execute()` returns a finite array of that shape in robot action space.
The generic worker itself does not interpret model inputs or outputs.

One request may be outstanding until its result is collected. Input data is
serialized at submission before control returns, so the caller may reuse its
observation buffers. Submission and polling do not wait for inference. Array
serialization and transport still have a cost; this path does not claim zero
copy, kernel preemption, or CUDA graph scheduling. Python serialization is
only for trusted local child processes, not an external network protocol.

`WorkerActionChunks` feeds the existing C action-chunk mode through the
additive `nexus_action_chunk_create_external` callback interface. Native graph
and external execution share consumption, deadline, fallback, and statistics
logic. External requests are explicit: the host supplies fresh observations.
Provider-state projection and raw-prefix writes are not exposed by this path.
Reset waits for the running request, discards its output, resets the provider,
and clears the chunk mode. Close drains execution. Neither operation promises
to interrupt a hung provider; worker supervision and alternate Python
environments are separate follow-up work.

The execution-only shared library needs neither FlashRT headers nor CUDA:

```sh
cmake -S . -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure
python tests/gate_worker_chunks.py --nexus build/libcapsule_nexus.so
tools/build_native_wheel.sh --execution-only
```

Pass a library explicitly or install it under `flashrt_nexus/lib`. Native
model-runtime deployments continue to use the FlashRT-enabled shared library.

The Python native action-chunk host also accepts a single declared stage.
Split runtimes retain their context/action path. OPAQUE stages remain
synchronous; use an execution worker when a host call must run off the control
thread. A pending request is rejected before staging new native inputs.

For Structures exports, `AdoptedRuntime(export, owners=(stage, ...))` provides
blocking execution of the declared plan inside a worker. The caller retains
the export and tensors, updates input windows on the producer stream, and
applies its own output processors after `step()`. `close()` drains Nexus and
releases its adoption before the caller releases the producer export. This
path requires a graph-enabled library and compatible FlashRT exec build.

`tests/gate_captured_runtime.py --nexus <graph-library>` validates that bridge
with a synthetic CUDA graph and changing inputs. It is a contract test, not a
policy accuracy or acceleration benchmark.
