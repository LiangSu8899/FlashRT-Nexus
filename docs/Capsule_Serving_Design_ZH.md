# Capsule —— Serving 基座设计（构建于 FlashRT 执行契约之上）

> 状态：设计草案 · 2026-06-28
> 一个全新的独立仓库（`capsule`），把 FlashRT 的执行契约（`exec/`，三原子 C ABI）与
> capsule serving 思想（snapshot / restore / fork / time-travel）落地为面向物理 AI 的
> **通用 serving 基础设施**。
> FlashRT 是推理后端；`capsule` 是其上的 serving 机制；具体部署（某机器人产品、lerobot
> 胶水层、某个特定 agent 应用）只是**组合** `capsule`，并各自留在自己的仓库里。

---

## 0. 一句话立论

> FlashRT 的 `exec/` 契约是**执行机制**（replay 一个 graph、共享一个 buffer、把 replay 串成
> DAG）。`capsule` 是 **serving 机制**：在该契约之上的一套小而可复用的
> *状态 + 调度 + 生命周期* 运行时；在它之上，**每一种具体的物理 AI serving 模式都只是一层薄薄的
> 组合** —— 而真正与设备/机器人强绑定的部署，全部留在外部的合作仓库里。

`capsule` 之于部署，正如 `exec/` 之于场景：它**只固定机制，绝不固定场景策略 —— 而且是递归地。**
这条递归正是让它不会膨胀成过度工程化框架的全部纪律所在。

本文档是 FlashRT Capsule 论文（`FlashRT_Capsule_Paper_Idea_v2.md` + `arxiv/`）在生产基建形态上的
落地实现。论文里的每一个概念在这里都有归宿；核心里的每一个抽象在论文里都有依据。

---

## 1. 范围 —— `capsule` 是什么、不是什么

| | |
|---|---|
| **是**（通用基建） | capsule 状态生命周期（snapshot/restore/fork/time-travel + GPU↔内存↔磁盘 分层）· 子图调度（多频率、并发、可中断）· session/context（仅元数据日志）· 异步 runtime/驱动 · 接入 FlashRT 的 engine 适配缝 · 通用传输适配器 |
| **不是**（留在合作仓库） | 某个机器人的节拍/传感器/ROS2 bringup · lerobot 胶水 · 某个 agent 产品的 ReAct 循环与工具 · 某个云边端产品的拓扑 · **任何"认得真实设备"的东西** |
| **不是**（留在 FlashRT） | graph 捕获 · kernel · 标定/autotune · `frt_*` C ABI · 模型 frontend |

使命：**做被各种部署组合的基座，但不拥有它们中的任何一个。** 具身是第一大、也最难的目标
（低延迟、小 batch、端侧/云边端混合、多模型、异步、可中断）；正因为基座只做机制，它能辐射到所有
物理 AI serving 模式（omni、本地智能体、智能体工作站）。

---

## 2. 依赖边界（最重要的一张图）

```
┌──────────────────────────────────────────────────────────────────────────┐
│  部署层  （合作仓库 —— 在外）                                                 │
│  机器人公司部署 · lerobot 胶水 · 北大真机 + 训练 ·                            │
│  某个 agent 应用 · 某个云边端产品                                            │
└───────────────────────────────┬────────────────────────────────────────────┘
                                 │  组合（import 基座）
┌───────────────────────────────▼────────────────────────────────────────────┐
│  ███  capsule ：serving 基座（通用基建）  ███                                 │
│                                                                              │
│   modes/        薄的参考组合                                                  │
│                 agent · rollout · handoff · duplex · omni · edge             │
│  ──────────────────────────────────────────────────────────────────────     │
│   core/         "serving 契约" —— 5 个抽象                                    │
│                 Engine · Capsule/Store · Schedule · Session · Runtime         │
│  ──────────────────────────────────────────────────────────────────────     │
│   adapters/     边缘（可插拔）                                                │
│                 transport：openai/sse · grpc · ros2 · shm-link               │
│                 engines：  逐 frontend 的适配器（qwen36, pi05, higgs, …）     │
│                 placement：node · link （云边端）                            │
└───────────────────────────────┬────────────────────────────────────────────┘
                                 │  只依赖 FlashRT 的稳定表面（单向）                │
┌───────────────────────────────▼────────────────────────────────────────────┐
│  FlashRT  （后端 —— 纯净、仅新增）                                            │
│   frontend capsule/engine 表面   (snapshot/restore · graphs · state)         │
│   exec 契约  C ABI               (Buffer · Graph · Plan · Event · ShapeKey)   │
│   csrc kernels                                                               │
└──────────────────────────────────────────────────────────────────────────────┘
```

