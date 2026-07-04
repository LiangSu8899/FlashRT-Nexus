# Embedded Nexus

HTTP is a transport, not the model lifecycle. The lifecycle is the resident
deployment session:

```
SETUP -> EXPORT -> ADOPT -> WARM -> SERVE -> DRAIN
```

`serve.embedded.EmbeddedSession` opens the same manifest as `nexus serve`, but
the application calls the session directly with image/state buffers. There is
no HTTP server, JSON parse, base64 decode, or socket hop on this path.

```python
import numpy as np

from serve.embedded import EmbeddedSession

with EmbeddedSession.open("examples/pi05_libero.yaml") as nx:
    images = [np.zeros((224, 224, 3), dtype=np.uint8) for _ in range(3)]
    result = nx.act(images, prompt="pick up the red block", seed=7)
    cap = nx.snapshot("ep-001")
    nx.reset(cap)
```

The host owns the thread. In a robot this is usually the control loop, a ROS2
executor callback, or a shared-memory daemon loop:

```
camera/state ready -> set ports -> tick/mode step -> read actions
```

Nexus core still owns no thread. The embedded session only owns the adopted
model runtime, `cap_ctx`, capsule store, and mode/session state. This is the
first no-HTTP entry; future C++/shm/ROS2 transports should call the same
session semantics instead of adding model-specific loops.
