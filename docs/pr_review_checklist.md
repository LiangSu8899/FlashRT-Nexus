# FlashRT Nexus PR review checklist

Use this checklist with `CONTRIBUTING.md`. Review the lowest changed layer
first and reject any dependency or policy that moved downward for convenience.

## Scope and layering

- [ ] `core/` remains zero-dependency, mechanism-only, lock-free, and owns no
      loop, thread, model semantics, protocol, scheduler, or eviction policy.
- [ ] `host/` uses capsule types only and contains no FlashRT include, model
      name, graph name assumption, or scenario state machine.
- [ ] Producer-specific conversion stays under `backends/<producer>/`.
- [ ] Scheduling/mode policy stays in `nexus/`; transport and deployment
      lifecycle stay in `serve/` or the application host.
- [ ] A transport adapts calls and never owns producer/model lifetime.

## Model-runtime seam

- [ ] Ports, stages, regions, identity, and fingerprint are treated as
      producer declarations, not reinterpreted model knowledge.
- [ ] STAGED inputs/outputs have real verbs and cannot advertise-and-refuse.
- [ ] SWAP windows are validated before use; SETUP inputs are rejected in a
      tick.
- [ ] Stage dependencies reference earlier stages and graph/stream indices are
      checked before adoption.
- [ ] Mirrored modality/dtype/direction/update enums are compile-time asserted
      in the producer adapter; capsule public headers do not include producer
      ABI headers.
- [ ] A new backend is an instance `cap_backend` implementation, not a
      `backend_kind` field, global registry, or model-runtime fork.
- [ ] Legacy stages and a generic plan are XOR authorities; pure generic GRAPH
      is rejected and OPAQUE executor references remain producer-opaque.
- [ ] Provider-only adoption rejects GRAPH, SWAP, streams, buffers, and regions
      instead of fabricating backend resources.

## Lifecycle and hot path

- [ ] Every successful adopt has one explicit close path; partial failures
      release backend/export/model/event ownership exactly once.
- [ ] Hot verbs, tick, swap, fire, and readiness checks allocate nothing.
- [ ] Capture/prepare remains outside the tick and shape misses fail loudly.
- [ ] Snapshot/restore copies only declared ordered regions and rejects a
      fingerprint mismatch.
- [ ] One context is not driven concurrently unless the owning L2 API provides
      explicit serialization.
- [ ] OPAQUE is blocking and complete on return; no event, pending, async, or
      cancellation behavior is claimed without a separately versioned seam.
- [ ] GRAPH-to-OPAQUE dependencies synchronize required graph streams;
      OPAQUE-to-GRAPH dependencies complete before graph enqueue.
- [ ] Model-level state succeeds only for all-GRAPH runtimes with regions;
      mixed, OPAQUE, and step-only paths fail closed.
- [ ] A loader closes the adopted runtime before its handle-local producer DSO;
      no live extension or callback pointer can outlast `dlclose`.

## Validation

- [ ] Zero-dependency core/stub build and CTest pass.
- [ ] FlashRT adapter build and model adoption tests pass when the seam changes.
- [ ] Relevant scheduler/mode/embedded tests pass.
- [ ] ABI-only CPU build has no CUDA, exec, graph-adapter, or backend undefined
      symbols; graph-enabled tests preserve the legacy graph path.
- [ ] Cross-repository gates use declared ports and avoid model constants.
- [ ] Numerical claims use bit/token equality or a fixed justified tolerance.
- [ ] Behavior changes and migration requirements are documented.

## Public hygiene

- [ ] PR and docs use `<repo>`, `<build-dir>`, `<checkpoint>`, and similar
      placeholders instead of local paths.
- [ ] No user, host, container, environment, credential, internal URL,
      checkpoint, dataset, generated trace, or build artifact is committed.
- [ ] Validation reports disclose only information needed to reproduce the
      public capability claim.