**单向依赖。** `capsule` 依赖 FlashRT 的两个*稳定*表面（`frt_*` C ABI + 一个薄的 frontend
engine/capsule 协议）。FlashRT 永不反向依赖。新增模型 = 在 `capsule` 里加一个 **engine 适配器**，
绝不改 FlashRT。这正是 exec 契约已经在执行的"仅新增"接缝（`docs/exec_contract.md` §9.2）。

---

## 3. serving 契约 —— 5 个抽象（对应"3 原子 + 1 key"）

exec 契约刻意做得极小（Buffer / Graph / Plan / ShapeKey）。`capsule` 也配一个同样小的对象模型，
其余一切皆组合。

```
Engine    一个 FlashRT 后端模型的可服务表面：命名的 graphs（ShapeKey 变体）、命名的状态 buffer、
          以及可选的能力钩子（capsule、tokenize、policy）。
          唯一触碰 FlashRT 内部的东西。这条线之上，全部与 FlashRT 无关。

Capsule   一份冻结、可恢复的执行边界（元数据 + buffer 集合 + 分层位置）。
+Store    注册表：snapshot · restore · fork · match · pin/LRU · tier(GPU↔内存↔磁盘) · serialize。
          这就是"极致的状态控制"。

Schedule  子图调度器：cadence（多频率）· concurrency（多 stream）· priority · interrupt/preempt
          —— 在契约那个"笨 Plan"之上的策略。这就是"子图调度"。

Session   有状态的服务实体：一份元数据日志（tokens / obs 引用）+ 绑定的 capsule + 一个
          (重)进入计划（restore / append / rebuild）。绝不持有设备内存。

Runtime   异步驱动循环：inputs → schedule.tick → engine.fire → capsule ops → outputs，
          带命令式的中断点。这就是"极致的响应" + 异步推理。
```

### 3.1 接口草图（Python 形态，刻意做薄）

**Engine** —— 基于能力（capability），让 VLA 和 LLM 共用同一接缝而无需臃肿基类：

```python
class Engine(Protocol):
    name: str
    def graphs(self) -> Mapping[str, GraphHandle]   # 命名；每个是 ShapeKey→exec 变体表
    def state(self)  -> Mapping[str, BufferHandle]   # 命名设备 buffer（frontend 持有）
    def fire(self, target: PlanHandle | GraphHandle, key: ShapeKey, stream: int) -> None
        # 直接转发到 frt_plan_execute / frt_graph_replay —— 零额外开销

# engine 可以声明的能力（组合，而非继承）：
class Capsuleable(Protocol):
    def snapshot(self, boundary) -> CapsuleData      # frontend 拷贝边界 buffer
    def restore(self, cap: CapsuleData) -> None
    def aligned_boundary(self, n: int) -> int        # chunk 对齐的正确性条件
class Tokenizable(Protocol):
    def tokenize(self, messages, tools=None) -> list[int]
    def prefill(self, ids, *, cached=0, K=6) -> None
    def decode_stream(self, *, max_tokens, K) -> Iterable[Chunk]
class Policyable(Protocol):                          # VLA / diffusion
    def act(self, obs_buffer, *, steps_baked=True) -> Action
```

> `snapshot/restore` 在物理上活在 FlashRT frontend 里（它持有 buffer）；Engine 只是把它们*暴露*出来。
> 由基座决定*何时 / 哪一个* —— 这就是红线在守住的地方。

