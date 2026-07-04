# Nexus Serve: HTTP Transport

`nexus serve` is the HTTP product entry over the Nexus lifecycle:

```
SETUP -> EXPORT -> ADOPT -> WARM -> SERVE -> DRAIN
```

It is a transport adapter, not the lifecycle itself. The producer loads the
model, captures graphs, and exports the standard model-runtime face. Nexus
adopts that face, owns a resident session, and maps HTTP requests to declared
ports/stages plus capsule verbs.

Use HTTP for demos, remote debugging, local dashboards, and non-real-time
clients. For a same-process robot loop or low-latency local AI loop, use the
embedded entries in [`docs/embedded.md`](embedded.md) and
[`docs/cpp_embedded.md`](cpp_embedded.md).

The producer is loaded through `producer.entry` (`module:function`). The Pi0.5
example uses a bundled plugin; production plugins can live beside their
FlashRT producers while returning the same `ProducerHandle` shape to Nexus.

## Lifecycle

| phase | owner | meaning |
|---|---|---|
| SETUP | producer | load weights, initialize runtime, run calibration/warmup needed for capture |
| EXPORT | producer | expose `frt_model_runtime_v1`: ports, stages, streams, regions, identity |
| ADOPT | Nexus | adopt the model-runtime face into `cap_model_runtime` and create `cap_ctx` |
| WARM | producer/Nexus | prepare any required graph variants before serving |
| SERVE | transport/mode | apply inputs, tick or fire stages, read outputs, snapshot/reset |
| DRAIN | Nexus | release model/session resources; persistent capsules remain on disk |

## Security posture

The shell binds `127.0.0.1` by default and carries no authentication: it is a
same-host control surface, like a robot-local daemon. To expose it beyond the
host, front it with an authenticating reverse proxy — do not just set
`serve.host: 0.0.0.0`. Request bodies are capped (64 MiB); one model session
serializes its mutating verbs (`act`/`snapshot`/`reset`), so concurrent
clients are safe but share one model's throughput. `/v1/state` intentionally
reports the full identity string (white-box operations); treat it as
deployment metadata, not a secret.

For true robot hot paths, do not send camera frames as JSON/base64 HTTP. Use
HTTP to verify lifecycle and behavior, then put the real control loop on the
embedded C/Python session API.

## Run

```sh
export FLASHRT_DIR=/path/to/FlashRT
export PI05_CHECKPOINT=/path/to/pi05_checkpoint
export PI05_LIB=$FLASHRT_DIR/cpp/build/libflashrt_cpp_pi05_c.so
export NEXUS_LIB=./build/libcapsule_nexus_flashrt.so

bin/nexus serve examples/pi05_libero.yaml
```

Health and state:

```sh
curl http://127.0.0.1:8080/healthz
curl http://127.0.0.1:8080/v1/state
```

## Act API

Action request:

```json
POST /v1/act
{
  "images": [
    {"width": 224, "height": 224, "data": "<base64 rgb bytes>"},
    {"width": 224, "height": 224, "data": "<base64 rgb bytes>"},
    {"width": 224, "height": 224, "data": "<base64 rgb bytes>"}
  ],
  "state": [0.0, 0.0, 0.0],
  "prompt": "pick up the red block",
  "seed": 7
}
```

Response:

```json
{
  "actions": [[0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0]],
  "chunk_id": 1,
  "latency_ms": 30.7
}
```

`images` is required by the Pi0.5 example. `prompt` and `state` are routed only
when the producer exports matching `prompt`/`text` or `state` ports. If a
producer bakes prompt handling into setup, a request may repeat the setup
prompt but cannot change it dynamically.

Session verbs:

```sh
curl -X POST http://127.0.0.1:8080/v1/session/snapshot \
  -H 'content-type: application/json' -d '{"capsule":"ep-001"}'

curl -X POST http://127.0.0.1:8080/v1/session/reset \
  -H 'content-type: application/json' -d '{"capsule":"ep-001"}'
```

When `state.capsule_dir` is set, snapshots are serialized as `<capsule>.cap`
with an fsync'd temporary file, atomic replace, and parent-directory fsync.
They are loaded again on the next `nexus serve` startup. Capsule names are
restricted to `[A-Za-z0-9][A-Za-z0-9_.-]{0,127}` so a request cannot escape the
configured directory.

Capsule correctness is deployment-bound. Within one live capture/deployment,
snapshot and restore are bit-exact at the declared boundary. A serialized
capsule reloads into the current live deployment after the fingerprint guard
accepts it. It does not promise that two independent process startups or graph
captures will produce bitwise-identical future floating-point outputs: producer
autotune, kernel algorithm selection, driver versions, and hardware can make
small FP differences while preserving the same runtime contract. For
bit-exact validation, compare inside one process/capture, or pin the producer's
autotune and kernel-selection settings.

## Deployment Patterns

### Robot lab demo

Run `nexus serve` on the robot workstation or edge box bound to localhost. A
test script, dashboard, or teleop tool sends `/v1/act` requests. This validates
model setup/export/adoption, action shape, capsule reset, and error handling.

Do not expose this endpoint directly on a robot network. If remote access is
needed, put an authenticating reverse proxy in front of it and keep the Nexus
process bound to localhost.

### Local AI demo

Use HTTP when an app already speaks JSON or OpenAI-style protocols and latency
is dominated by model execution rather than serialization. The same session
verbs can snapshot an agent state, reset a local workflow, or inspect the
adopted model identity.

### Production hot path

Keep the manifest and producer plugin, but replace `serve.transport: act_http`
with an embedded/shm/ROS2 adapter. The adapter should call the same session
semantics documented in [`docs/cpp_embedded.md`](cpp_embedded.md):

```
camera/state ready -> fill inputs -> nexus_embedded_step -> publish actions
```

## Boundary

The serve shell does not interpret graph names, subgraph cuts, or model
internals. The producer owns capture and exports ports/stages. The shell maps
HTTP payloads to declared model-runtime ports and drives `cap_model_tick` plus
capsule verbs.

## Manifest

```yaml
model:
  checkpoint: ${PI05_CHECKPOINT:-/path/to/pi05_checkpoint}
  config: pi05
  framework: torch
  hardware: auto
  precision: fp16
  num_views: 3
  steps: 10
  stage_plan: full
  io: native
  prompt: pick up the red block
producer:
  kind: python
  entry: serve.producer_plugins.pi05:build
  flashrt_dir: ${FLASHRT_DIR:-/path/to/FlashRT}
  nexus_lib: ${NEXUS_LIB:-build/libcapsule_nexus_flashrt.so}
  native_verbs: ${PI05_LIB:-/path/to/libflashrt_cpp_pi05_c.so}
mode:
  kind: tick
serve:
  transport: act_http
  host: 127.0.0.1
  port: 8080
state:
  capsule_dir: ./capsules
```
