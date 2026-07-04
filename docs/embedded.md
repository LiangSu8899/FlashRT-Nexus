# Embedded Nexus: No-HTTP Python Entry

HTTP is a transport, not the model lifecycle. The lifecycle is the resident
deployment session:

```
SETUP -> EXPORT -> ADOPT -> WARM -> SERVE -> DRAIN
```

`serve.embedded.EmbeddedSession` opens the same manifest as `nexus serve`, but
the application calls the session directly with image/state buffers. There is
no HTTP server, JSON parse, base64 decode, or socket hop on this path.
Actions are returned as a NumPy array, not JSON-shaped lists; conversion belongs
at an outer transport boundary, not in the embedded hot path.

Use this entry for:

- robot deployment dry-runs before writing a C++/ROS2 adapter;
- local AI apps that run in one Python process;
- profiling model-runtime ports/stages without HTTP overhead;
- validating snapshot/reset and dynamic inputs from direct arrays.

For a C++ robot loop or transport adapter, use
[`docs/cpp_embedded.md`](cpp_embedded.md).

## Usage

```python
import numpy as np

from serve.embedded import EmbeddedSession

with EmbeddedSession.open("examples/pi05_libero.yaml") as nx:
    images = [np.zeros((224, 224, 3), dtype=np.uint8) for _ in range(3)]
    result = nx.act(images, prompt="pick up the red block", seed=7)
    assert result.actions.shape == (10, 7)
    cap = nx.snapshot("ep-001")
    nx.reset(cap)
```

`EmbeddedSession.open()` runs the same SETUP -> EXPORT -> ADOPT lifecycle as
`nexus serve`. After it returns, the session is resident and the host controls
when ticks happen.

The API surface is intentionally small:

| method | purpose |
|---|---|
| `health()` | phase, fingerprint, capsule count |
| `state()` | identity, ports, action shape, loaded capsules |
| `act(images, state=None, prompt=None, seed=None)` | apply direct arrays, tick once, return action array |
| `snapshot(name)` | freeze current model regions into a named capsule |
| `reset(name)` | restore a named capsule |
| `close()` | release the adopted model/session |

`images` are `uint8` HWC RGB arrays. The producer determines how many views are
required and whether `state` or `prompt` are dynamic ports. If a port was not
exported by the producer, Nexus rejects that dynamic input instead of silently
ignoring it.

## Trigger Model

The host owns the thread. In a robot this is usually the control loop, a ROS2
executor callback, or a shared-memory daemon loop:

```
camera/state ready -> set ports -> tick/mode step -> read actions
```

Nexus core still owns no thread. The embedded session only owns the adopted
model runtime, `cap_ctx`, capsule store, and mode/session state.

For C++ robot loops and transport adapters, use the C ABI in
[`docs/cpp_embedded.md`](cpp_embedded.md). It exposes the same session semantics
as POD structs and includes a batched `nexus_embedded_step()` for ROS2/shm style
control ticks.

## Deployment Patterns

### Robot pre-deploy

Run the Python embedded gate on the target machine after the FlashRT producer
and Nexus shared library are built. This proves the model can be loaded,
exported, adopted, driven from direct arrays, snapshotted, and restored without
HTTP.

Once this passes, the C++/ROS2 adapter should call the C ABI in
`nexus/embedded/session.h` rather than duplicating session logic.

### Local AI

A local app can keep one `EmbeddedSession` open for the lifetime of the
application, feeding screen frames, audio features, or local state through
model-runtime ports. Snapshot/reset then become local workflow operations:
save a warm agent state, fork a branch, or undo to a known state.

## Gate

```sh
export FLASHRT_DIR=/path/to/FlashRT
export PI05_CHECKPOINT=/path/to/pi05_checkpoint
export PI05_LIB=$FLASHRT_DIR/cpp/build/libflashrt_cpp_pi05_c.so
export NEXUS_LIB=./build/libcapsule_nexus_flashrt.so

python tests/gate_pi05_embedded.py --iters 32
```

The gate checks direct array input, deterministic seeds, snapshot/reset, and a
short latency loop without starting an HTTP server.