**Capsule + Store** —— 状态原语（状态控制核心）：

```python
@dataclass(frozen=True)
class Capsule:                 # serving 侧的句柄 —— 只有元数据
    id: str; engine: str
    boundary: Boundary         # token 位置 | episode 初始 | digest+salt
    tier: Tier                 # GPU | HOST | DISK
    nbytes: int; digest: str

class CapsuleStore:
    def snapshot(self, eng: Engine, boundary, *, pin=False, tier=GPU) -> Capsule
    def restore (self, eng: Engine, cap: Capsule) -> None         # -> eng.restore，复用同一组 graph
    def fork    (self, cap: Capsule, n: int) -> list[Session]     # 1 次 prefill -> N 个分支
    def match   (self, eng, tokens) -> tuple[Capsule | None, int] # 最长前缀 / digest
    def pin/unpin/evict(self, cap) ; def tier(self, cap, to)       # LRU + GPU↔内存↔磁盘
    def serialize(self, cap) -> bytes ; def load(self, b) -> Capsule  # 跨节点 / 持久化
```

**Schedule** —— 子图调度器（§4 展开）：

```python
@dataclass
class Stage:
    target: PlanHandle | GraphHandle  # 一次推理的契约 DAG（推理内部）
    key: ShapeKey
    stream: int | Literal["auto"]
    cadence: Cadence                  # EVERY | RATE(1,N) | ON_EVENT | ON_DEMAND
    priority: int

class Schedule:
    stages: list[Stage]
    deps:   list[tuple[int, int]]      # (after, before) -> 发射为 frt event
    def due(self, clock, events) -> list[Stage]   # 多频率：本 tick 该发哪些
    def preempt(self, victim, urgent) -> None      # 中断 = 停止发射 + 高优先发射
    # Schedule 只发 frt_* 调用。它不捕获任何东西，不持有任何设备内存。
```

**Session** —— 仅元数据的生命周期：

```python
@dataclass
class Session:
    id: str; engine: str
    journal: list[int] | EpisodeLog    # tokens / 观测引用 —— 仅元数据
    capsule: Capsule | None
    def entry_plan(self, incoming) -> Entry  # RESTORE | APPEND | REBUILD（即 PrefixPlan）
```

**Runtime** —— 异步驱动：

```python
class Runtime:
    async def submit(self, req) -> Stream            # 非阻塞；多请求在飞
    def drive(self, session, schedule):              # 命令式外循环
        while session.alive:
            x = self.poll()                          # token | 传感器帧 | 请求
            if self.interrupt():                     # 微秒级，无需重捕获
                schedule.preempt(...); self.swap_buffer(...)
            for st in schedule.due(self.clock, self.events):
                session.engine.fire(st.target, st.key, st.stream)
            self.emit(...)                           # 中断粒度 = 一次短 replay
```

整个核心就这些。五个名词，各自几个方法，每个最终都落到 `frt_*`。

---

## 4. 子图调度 —— 核心功能，展开

exec 契约给的是一个**笨 Plan**：一次推理*内部*、由 `(Graph, ShapeKey)` replay 组成的、只表达数据
依赖的静态 DAG。`capsule` 补上契约刻意不持有的**时间/动态策略**：

```
  契约 (frt Plan)                  基座 (Schedule)
  推理内部、静态                    跨推理、动态
  ───────────────────────          ─────────────────────────────────────────────
  vision → encoder → action        Plan 何时发？(cadence / event)
  通过 event 表达数据依赖           在哪个 stream + 优先级？(并发)
  一次 replay                      交错 N 个 session / 模型 (异步)
                                   抢占 / 中断 (响应控制)
                                   多频率扇出 (planner 1 : actor N)
```

三种典型调度形态，几乎覆盖所有物理 AI 模式 —— 正是现有 FlashRT 参考 host 已证明的那两种，外加异步扇入：

