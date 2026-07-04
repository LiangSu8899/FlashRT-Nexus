# C/C++ Embedded Session

This is the no-HTTP surface for robot and local-control hosts.

The producer still owns setup:

```
load weights -> capture graphs -> export frt_model_runtime_v1 -> adopt as cap_model_runtime
```

The C/C++ control layer starts after adoption. It receives a
`cap_model_runtime*`, opens a resident `nexus_embedded_session`, and drives the
declared ports/stages directly.

```c
#include "nexus/embedded/session.h"

nexus_embedded_config cfg = {0};
cfg.struct_size = sizeof(cfg);
cfg.model = adopted_model;

nexus_embedded_session* s = NULL;
nexus_embedded_open(&cfg, &s);

nexus_embedded_input in = {0};
in.struct_size = sizeof(in);
in.port = "obs";
in.data = camera_bytes;
in.bytes = camera_nbytes;
in.update = NEXUS_EMBEDDED_SWAP;
in.stream = 0;

nexus_embedded_output out = {0};
out.struct_size = sizeof(out);
out.port = "actions";
out.data = action_buffer;
out.capacity = action_capacity;
out.stream = -1;

nexus_embedded_tick_result tr = {0};
nexus_embedded_step(s, &in, 1, &out, 1, &tr);

nexus_embedded_snapshot(s, "ep-001");
nexus_embedded_restore(s, "ep-001");
nexus_embedded_close(s);
```

`nexus_embedded_step()` is the transport adapter seam. A ROS2, shared-memory,
camera SDK, or in-process controller maps its incoming buffers to
`nexus_embedded_input[]`, maps action destinations to `nexus_embedded_output[]`,
and calls one step per control tick.

The session owns no thread. The host loop decides when to call it:

```
ROS2 callback / control timer / shm eventfd
  -> fill nexus_embedded_input[]
  -> nexus_embedded_step()
  -> publish or consume nexus_embedded_output[]
```

## Persistence

The C ABI does not choose a filesystem policy. It exposes capsule blobs:

```c
size_t n = 0;
nexus_embedded_serialize(s, "ep-001", NULL, &n);
/* host allocates/writes n bytes */
nexus_embedded_load(s, "ep-001", blob, n);
```

Loaded capsules are restored with `cap_restore_into` under the hood, because a
loaded blob has no original live buffer addresses. Same-process snapshots use
`cap_restore`.

## Gate

`test_embedded_session` builds a fake model-runtime face over the stub backend
and verifies:

- SWAP input + tick + output
- STAGED input + tick + output
- batched `nexus_embedded_step()`
- snapshot/restore
- serialize/load + restore into current regions
- capsule name filtering

This test is dependency-free and runs in `ctest`.
