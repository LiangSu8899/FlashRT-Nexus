# Compatibility notes

## Unreleased

### Action-chunk model-port staging

- `state_input_port` is valid only with the `projected_state` prepare policy.
  Configurations that previously supplied it with another policy and had it
  ignored now fail during mode creation. Silent configuration loss is not a
  supported compatibility behavior.
- A nonzero `state_input_port` must reference a compatible
  `STATE/F32/STAGED` input port with a working producer `set_input` verb.
  Invalid modality, dtype, direction, update class, rank, or shape fails at
  setup, before any request fires.
- Capsule model-schema enum aliases are now available from
  `capsule/model_runtime.h`. They mirror producer-v1 values without changing
  the `cap_model_runtime` or `cap_model_port` layouts.