```
 (A) 顺序 hand-off       planner ─subtask(共享 Buffer)─▶ actor ─▶ act
     多频率 1:N           (低频)                          (高频)
     中断 = 覆写 subtask Buffer（微秒级，无需重捕获）        ← 当前 robot_handoff

 (B) 并发 co-host        policy(stream P)  ‖  critic(stream C)  → 自动终止
     小 batch 填谷         一个 exec ctx，硬件重叠                ← 当前 robot_recap

 (C) 异步交错             sess#1 decode(s0) ─┐
     多 session           sess#2 prefill(s1)─┼─ Runtime 在事件循环上交错
     延迟优先             barge-in(s2,高优先)┘  ← duplex / 智能体工作站
```

基座持有、而契约拒绝的关键调度决策：

- **Cadence / 多频率** —— planner 每 N 个 actor tick；vision 30 Hz → action 50 Hz。写在每个
  Stage 上，而非烤进 graph。
- **并发 vs p99** —— 是否把第二个模型放到另一个 stream 上重叠以回收小 batch 余量，*还是*让 GPU
  空着以保护尾延迟。机制（多 stream + event）在契约里是免费的；*决策*在这里。
- **中断 / 抢占** —— 粒度 = 一次短 replay（VLA ~17 ms，decode 亚毫秒）。"不发下一次 replay，转而
  发一个高优先 graph。" 任何在中断节拍上可变的东西（subgoal、目标位姿、模式 flag）都是一个**绑定的
  Buffer**，微秒级覆写 —— 绝不烤进 graph。

让它不沦为重型调度器的纪律：**Schedule 只发 `frt_*` 调用，除了自己那张 stage 表外不持有任何状态。**
没有它自己拥有的 GPU 工作队列，没有设备内存，没有捕获。如果某个调度特性想要新的 graph 或 buffer，
那是 FlashRT/契约的事，不是 serving 的事。

---

## 5. 状态控制 + 异步 runtime —— "极致的状态控制与响应"

### 5.1 Capsule = 状态控制单元，也是状态迁移单元

capsule 是赋予极致状态控制的那一个抽象；而且关键在于，它同时充当**云边端的迁移单元**：

```
  时间轴（同一 engine、不同边界）                空间轴（跨节点）
  ──────────────────────────────────────────   ─────────────────────────────────
  snapshot   冻结一个边界                        serialize  capsule -> bytes
  restore    热启动 / 撤销一回合（time-travel）  ship       经由一个 Link
  fork       1 个前缀 -> N 个分支                load       在另一节点 restore
  tier       GPU -> 内存 -> 磁盘（工作集）       ──────────────────────────────────
                                                => "云上规划，端上 restore"
```

这正是基座对具身 AI 那些硬骨头*天然契合*的原因（均为论文实测，RTX 5090）：

- **混合循环状态**（Qwen3.6 gated-delta-net）不可按前缀寻址 → 只有 snapshot/restore 能复用它。
  block/radix KV cache 在结构上做不到。（论文 R1/R7：capsule TTFT 平稳 ~139 ms，而 vLLM-APC 在
  工作集超 ~16k 后崩到 ~519 ms。）
- **episode reset**（RL rollout）= restore-到-初始，*同一个动词*。（`verify_capsule.py` cosine 1.0，
  逐位一致。）
- **barge-in（打断）** = restore 一个 pin 住的 persona capsule，而非重新 prefill。（论文 R9：
  端到端 TTFA 384 ms → 235 ms。）
- **有界重入**（物理世界，Lv3→Lv0 分级）："恢复计算，而非恢复世界"。*机制*是"restore 到某个选定的
  边界 tier"；*分级策略*（场景变了多少、哪个 tier 仍有效）是 Mode/合作方的事。基座只提供
  "在边界处 restore"。

### 5.2 异步 runtime —— 非阻塞、中断优先

