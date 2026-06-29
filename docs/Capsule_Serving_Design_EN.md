# Capsule — Serving Substrate Design (over the FlashRT execution contract)

> Status: design draft · 2026-06-28
> A new, standalone repository (`capsule`) that turns the FlashRT execution contract
> (`exec/`, the 3-atom C ABI) and the capsule serving idea (snapshot / restore / fork /
> time-travel) into a **general serving infrastructure base** for physical AI.
> FlashRT is the inference backend; `capsule` is the serving mechanism above it; specific
> deployments (a robot product, lerobot glue, a particular agent app) compose `capsule`
> and live in their own repos.

---

## 0. The one-sentence thesis

> FlashRT's `exec/` contract is the **execution mechanism** (replay a graph, share a buffer,
> DAG the replays). `capsule` is the **serving mechanism**: a small, reusable
> *state + schedule + lifecycle* runtime over that contract, on top of which **every specific
> physical-AI serving mode is a thin composition** — and the truly device/robot-specific
> deployment stays out, in partner repos.

`capsule` is to deployments what `exec/` is to scenarios: it fixes **mechanism, never scenario
policy — recursively**. That recursion is the entire discipline that keeps it from bloating
into an over-engineered framework.

This document is the realization, in production-infra form, of the FlashRT Capsule paper
(`FlashRT_Capsule_Paper_Idea_v2.md` + `arxiv/`). Every paper concept has a home here; every
core abstraction has a paper justification.

---

## 1. Scope — what `capsule` IS and is NOT

| | |
|---|---|
| **IS** (the general infra) | capsule state lifecycle (snapshot/restore/fork/time-travel + GPU↔RAM↔disk tiering) · subgraph scheduling (multi-rate, concurrent, interruptible) · sessions/contexts (metadata journals) · async runtime/driver · the engine adapter seam to FlashRT · generic transport adapters |
| **IS NOT** (stays in partner repos) | a specific robot's cadence/sensors/ROS2 bringup · lerobot glue · a specific agent product's ReAct loop & tools · a specific cloud-edge product's topology · **anything that knows a real device** |
| **IS NOT** (stays in FlashRT) | graph capture · kernels · calibration/autotune · the `frt_*` C ABI · model frontends |

The mission: **be the substrate the deployments compose, own none of them.** Embodied AI is the
first and hardest target (low latency, small batch, edge/cloud-edge hybrid, multi-model, async,
interruptible), and because the substrate is mechanism-only it radiates to every physical-AI
serving mode (omni, local agents, agent workstations).

---

## 2. Dependency boundary (the most important diagram)

```
┌──────────────────────────────────────────────────────────────────────────┐
│  DEPLOYMENTS  (partner repos — OUT)                                         │
│  robot-company deploy · lerobot glue · PKU real-robot + training ·          │
│  a specific agent app · a specific cloud-edge product                       │
└───────────────────────────────┬────────────────────────────────────────────┘
                                 │  compose (import the substrate)
┌───────────────────────────────▼────────────────────────────────────────────┐
│  ███  capsule : serving substrate (general infra)  ███                       │
│                                                                              │
│   modes/        thin reference compositions                                  │
│                 agent · rollout · handoff · duplex · omni · edge             │
│  ──────────────────────────────────────────────────────────────────────     │
│   core/         the "serving contract" — 5 abstractions                      │
│                 Engine · Capsule/Store · Schedule · Session · Runtime         │
│  ──────────────────────────────────────────────────────────────────────     │
│   adapters/     edges (pluggable)                                            │
│                 transport: openai/sse · grpc · ros2 · shm-link               │
│                 engines:   per-frontend adapters (qwen36, pi05, higgs, …)    │
│                 placement: node · link  (cloud-edge-device)                  │
└───────────────────────────────┬────────────────────────────────────────────┘
                                 │  depends on STABLE surfaces ONLY (one-way)
┌───────────────────────────────▼────────────────────────────────────────────┐
│  FlashRT  (backend — pure, additive-only)                                    │
│   frontend capsule/engine surface   (snapshot/restore · graphs · state)      │
│   exec contract  C ABI              (Buffer · Graph · Plan · Event · ShapeKey)│
│   csrc kernels                                                               │
└──────────────────────────────────────────────────────────────────────────────┘
```

