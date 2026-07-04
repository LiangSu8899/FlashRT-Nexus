# Nexus Serve

`nexus serve` is the product entry over the existing mechanism. It owns the
deployment lifecycle:

```
SETUP -> EXPORT -> ADOPT -> WARM -> SERVE -> DRAIN
```

The first implementation is intentionally narrow and useful: a Pi0.5 VLA
producer exports the standard model-runtime face, Nexus adopts it, and an Act
HTTP transport serves synchronous action ticks plus session snapshot/reset.

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

## Boundary

The serve shell does not interpret graph names, subgraph cuts, or model
internals. The producer owns capture and exports ports/stages. The shell maps
HTTP payloads to declared model-runtime ports and drives `cap_model_tick` plus
capsule verbs.

`images` is required for the Pi0.5 example. `prompt` and `state` are routed only
when the producer exports matching `prompt`/`text` or `state` ports; otherwise a
dynamic value is rejected instead of silently ignored. A request that repeats the
setup prompt remains valid for producers that bake prompt handling into setup.