```
        ┌─────────────── Runtime 事件循环 (asyncio) ───────────────┐
 输入   │  poll(tokens / 传感器帧 / 请求)                          │
   ───▶ │     │                                                    │
        │     ▼                                                    │
        │  中断? ──是──▶ schedule.preempt + 换 Buffer（微秒级）     │
        │     │ 否                                                 │
        │     ▼                                                    │
        │  schedule.due(clock,events) ─▶ engine.fire(...) 到各 stream │──▶ GPU (frt replay)
        │     │            (非阻塞；event 做跨 stream 同步)         │      (一次 cudaGraphLaunch)
        │     ▼                                                    │
 输出   │  emit(已提交 chunk / 动作 / SSE)  ◀── event 就绪时        │
   ◀─── └──────────────────────────────────────────────────────────┘
```

GPU 热路径永不阻塞 host：工作发射到 stream 上，完成通过契约的 event 观察，循环交错多个 session。
**异步推理、多模型、多子图，全部从"在 5 个抽象之上跑一个异步循环"里自然落出** —— 不需要专门的异步
引擎。GIL 不是问题，因为热路径是 GPU replay（契约本身就是原生循环）；当某个完全嵌入式部署需要时，
一个 C++/Rust 的 Runtime 可以在同一接口背后替换掉 Python 那个。

---

## 6. 云边端 —— 最小接缝，延后实现

不要造集群调度器。两个薄名词就能在不过度工程化的前提下表达混合：

```python
class Node:   # 一个进程：托管若干 Engine + 一个 Runtime（节点本地是诚实的范围）
class Link:   # 在 Node 之间搬运 {消息, Capsule 字节, Buffer 字节}
```

混合模式随之组合：

- **云上规划、端上执行** —— 云 Node 跑一个 planner Engine，把一个 `subgoal` Buffer（或一个
  Capsule）经 Link 送出；端 Node restore 它并高频跑 actor。
- **端侧冷启动** —— 把 pin 住的前缀 Capsule 持久化到磁盘（L3）；端侧瞬间恢复，而非冷 prefill。
- **session 迁移** —— 序列化一个 session 的 Capsule，送出，在更强的节点上 restore。

**建议**：core 里只放 `Node/Link` 这层*接缝*（接口 + 一个本地/进程内实现），把真正的联网传输延后到某个
合作方需要时再作为 adapter 实现。这既尊重论文诚实的"单节点、延迟优先"边界，又留好了门 —— 同时避免
造一个没人要的分布式系统。capsule 是同部署的二进制 blob（精确权重 + 量化 + kernel + 分桶），所以
跨节点搬运是热启动/迁移机制，不是可移植的跨版本缓存。

---

## 7. 仓库结构（清晰、扁平、最小）

```
capsule/
  core/                              # 5 个抽象 —— "serving 契约"。无任何场景代码。
    engine.py                        #   Engine 协议 + 能力 mixin
    capsule.py                       #   Capsule + CapsuleStore（snapshot/restore/fork/tier/serialize）
    schedule.py                      #   Schedule + Stage + cadence/preempt（子图调度器）
    session.py                       #   Session + SessionRegistry + Entry(restore/append/rebuild)
    runtime.py                       #   异步 Runtime / 驱动循环
    placement.py                     #   Node + Link 接缝（进程内实现 + 接口）
    SERVING_CONTRACT.md              #   规范 + 红线（治理文档）
  adapters/
    engines/                         #   逐 FlashRT-frontend 适配器（薄）
      qwen36.py  pi05.py  higgs.py
    transport/                       #   协议边缘（可插拔）
      openai_sse.py  grpc.py  ros2.py  shm_link.py
  modes/                             #   薄的参考组合（不是部署）
    agent/        #  LLM 热启动：pin 共享前缀，restore-还是-rebuild   (<- qwen36_agent 核心)
    rollout/      #  RL episode：restore-到-初始，policy‖critic        (<- robot_recap)
    handoff/      #  planner→actor 多频率 buffer hand-off              (<- robot_handoff)
    duplex/       #  LLM→TTS barge-in（中断 + persona capsule）
    edge/         #  云规划 / 端执行 经由一个 Link                    (仅参考)
  tests/                             #  一致性：engine 协议 + capsule 逐位 + schedule
  examples/                          #  可跑的 smoke（社区可玩）
  docs/                              #  architecture.md（上面的图）, migration.md
  pyproject.toml                     #  依赖 flashrt（唯一硬依赖）
```