**One-way dependency.** `capsule` depends on FlashRT's two *stable* surfaces (the `frt_*` C ABI
and a thin frontend engine/capsule protocol). FlashRT never depends back. Adding a new model =
a new **engine adapter** in `capsule`, never a FlashRT edit. This is the same additive-only seam
the exec contract already enforces (`docs/exec_contract.md` §9.2).

---

## 3. The serving contract — 5 abstractions (mirrors "3 atoms + 1 key")

The exec contract is intentionally tiny (Buffer / Graph / Plan / ShapeKey). `capsule` gets an
equally small object model. Everything else is composition.

```
Engine    a FlashRT-backed model's servable surface: named graphs (ShapeKey variants),
          named state buffers, and optional capability hooks (capsule, tokenize, policy).
          The ONLY thing that touches FlashRT internals. Above this line is FlashRT-agnostic.

Capsule   a frozen, restorable execution boundary (metadata + buffer-set + tier location).
+Store    the registry: snapshot · restore · fork · match · pin/LRU · tier(GPU↔RAM↔disk) ·
          serialize. This is the "ultimate state control".

Schedule  the subgraph scheduler: cadence (multi-rate) · concurrency (streams) · priority ·
          interrupt/preempt — POLICY over the contract's dumb Plan. The "subgraph scheduling".

Session   a stateful serving entity: a METADATA journal (tokens / obs refs) + a bound capsule
          + a (re)entry plan (restore vs append vs rebuild). Never owns device memory.

Runtime   the async driver loop: inputs → schedule.tick → engine.fire → capsule ops → outputs,
          with imperative interrupt points. The "ultimate responsiveness" + async inference.
```

### 3.1 Interface sketches (Python-shaped, deliberately thin)

**Engine** — capability-based, so a VLA and an LLM share the seam without a fat base class:

```python
class Engine(Protocol):
    name: str
    def graphs(self) -> Mapping[str, GraphHandle]   # named; each is a ShapeKey→exec table
    def state(self)  -> Mapping[str, BufferHandle]   # named device buffers (frontend-owned)
    def fire(self, target: PlanHandle | GraphHandle, key: ShapeKey, stream: int) -> None
        # delegates straight to frt_plan_execute / frt_graph_replay — zero added overhead

# capabilities an engine MAY declare (composition, not inheritance):
class Capsuleable(Protocol):
    def snapshot(self, boundary) -> CapsuleData      # frontend copies boundary buffers
    def restore(self, cap: CapsuleData) -> None
    def aligned_boundary(self, n: int) -> int        # chunk-alignment correctness condition
class Tokenizable(Protocol):
    def tokenize(self, messages, tools=None) -> list[int]
    def prefill(self, ids, *, cached=0, K=6) -> None
    def decode_stream(self, *, max_tokens, K) -> Iterable[Chunk]
class Policyable(Protocol):                          # VLA / diffusion
    def act(self, obs_buffer, *, steps_baked=True) -> Action
```

> `snapshot/restore` physically live in the FlashRT frontend (it owns the buffers); the Engine
> just *exposes* them. The substrate decides *when / which* — that is the red line holding.

**Capsule + Store** — the state primitive (the state-control core):

```python
@dataclass(frozen=True)
class Capsule:                 # serving-side HANDLE — metadata only
    id: str; engine: str
    boundary: Boundary         # token pos | episode-init | digest+salt
    tier: Tier                 # GPU | HOST | DISK
    nbytes: int; digest: str

class CapsuleStore:
    def snapshot(self, eng: Engine, boundary, *, pin=False, tier=GPU) -> Capsule
    def restore (self, eng: Engine, cap: Capsule) -> None         # -> eng.restore, same graphs
    def fork    (self, cap: Capsule, n: int) -> list[Session]     # 1 prefill -> N branches
    def match   (self, eng, tokens) -> tuple[Capsule | None, int] # longest-prefix / digest
    def pin/unpin/evict(self, cap) ; def tier(self, cap, to)       # LRU + GPU↔HOST↔DISK
    def serialize(self, cap) -> bytes ; def load(self, b) -> Capsule  # cross-node / persist
```

**Schedule** — the subgraph scheduler (§4 expands this):

