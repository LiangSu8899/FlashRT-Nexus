# C/C++ Embedded Session

This is the no-HTTP surface for robot and local-control hosts.

The producer still owns setup:

```
load weights -> capture graphs -> export frt_model_runtime_v1 -> adopt as cap_model_runtime
```

The C/C++ control layer starts after adoption. It receives a
`cap_model_runtime*`, opens a resident `nexus_embedded_session`, and drives the
declared ports/stages directly.

This layer does not load checkpoints and does not capture graphs. That remains
the FlashRT producer's job. This layer is the stable control-loop surface below
HTTP and above the adopted model-runtime face.

## Interface

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

Core calls:

| function | purpose |
|---|---|
| `nexus_embedded_open` / `close` | create and release the resident session over `cap_model_runtime*` |
| `nexus_embedded_set_input` | send a STAGED input through the producer verb |
| `nexus_embedded_swap` | write a SWAP port window directly |
| `nexus_embedded_tick` | run the declared model stage DAG once |
| `nexus_embedded_get_output` | read one output port |
| `nexus_embedded_step` | batch input updates, one tick, and output reads for one control cycle |
| `nexus_embedded_snapshot` / `restore` | named same-process capsules |
| `nexus_embedded_serialize` / `load` | host-defined persistence or transport of capsule blobs |

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

## Robot Deployment

The intended robot structure is:

```
FlashRT producer process / library
  -> frt_model_runtime_v1
  -> flashrt_adopt_model_runtime(...)
  -> cap_model_runtime*
  -> nexus_embedded_open(...)

ROS2/shm/control loop
  -> sensor buffers become nexus_embedded_input[]
  -> action buffers become nexus_embedded_output[]
  -> nexus_embedded_step(...)
```

For ROS2:

- camera topics map to IMAGE/STAGED or IMAGE/SWAP ports, depending on the
  producer's exported port class;
- robot joint/state topics map to STATE ports;
- command/prompt updates map to TEXT/STAGED ports if exported;
- the action publisher reads `nexus_embedded_output.written` and publishes the
  action chunk;
- the ROS2 executor owns the thread; Nexus owns no executor and no callback.

For shared memory:

- a camera or perception process writes frame slots into a ring buffer;
- the Nexus adapter reads the slot pointer and fills `nexus_embedded_input`;
- an action ring buffer or controller-owned array becomes `nexus_embedded_output`;
- synchronization policy belongs to the adapter, not the capsule core.

For direct camera SDKs:

- CPU RGB frames can be passed as direct host buffers;
- pinned/GPU camera buffers should be represented by the producer's chosen
  port/update contract;
- Nexus does not reinterpret pixel formats beyond what the producer declares.

## Local AI Deployment

Local apps can use the same ABI when Python is not desired on the hot path:

```
screen/audio/local state event
  -> nexus_embedded_input[]
  -> nexus_embedded_step()
  -> token/action/audio output buffers
```

The important property is the same as robotics: the application owns the event
loop and calls Nexus when work is due. HTTP is not involved.

## Threading

One `nexus_embedded_session` serializes mutating verbs internally so a threaded
transport cannot drive one `cap_ctx` concurrently. For maximum determinism, a
robot control loop should still treat a session as a single-thread-owned object
and use the transport layer to serialize callbacks.

Multiple models should use multiple sessions. Cross-model scheduling is an L2
mode/policy concern and should not be hidden inside a transport adapter.

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

The bit-exact boundary is the live deployment/capture. A same-process
snapshot/restore is bit-exact for the declared regions. A blob loaded after a
restart is guarded by the fingerprint and restores into the current live
regions, but the next model tick may still differ slightly from a previous
process if the producer recaptured graphs or autotuned kernels differently.
Pin producer autotune settings when cross-process bitwise replay is a product
requirement.

## Gate

`test_embedded_session` builds a fake model-runtime face over the stub backend
and verifies:

- SWAP input + tick + output
- STAGED input + tick + output
- batched `nexus_embedded_step()`
- multi-threaded calls serialize through one session
- snapshot/restore
- serialize/load + restore into current regions
- capsule name filtering

This test is dependency-free and runs in `ctest`.
