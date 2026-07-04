# Nexus Serve

`nexus serve` is the product entry over the existing mechanism. It owns the
deployment lifecycle:

```
SETUP -> EXPORT -> ADOPT -> WARM -> SERVE -> DRAIN
```

The first implementation is intentionally narrow and useful: a Pi0.5 VLA
producer exports the standard model-runtime face, Nexus adopts it, and an Act
HTTP transport serves synchronous action ticks plus session snapshot/reset.
The producer is loaded through `producer.entry` (`module:function`); the Pi0.5
example uses a bundled plugin, and future model plugins can live with their
FlashRT producers while exposing the same handle to Nexus.

## Security posture

The shell binds `127.0.0.1` by default and carries no authentication: it is a
same-host control surface, like a robot-local daemon. To expose it beyond the
host, front it with an authenticating reverse proxy — do not just set
`serve.host: 0.0.0.0`. Request bodies are capped (64 MiB); one model session
serializes its mutating verbs (`act`/`snapshot`/`reset`), so concurrent
clients are safe but share one model's throughput. `/v1/state` intentionally
reports the full identity string (white-box operations); treat it as
deployment metadata, not a secret.

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

Session verbs:

```sh
curl -X POST http://127.0.0.1:8080/v1/session/snapshot \
  -H 'content-type: application/json' -d '{"capsule":"ep-001"}'

curl -X POST http://127.0.0.1:8080/v1/session/reset \
  -H 'content-type: application/json' -d '{"capsule":"ep-001"}'
```

When `state.capsule_dir` is set, snapshots are serialized as `<capsule>.cap`
with an atomic replace and are loaded again on the next `nexus serve` startup.
Capsule names are restricted to `[A-Za-z0-9][A-Za-z0-9_.-]{0,127}` so a
request cannot escape the configured directory.

## Boundary

The serve shell does not interpret graph names, subgraph cuts, or model
internals. The producer owns capture and exports ports/stages. The shell maps
HTTP payloads to declared model-runtime ports and drives `cap_model_tick` plus
capsule verbs.

`images` is required for the Pi0.5 example. `prompt` and `state` are routed only
when the producer exports matching `prompt`/`text` or `state` ports; otherwise a
dynamic value is rejected instead of silently ignored. A request that repeats the
setup prompt remains valid for producers that bake prompt handling into setup.

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