```python
@dataclass
class Stage:
    target: PlanHandle | GraphHandle  # the contract DAG for ONE inference (intra-inference)
    key: ShapeKey
    stream: int | Literal["auto"]
    cadence: Cadence                  # EVERY | RATE(1,N) | ON_EVENT | ON_DEMAND
    priority: int

class Schedule:
    stages: list[Stage]
    deps:   list[tuple[int, int]]      # (after, before) -> emitted as frt events
    def due(self, clock, events) -> list[Stage]   # multi-rate: which fire this tick
    def preempt(self, victim, urgent) -> None      # interrupt = stop issuing + high-prio fire
    # Schedule emits ONLY frt_* calls. It captures nothing and owns no device memory.
```

**Session** — metadata-only lifecycle:

```python
@dataclass
class Session:
    id: str; engine: str
    journal: list[int] | EpisodeLog    # tokens / observation refs — METADATA ONLY
    capsule: Capsule | None
    def entry_plan(self, incoming) -> Entry  # RESTORE | APPEND | REBUILD  (the PrefixPlan)
```

**Runtime** — the async driver:

```python
class Runtime:
    async def submit(self, req) -> Stream            # non-blocking; many in flight
    def drive(self, session, schedule):              # imperative outer loop
        while session.alive:
            x = self.poll()                          # token | sensor frame | request
            if self.interrupt():                     # µs, no recapture
                schedule.preempt(...); self.swap_buffer(...)
            for st in schedule.due(self.clock, self.events):
                session.engine.fire(st.target, st.key, st.stream)
            self.emit(...)                           # interrupt granularity = one short replay
```

That is the entire core. Five nouns, each a few methods, each bottoming out in `frt_*`.

---

## 4. Subgraph scheduling — the core feature, expanded

The exec contract gives a **dumb Plan**: a static DAG of `(Graph, ShapeKey)` replays with
data-only deps inside *one* inference. `capsule` adds the **temporal / dynamic policy** the
contract deliberately refuses to hold:

```
  CONTRACT (frt Plan)              SUBSTRATE (Schedule)
  intra-inference, static          inter-inference, dynamic
  ───────────────────────          ─────────────────────────────────────────────
  vision → encoder → action        WHEN does the plan fire? (cadence / event)
  data deps via events             ON WHICH stream + priority?  (concurrency)
  one replay                       interleave N sessions / models  (async)
                                   preempt / interrupt  (response control)
                                   multi-rate fan-out  (planner 1 : actor N)
```

Three canonical schedule shapes cover essentially all physical-AI patterns — exactly the two the
existing FlashRT reference hosts already prove, plus async fan-in:

```
 (A) SEQUENTIAL hand-off   planner ─subtask(shared Buffer)─▶ actor ─▶ act
     multi-rate 1:N         (low rate)                       (high rate)
     interrupt = overwrite the subtask Buffer (µs, no recapture)   ← robot_handoff today

 (B) CONCURRENT co-host    policy(stream P)  ‖  critic(stream C)  → auto-terminate
     gap-fill small-batch   one exec ctx, hardware overlaps          ← robot_recap today

 (C) ASYNC interleave      sess#1 decode(s0) ─┐
     many sessions          sess#2 prefill(s1)─┼─ Runtime interleaves on the event loop
     latency-first          barge-in(s2,hi-prio)┘  ← duplex / agent-workstation
```

Key scheduling decisions the substrate owns (and the contract refuses):

- **Cadence / multi-rate** — planner every N actor ticks; vision 30 Hz → action 50 Hz. Encoded
  per-Stage, not in the graph.
- **Concurrency vs p99** — whether to overlap a second model on another stream to reclaim
  small-batch headroom, *or* keep the GPU idle to protect tail latency. The mechanism
  (multi-stream + event) is free in the contract; the *decision* is here.
- **Interrupt / preempt** — granularity = one short replay (VLA ~17 ms, decode sub-ms). "Stop
  issuing the next replay, fire a high-priority graph." Anything mutable at interrupt cadence
  (subgoal, target pose, mode flag) is a **bound Buffer**, overwritten in µs — never baked into a
  graph.

The discipline that keeps this from becoming a heavyweight scheduler: **Schedule emits only
`frt_*` calls and holds no state but its own stage table.** No queue of GPU work it owns, no
device memory, no capture. If a scheduling feature wants a new graph or buffer, that is a
FlashRT/contract concern, not a serving one.

---

## 5. State control + async runtime — "ultimate state control & responsiveness"

### 5.1 Capsule = the unit of state control AND state mobility

