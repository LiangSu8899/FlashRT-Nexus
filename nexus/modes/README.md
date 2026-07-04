# nexus/modes/ — interaction modes

A mode is the layer between a scheduler and an application: an
interaction state machine deciding what the application observes
between stage fires. The contract every mode implements (config
struct, state enum + non-blocking poll, allocation at construction,
counter telemetry, C ABI mirror, deadlines as states) is fixed in
[`docs/modes.md`](../../docs/modes.md).

Layout: **one directory per mode**. Family sub-grouping is introduced
only when a family reaches two members.

| Mode | Interaction pattern | Entry |
|---|---|---|
| [`rtc_action_chunk/`](rtc_action_chunk/) | embodied async inference: fire an action stage, execute the previous chunk until the new one is ready, fall back on deadline overrun | `nexus/modes/rtc_action_chunk` |
