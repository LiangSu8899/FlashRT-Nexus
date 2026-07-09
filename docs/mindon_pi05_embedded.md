# Mindon Pi0.5 Embedded Integration

This document is the Nexus-side companion to the FlashRT Pi0.5 native IO
contract. It explains how a Mindon C++ `vla_runner` should use Nexus without
adding model-specific behavior to Nexus.

Nexus consumes the producer-declared model-runtime face. It does not tokenize
prompts, normalize state, resize images, denormalize actions, parse graph
names, or inspect Pi0.5 internal buffers.

## Supported Lanes

### Lane A: Current Contract

Lane A removes Python from the hot loop while keeping Python as the setup
producer:

```
FlashRT Python setup
  load checkpoint -> capture graphs -> export frt_model_runtime_v1
Nexus adoption
  flashrt_adopt_model_runtime -> cap_model_runtime
Mindon hot loop
  nexus_embedded_step / cap_model_tick
```

The current Pi0.5 native face exports:

| port | update | Nexus action |
|---|---|---|
| `images` | `STAGED` | call `nexus_embedded_set_input` or include a staged input in `nexus_embedded_step` |
| `noise` | `SWAP` | write the declared buffer window directly |
| `actions` | `STAGED` output | call `nexus_embedded_get_output` or include an output in `nexus_embedded_step` |

Prompt is setup-time in Lane A. The current producer does not export hot
`prompt` or `state` ports.

### Lane B: After FlashRT Prompt/State Staging

When FlashRT exports real `prompt: TEXT/STAGED` and `state: STATE/STAGED`
ports, Nexus adoption automatically exposes them as `cap_model_port` entries.
Mindon sends those inputs with the same embedded/session APIs. Nexus core does
not change.

### Lane C: Future Native Producer

When FlashRT implements `frt_model_runtime_open_v1(config_json, &out)`, the
Mindon host adopts the returned `frt_model_runtime_v1*` exactly as in Lane A.
The producer language changes; Nexus and the control loop shape do not.

## Embedded Session Flow

Use the no-HTTP embedded ABI for robot or local-control loops:

```c
nexus_embedded_config cfg = {0};
cfg.struct_size = sizeof(cfg);
cfg.model = adopted_model;

nexus_embedded_session* session = NULL;
nexus_embedded_open(&cfg, &session);

nexus_embedded_input inputs[2] = {0};
/* Fill inputs from declared ports. */

nexus_embedded_output outputs[1] = {0};
/* Fill output destination for the declared actions port. */

nexus_embedded_tick_result result = {0};
nexus_embedded_step(session, inputs, 2, outputs, 1, &result);
```

[`examples/mindon_pi05_embedded_host.cpp`](../examples/mindon_pi05_embedded_host.cpp)
contains the same shape as a compile-checked C++ wrapper. It starts from an
already adopted `cap_model_runtime*`, verifies `images`/`noise`/`actions` port
update classes, sends optional `prompt`/`state` only when those ports exist,
and leaves all Pi0.5 formatting and tensor semantics in the FlashRT producer.

Transport adapters, ROS2 callbacks, shared-memory readers, or camera SDK loops
should only map external buffers into `nexus_embedded_input[]` and
`nexus_embedded_output[]`. They should not own model setup/capture or interpret
model internals.

## Port Handling Rules

Discover ports by name and inspect their update class before writing.

For `SWAP` ports:

- get the `cap_buffer` window from the adopted `cap_model_runtime`;
- check `port.bytes` before copying;
- write with `nexus_embedded_swap`, `cap_swap`, or the equivalent helper;
- do not call the producer `set_input` verb.

For `STAGED` ports:

- call `nexus_embedded_set_input` or provide a staged input in
  `nexus_embedded_step`;
- pass the payload bytes required by the producer's modality;
- treat producer errors as input-contract failures.

For `SETUP` ports:

- do not update inside a control tick.

## Action Output

Mindon must derive action buffer capacity from the declared `actions` port
shape and dtype. The Pi0.5 producer may expose `(10, 7)`, `(50, 7)`, or another
fixed action chunk. Nexus does not hard-code action shape.

The `actions` port is the logical robot action output after producer
postprocess. If a deployment needs raw model action state, it must use a
separate producer-declared raw port such as `actions_raw`.

## Prompt and State

Pi0.5 state is producer-owned semantics. It is rendered into prompt tokens by
FlashRT, not Nexus.

Current Lane A behavior:

- prompt and state are not hot ports;
- changing prompt/state requires a producer-side setup refresh;
- Nexus can still snapshot/restore the declared runtime regions.

Future Lane B behavior:

- Mindon writes `prompt` and `state` as ordinary STAGED ports;
- the FlashRT producer formats, tokenizes, embeds, and writes graph-safe
  prompt buffers;
- Nexus adoption and embedded sessions stay unchanged.

## Capsules

Capsules snapshot the producer-declared regions in contractual order. The
capsule fingerprint is the producer fingerprint. A restore failure caused by a
fingerprint mismatch is the correct response to a different deployment
identity.

Mindon should use:

- `nexus_embedded_snapshot` for same-process episode boundaries;
- `nexus_embedded_restore` for rollback/reset into the same live deployment;
- `nexus_embedded_serialize` and `nexus_embedded_load` only when the host owns
  persistence policy.

Capsule bytes are opaque to Nexus and to the transport. Do not parse internal
buffers in a transport adapter.

## Optional Msgpack or Websocket Adapter

If Mindon keeps an existing msgpack/websocket protocol, implement it as an L3
transport adapter over the same embedded/session semantics:

```
network message -> declared port payloads -> nexus_embedded_step -> response
```

Boundary rules:

- msgpack/websocket code stays in the transport adapter;
- no `frt_*` producer internals leak into the protocol;
- the adapter does not load checkpoints or capture graphs;
- the adapter does not assume graph names or internal buffer names;
- model-specific parsing remains in the FlashRT producer.

## Gate Checklist

- Nexus core, backend, scheduler, and mode code have no Pi0.5-specific diff.
- `flashrt_adopt_model_runtime` succeeds and exposes the expected ports.
- `nexus_embedded_step` can run Lane A for a fixed prompt.
- Snapshot -> restore -> continue works for the declared regions.
- When FlashRT adds prompt/state ports, adoption shows two additional ports
  without a Nexus core change.
- Any transport adapter proves it only maps protocol payloads to declared
  ports and session verbs.