The capsule is the single abstraction that gives ultimate state control, and — crucially — it
doubles as the **cloud-edge transfer unit**:

```
  TIME axis (same engine, different boundary)        SPACE axis (across nodes)
  ──────────────────────────────────────────        ─────────────────────────────────
  snapshot   freeze a boundary                       serialize  capsule -> bytes
  restore    warm-start / undo a turn (time-travel)  ship       over a Link
  fork       1 prefix -> N branches                  load       restore on the other node
  tier       GPU -> host RAM -> disk (working set)   ──────────────────────────────────
                                                     => "plan on cloud, restore on edge"
```

This is what makes the substrate *native* to embodied AI's hard parts (all measured in the paper,
RTX 5090):

- **Hybrid recurrent state** (Qwen3.6 gated-delta-net) is not prefix-addressable → only
  snapshot/restore reuses it. Block/radix KV caches structurally cannot. (Paper R1/R7: capsule
  TTFT flat ~139 ms while vLLM-APC collapses to ~519 ms past ~16k working set.)
- **Episode reset** (RL rollout) = restore-to-initial, *the same verb*. (`verify_capsule.py`
  cosine 1.0, bit-exact.)
- **Barge-in** = restore a pinned persona capsule instead of re-prefilling. (Paper R9:
  384 ms → 235 ms end-to-end TTFA.)
- **Bounded re-entry** (physical world, levels Lv3→Lv0): "restore computation, not the world."
  The *mechanism* is "restore to a chosen boundary tier"; the *level policy* (which tier is valid
  given how much the scene changed) is a Mode / partner concern. The substrate just offers
  restore-at-boundary.

### 5.2 Async runtime — non-blocking, interrupt-first

```
        ┌─────────────── Runtime event loop (asyncio) ───────────────┐
 inputs │  poll(tokens / sensor frames / requests)                   │
   ───▶ │     │                                                      │
        │     ▼                                                      │
        │  interrupt? ──yes──▶ schedule.preempt + swap Buffer (µs)   │
        │     │ no                                                   │
        │     ▼                                                      │
        │  schedule.due(clock,events) ─▶ engine.fire(...) on streams │──▶ GPU (frt replay)
        │     │            (non-blocking; events sync cross-stream)  │      (one cudaGraphLaunch)
        │     ▼                                                      │
 outputs│  emit(committed chunk / action / SSE)  ◀── on event ready  │
   ◀─── └────────────────────────────────────────────────────────────┘
```

The GPU hot path never blocks the host: work is issued on streams, completion observed via the
contract's events, and the loop interleaves many sessions. **Async inference, multi-model, and
multi-subgraph all fall out of "an async loop over the 5 abstractions"** — no bespoke async engine
needed. The GIL is a non-issue because the hot path is GPU replay (the contract is already the
native loop); a C++/Rust Runtime can replace the Python one behind the same interface when a fully
embedded deployment needs it.

---

## 6. Cloud-edge-device — minimal seam, deferred implementation

Do not build a cluster scheduler. Two thin nouns make hybrid expressible without
over-engineering:

```python
class Node:   # a process: hosts Engines + a Runtime (node-local is the honest scope)
class Link:   # moves {messages, Capsule bytes, Buffer bytes} between Nodes
```

Hybrid patterns then compose:

- **Cloud plans, edge acts** — cloud Node runs a planner Engine, ships a `subgoal` Buffer (or a
  Capsule) over a Link; edge Node restores it and runs the actor at high rate.
- **Edge cold-start** — persist a pinned-prefix Capsule to disk (L3); edge resumes instantly
  instead of cold-prefilling.
- **Session migration** — serialize a session's Capsule, ship, restore on a beefier node.

**Recommendation:** ship the `Node/Link` *seam* (interface + a local/in-proc impl) in the core,
defer real networked transports to adapters built when a partner needs them. This honors the
paper's honest "single-node, latency-first" boundary while leaving the door open — and avoids
inventing a distributed system nobody has asked for yet. A capsule is a same-deployment binary
blob (exact weights + quant + kernel + bucketing), so cross-node shipping is a warm-start /
migration mechanism, not a portable cross-version cache.

---

## 7. Repo layout (clear, flat, minimal)

