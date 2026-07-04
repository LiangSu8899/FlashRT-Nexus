# host/ — L2: the standard model-runtime face

`include/capsule/model_runtime.h`: the first framework surface.
Schedulers, sessions, and runtime loops code against `cap_model_runtime`
in capsule types only — never against a producer's export or a model's
pipeline. Mechanism-thin: lookups, the allocation-free tick, FFI
accessors. Interface norms: `docs/model_runtime.md`.
