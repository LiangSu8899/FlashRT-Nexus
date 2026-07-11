## Summary

<!-- Describe the behavior and the lowest layer that owns it. -->

## Contract boundaries

<!-- Explain lifecycle, mechanism/policy, backend, and model-semantic ownership. -->

## Compatibility

<!-- ABI, schema, fingerprint, capsule, transport, and migration impact. -->

## Validation

<!-- Use sanitized commands/results with no private paths or environments. -->

- [ ] Core remains zero-dependency and mechanism-only
- [ ] Host remains capsule-typed and model-agnostic
- [ ] STAGED ports have real producer verbs
- [ ] Lifecycle and partial-failure cleanup are tested
- [ ] Hot paths remain allocation-free
- [ ] Producer/capsule enum mirrors are compile-time asserted
- [ ] Relevant core, conformance, mode, and embedded tests pass
- [ ] Docs and behavior-change notes are updated
- [ ] Diff contains no private paths, hosts, containers, credentials, or logs