```
capsule/
  core/                              # the 5 abstractions — the "serving contract". NO scenario code.
    engine.py                        #   Engine protocol + capability mixins
    capsule.py                       #   Capsule + CapsuleStore (snapshot/restore/fork/tier/serialize)
    schedule.py                      #   Schedule + Stage + cadence/preempt (subgraph scheduler)
    session.py                       #   Session + SessionRegistry + Entry(restore/append/rebuild)
    runtime.py                       #   async Runtime / driver loop
    placement.py                     #   Node + Link seam (in-proc impl + interface)
    SERVING_CONTRACT.md              #   the spec + the red line (governance doc)
  adapters/
    engines/                         #   per-FlashRT-frontend adapters (thin)
      qwen36.py  pi05.py  higgs.py
    transport/                       #   protocol edges (pluggable)
      openai_sse.py  grpc.py  ros2.py  shm_link.py
  modes/                             #   THIN reference compositions (not deployments)
    agent/        #  LLM warm-start: pin shared prefix, restore-vs-rebuild   (<- qwen36_agent core)
    rollout/      #  RL episode: restore-to-initial, policy‖critic           (<- robot_recap)
    handoff/      #  planner→actor multi-rate buffer hand-off                 (<- robot_handoff)
    duplex/       #  LLM→TTS barge-in (interrupt + persona capsule)
    edge/         #  cloud-plan / edge-act over a Link                        (reference only)
  tests/                             #  conformance: engine-protocol + capsule bit-exact + schedule
  examples/                          #  runnable smoke (community-playable)
  docs/                              #  architecture.md (the diagrams above), migration.md
  pyproject.toml                     #  depends on flashrt (the only hard dep)
```

`core/` imports nothing from `modes/` or `adapters/engines/`. A mode that grows scenario weight
**splits out to a partner repo** — the core never notices.

---

## 8. The radiation — every physical-AI scenario is a composition

The payoff and the proof the core is right: each scenario is
`Engine(s) × Capsule policy × Schedule shape × Session type`, nothing more.

| Scenario | Engine(s) | Capsule use | Schedule | Session |
|---|---|---|---|---|
| **Coding / agent workstation** | qwen36 (Tokenizable + Capsuleable) | pin shared prefix; restore-vs-rebuild; fork branches | async interleave (C) | chat journal |
| **VLA rollout / RL** | pi05 (Policyable + Capsuleable) | restore-to-initial each episode; deterministic replay | concurrent policy‖critic (B) | episode log |
| **Planner→actor (hierarchy)** | 2× pi05 | (handoff, not capsule) | sequential multi-rate (A) | subgoal buffer |
| **Duplex voice / barge-in** | qwen36 + higgs | restore persona capsule on barge-in | async + preempt (C) | dialog journal |
| **Omni / multimodal** | vision + LLM + audio | snapshot per-modality boundaries | concurrent + sequential mix | multimodal context |
| **Local agent (edge)** | small LLM | disk-persisted prefix capsule; warm cold-start | async (C) | session |
| **Cloud-edge hybrid** | planner@cloud + actor@edge | serialize + ship capsule/subgoal over Link | A across Nodes | distributed session |

Same five abstractions, every row. That is "radiate to all physical-AI scenarios" made concrete.

---

## 9. Governance — the red line for `capsule` (anti-over-engineering)

Recursive mechanism-not-policy. A one-page `core/SERVING_CONTRACT.md` red line every PR is
reviewed against:

1. **`core/` is serving mechanism, never scenario policy.** No specific robot cadence, agent ReAct
   loop, or protocol quirk in `core/`. Those are `modes/` (thin) or partner repos.
2. **Never own GPU state.** Engine/frontend owns device buffers; the substrate holds metadata +
   capsule *handles* + schedules. (Same rule the exec contract's §9.2 enforces for
   `SessionRecord`.)
3. **One-way dep on FlashRT's stable surfaces.** New model = new engine adapter, never a FlashRT
   edit. Additive only.
4. **Don't reinvent contract mechanism.** No graph capture, no kernels, no new buffer types in
   serving. If serving "needs a new mechanism," it goes *into the exec contract* as mechanism (the
   way host-backed buffers did), not faked above it.
5. **Modes stay thin; `core/` never imports a mode.** If a mode accretes weight, it splits out.

If a change needs a longer justification than these five lines, it is probably policy in the wrong
place.

---

## 10. Relationship to FlashRT `serving/` — re-implement fresh, don't migrate

