# backends/ — L0: capsule verbs per runtime

One directory per runtime substrate implementing the backend vtable
(buffers, graphs, streams, events, snapshot/restore):

- `stub/` — host-memory reference backend; tests and CI, no GPU.
- `flashrt/` — the FlashRT exec backend plus runtime/model adapters.

New backends must pass `tests/backend_conformance.h`. Nothing above
this layer may reach a runtime directly.