`core/` 不 import `modes/` 或 `adapters/engines/` 里的任何东西。一个长出场景重量的 mode
**分裂到合作仓库** —— core 浑然不觉。

---

## 8. 辐射 —— 每个物理 AI 场景都是一次组合

这是回报，也是核心正确的证明：每个场景都是
`Engine(若干) × Capsule 策略 × Schedule 形态 × Session 类型`，仅此而已。

| 场景 | Engine | Capsule 用法 | Schedule | Session |
|---|---|---|---|---|
| **编码 / 智能体工作站** | qwen36（Tokenizable + Capsuleable） | pin 共享前缀；restore-还是-rebuild；fork 分支 | 异步交错 (C) | 对话日志 |
| **VLA rollout / RL** | pi05（Policyable + Capsuleable） | 每 episode restore-到-初始；确定性回放 | 并发 policy‖critic (B) | episode 日志 |
| **planner→actor（层级）** | 2× pi05 | （handoff，非 capsule） | 顺序多频率 (A) | subgoal buffer |
| **双工语音 / barge-in** | qwen36 + higgs | 打断时 restore persona capsule | 异步 + preempt (C) | 对话日志 |
| **Omni / 多模态** | 视觉 + LLM + 音频 | 按模态 snapshot 边界 | 并发 + 顺序混合 | 多模态 context |
| **本地智能体（端侧）** | 小 LLM | 磁盘持久化的前缀 capsule；热冷启动 | 异步 (C) | session |
| **云边端混合** | planner@云 + actor@端 | 经 Link 序列化并送 capsule/subgoal | A 跨 Node | 分布式 session |

每一行都是同样这 5 个抽象。这就是"辐射到所有物理 AI 场景"的具体化。

---

## 9. 治理 —— `capsule` 的红线（反过度工程化）

递归的机制-非-策略。一页 `core/SERVING_CONTRACT.md` 红线，每个 PR 都对照它审：

1. **`core/` 是 serving 机制，绝非场景策略。** core 里不准有某个机器人的节拍、agent 的 ReAct
   循环、或协议的怪癖。那些是 `modes/`（薄）或合作仓库的事。
2. **绝不持有 GPU 状态。** Engine/frontend 持有设备 buffer；基座只持有元数据 + capsule *句柄* +
   schedule。（与 exec 契约 §9.2 对 `SessionRecord` 的规则相同。）
3. **对 FlashRT 稳定表面单向依赖。** 新模型 = 新 engine 适配器，绝不改 FlashRT。仅新增。
4. **不要重造契约机制。** serving 里不准有 graph 捕获、kernel、新 buffer 类型。如果 serving
   "需要一个新机制"，它该*进入 exec 契约*作为机制（就像 host-backed buffer 那样），而不是在上面
   伪造一个。
5. **mode 保持薄；`core/` 永不 import 一个 mode。** 一个 mode 攒了重量就分裂出去。

如果某个改动需要比这五条更长的辩解，它多半是放错地方的策略。

---

## 10. 与 FlashRT `serving/` 的关系 —— 全新重写，不迁移

> ⚠️ **已修订（2026-06-29）：** 原"把基建搬出去"的决策**已取代**。FlashRT 保持**冻结、不扩张**；
> 它的 `serving/` 示例**作为 demo 保留、不动**；`capsule` **全新构建**并把 `flash_rt` 作为依赖引用。
> 完整的打包/规范化/承接生态方案见配套的 **`Capsule_Serving_Integration_ZH.md`**。

决策：**FlashRT 保持冻结后端 + 社区 playground；`capsule` 把可复用核心干净重写**，把 FlashRT 的 demo
当*参考、而非搬运源*（单向依赖，FlashRT 零改动）。

