# nexus/ — L2: the framework (scenario-free machinery)

Reusable machinery above the model-runtime face. Nothing in here knows
a model name, a modality beyond its configured ports, or a deployment
scenario — that knowledge belongs in `scenarios/` (assemblies) or in
producers.

- `schedulers/` — stage-DAG runners and scheduling primitives
  (fire/query/sync, masks, period/phase tables).
- `state/` — policy stores over backend mechanisms (graph-cache
  budget, capsule stores).
- `modes/` — interaction state machines between a scheduler and an
  application; one directory per mode. Contract:
  `docs/modes.md`; index: `modes/README.md`.