> ⚠️ **Revised (2026-06-29):** the original "move infra out" decision is **superseded.** FlashRT stays
> **frozen and non-expanding**; its `serving/` examples **stay as demos, untouched**; `capsule` is
> built **fresh** and references `flash_rt` as a dependency. Full packaging/normalization/ecosystem
> plan in the companion **`Capsule_Serving_Integration_EN.md`**.

Decision: **FlashRT stays a frozen backend + community playground; `capsule` re-implements the
reusable cores clean**, using FlashRT's demos as *reference, not donor code* (one-way dependency,
zero FlashRT churn).

| FlashRT `serving/` demo (reference, stays) | Re-implemented fresh in `capsule` as | Why |
|---|---|---|
| `qwen36_agent/{session,prefix,auto_prefix}.py`, `CapsuleStore`, `engine.py` protocol | `core/{session,capsule,engine}.py` (generalized) | the reusable infra, rebuilt clean and model-agnostic |
| `qwen36_agent/{server,openai_stream,tool_stream}.py` | `adapters/transport/openai_sse.py` + `modes/agent/` | protocol = an edge adapter; agent loop = a thin mode |
| `robot_recap/`, `robot_handoff/`, `robot_host/` | `modes/{rollout,handoff}/` (robot binding → partner) | generic concurrent / sequential multi-model patterns |
| `exec/`, frontends, kernels, `snapshot_capsule/restore_capsule` | **stay in FlashRT** (referenced, not copied) | backend mechanism; reached via one per-model engine adapter |

End state: FlashRT = frozen backend + demos (untouched); `capsule` = all serving infra, depends on a
pinned `flash_rt`; deployments compose `capsule`. Some conceptual duplication between FlashRT demos
and `capsule` core is intended — it buys zero FlashRT churn and zero circular dependency.

---

## 11. Rollout — "5 phases, not 22 weeks" (mirrors the exec contract's discipline)

- **P0 spec (~days):** write `core/SERVING_CONTRACT.md` + the 5 interface stubs; map
  agent + rollout + handoff onto them *on paper* (reuse the exec_contract §4-style table).
  Gate: **no scenario field leaks into `core/`.**
- **P1 extract (~1–2 wk):** lift `SessionRegistry / CapsuleStore / PrefixPlan / engine-protocol`
  out of `qwen36_agent` into `core/`; wire the agent mode back through it. Gate: **qwen36 agent
  E2E + capsule tests token-exact, unchanged.**
- **P2 second family (~1–2 wk):** rollout + handoff modes (Pi05) on the same core. Gate:
  **`verify_capsule` cosine 1.0 + handoff byte-equal preserved; zero new core field.**
- **P3 async + transport (~1–2 wk):** the async Runtime + openai/sse + one more transport. Gate:
  **multi-session async interleave; barge-in latency reproduced (~235 ms).**
- **P4 cloud-edge seam (later):** Node/Link + capsule serialize/ship. Gate: **capsule computed on
  Node A restored on Node B, token/cos-exact.**

Each phase additive, opt-in, default path unchanged — the exact pattern the exec contract shipped
under.

---

## 12. Paper → infra mapping (faithful realization)

| Capsule paper concept | `capsule` home |
|---|---|
| capsule (snapshot / restore / fork / time-travel) | `core/capsule.py` — `CapsuleStore` |
| committed boundary, chunk-alignment correctness | `Capsuleable.aligned_boundary`, `Boundary` |
| buffer hand-off (planner→actor, value pass) | `core/schedule.py` shape (A) + shared Buffer |
| bounded re-entry (Lv3→Lv0) | restore-at-tier mechanism + Mode/partner level policy |
| multi-rate orchestration, interrupt | `Schedule` cadence + `Runtime` preempt |
| working set / pinned capsules | `CapsuleStore` pin / LRU / tier |
| deterministic replay / RL data integrity | capsule + `Runtime` record |
| LLM warm-start, VLA reset, robot rollout under one mechanism | `modes/{agent,rollout,handoff}` over one core |

Nothing in the paper lacks a home; nothing in the core lacks a paper justification. `capsule` is
the production-infra form of the paper's thesis: **FlashRT sessions are checkpointable, forkable,
and restorable — because we capture full execution state — and the same mechanism serves long-
running LLM agents, VLA diffusion policies, robot RL rollouts, and cloud-edge hybrids.**