| FlashRT `serving/` demo（参考, 保留） | 在 `capsule` 中全新重写为 | 为什么 |
|---|---|---|
| `qwen36_agent/{session,prefix,auto_prefix}.py`、`CapsuleStore`、`engine.py` 协议 | `core/{session,capsule,engine}.py`（泛化） | 可复用基建, 干净重建且模型无关 |
| `qwen36_agent/{server,openai_stream,tool_stream}.py` | `adapters/transport/openai_sse.py` + `modes/agent/` | 协议 = 边缘适配器；agent 循环 = 一个薄 mode |
| `robot_recap/`、`robot_handoff/`、`robot_host/` | `modes/{rollout,handoff}/`（机器人绑定 → 合作方） | 通用的并发 / 顺序 多模型模式 |
| `exec/`、frontend、kernel、`snapshot_capsule/restore_capsule` | **留在 FlashRT**（被引用, 非拷贝） | 后端机制；经一个 per-model engine 适配器接入 |

终态：FlashRT = 冻结后端 + demo（不动）；`capsule` = 全部 serving 基建, 依赖一个 pin 住的 `flash_rt`；
部署组合 `capsule`。FlashRT demo 与 `capsule` core 间一些概念重复是有意的 —— 换来 FlashRT 零改动、零循环依赖。

---

## 11. 推进 —— "5 个阶段，不是 22 周"（照搬 exec 契约的纪律）

- **P0 规范（~几天）：** 写 `core/SERVING_CONTRACT.md` + 5 个接口桩；把 agent + rollout + handoff
  在*纸面上*映射到它们（复用 exec_contract §4 那种表）。门禁：**没有任何场景字段漏进 `core/`。**
- **P1 抽取（~1–2 周）：** 把 `SessionRegistry / CapsuleStore / PrefixPlan / engine 协议` 从
  `qwen36_agent` 抽进 `core/`；让 agent mode 重新走它。门禁：**qwen36 agent E2E + capsule 测试
  token 逐位一致、不变。**
- **P2 第二个家族（~1–2 周）：** rollout + handoff mode（Pi05）跑在同一 core 上。门禁：
  **`verify_capsule` cosine 1.0 + handoff 逐字节相等 保持；core 零新字段。**
- **P3 异步 + 传输（~1–2 周）：** 异步 Runtime + openai/sse + 再加一个传输。门禁：
  **多 session 异步交错；barge-in 延迟复现（~235 ms）。**
- **P4 云边端接缝（later）：** Node/Link + capsule 序列化/送出。门禁：**在 Node A 算出的 capsule
  在 Node B restore，token/cos 逐位一致。**

每一阶段仅新增、可选开关、默认路径不变 —— 正是 exec 契约出货时的同一套打法。

---

## 12. 论文 → 基建 映射（忠实落地）

| Capsule 论文概念 | `capsule` 归宿 |
|---|---|
| capsule（snapshot / restore / fork / time-travel） | `core/capsule.py` —— `CapsuleStore` |
| committed boundary、chunk 对齐正确性 | `Capsuleable.aligned_boundary`、`Boundary` |
| buffer hand-off（planner→actor，传值） | `core/schedule.py` 形态 (A) + 共享 Buffer |
| 有界重入（Lv3→Lv0） | restore-at-tier 机制 + Mode/合作方的分级策略 |
| 多频率编排、中断 | `Schedule` cadence + `Runtime` preempt |
| 工作集 / pin 住的 capsule | `CapsuleStore` pin / LRU / tier |
| 确定性回放 / RL 数据完整性 | capsule + `Runtime` 录制 |
| LLM 热启动、VLA reset、机器人 rollout 同一机制 | `modes/{agent,rollout,handoff}` 跑在同一 core 上 |

论文里没有一个概念无家可归；核心里没有一个抽象没有论文依据。`capsule` 是论文立论的生产基建形态：
**FlashRT 的 session 是可检查点、可分叉、可恢复的 —— 因为我们捕获了完整执行状态 —— 同一个机制服务
长跑 LLM 智能体、VLA diffusion 策略、机器人 RL rollout、以及云边端混合。**
