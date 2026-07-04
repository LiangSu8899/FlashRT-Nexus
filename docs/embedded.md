# Embedded Nexus

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
