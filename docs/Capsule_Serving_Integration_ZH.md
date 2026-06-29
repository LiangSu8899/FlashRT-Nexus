# Capsule —— 衔接与打包附录（`capsule` 如何引用 FlashRT、如何承接生态）

> 状态：设计附录 · 2026-06-29 · 配套 `Capsule_Serving_Design_ZH.md`
> 回答具体的接缝问题：新 `capsule` 仓库如何引用 FlashRT、FlashRT 要怎么规范化、
> 编译/硬件/选择性编译归谁管、以及 serving 基座如何承接下游生态（lerobot、北大、agent、omni）。
>
> ⚠️ **姿态已被 `Capsule_Core_Spec_ZH.md`（权威核规范）修订。** 核是 **C++、零依赖、主权**；FlashRT 是
> **抽象接缝之后的第一个 backend（L0）,不是核的依赖**。本附录请读作**FlashRT *backend 适配器*如何打包与
> 选择性构建**（arch × model、指纹、三方归属）——这些对 L0 仍然成立。凡本文说"capsule 是纯 Python / 依赖
> flash_rt"之处,请替换为:核是 C++、不依赖任何东西；Python 是边缘的一个可选绑定；那个 flash_rt 依赖属于
> `backends/flashrt` 适配器,不属于核。

---

## 0. 决策反转 —— FlashRT 保持冻结（取代主设计 §10）

主设计的 §10 说"把 serving 基建从 FlashRT 搬出去"。**此决策反转。** 修订后的更优决策：

> **FlashRT 保持冻结、不再扩张。** 它是*库*：kernels + exec 契约 + 基础 runtime + 测试 + 一个加模型
> 和验证 demo 的 playground。它的 `serving/` 示例**作为 demo 保留、不动。** **`capsule` 全新构建**，
> 作为承接生态/定义层，并**把 FlashRT 作为依赖来引用** —— 它不从 FlashRT 里切代码出来。

可复用的核心（CapsuleStore、SessionRegistry、PrefixPlan、engine 协议）在 `capsule/core/` 里
**重新干净实现**，把 FlashRT 的 `serving/qwen36_agent` 当*参考*，而非搬运源。一些概念上的重复
（FlashRT demo 保留自己那个小 SessionRegistry；capsule 有生产级的那个）是有意为之：
**FlashRT 零改动，零循环依赖。**

为何更干净：FlashRT 始终是自洽的后端 + 社区 playground；`capsule` 拥有全部 serving 演进；两者永不缠绕。

---

## 1. 关键洞察 —— `capsule` 自身没有任何编译

这一点化解了大部分"前置工作很多"的担忧：

```
   生态 ──(protocol | library | embed)────────────────────┐
                                                           │
   capsule/  (纯 Python —— 不拥有 kernel, 不编译 C++)        │
     core/                ─ 逻辑依赖: exec C ABI ───────────┼──┐
     adapters/engines/<m> ─ 依赖: flash_rt frontend+kernels │  │ (选择性)
     adapters/transport/  ─ openai / grpc / ros2 / shm     │  │
   ────────────────────────────────────────────────────────┼──┼─ 运行时加载 (ctypes / import)
   FlashRT  (冻结库 —— 构建产物)                              │  │
     libflashrt_exec.so   ◀───────────────────────────────────┘  小、arch-light、零 csrc 依赖
     flash_rt[<model>]  frontends + csrc kernels  ◀──────────┘   选择性构建: arch × models
     serving/  (demo —— 保留, 不动)
```

因为 exec 层**已经**是 `csrc/` 的兄弟、对 kernel 零依赖（exec_contract §5），而 `capsule` 按红线
不拥有任何 kernel，所以 **`capsule` 就是一个 pip 可装的 Python 包，运行时加载 FlashRT 产物。**
那些重的 CUDA 编译*完全*是 FlashRT 的事，按 arch 选择性地、一次性完成。`capsule` 自己什么都不编译。

---

## 2. `capsule` 如何引用 FlashRT（打包 —— 已定）

**决策：依赖 `flash_rt` 包 + 选择性 extras。** v1 不拆独立 wheel。

```
pip install flash_rt[qwen36,pi05]      # 只装这些 frontend
# FlashRT 按目标 arch 构建一次:
#   cmake -B build -DGPU_ARCH=120 && cmake --build build -j --target <model targets>
pip install capsule                    # 纯 python; 依赖一个 pin 住的 flash_rt
```

- exec `.so` **打包在** `flash_rt` wheel **里面**；`capsule/core/` 通过 `flash_rt.runtime.exec`
  (ctypes) 拿到它。`core/` 的*逻辑*依赖只是 exec C ABI；*物理*包暂时是 `flash_rt`。
- `capsule` **pin** 一个已知的 `flash_rt` 版本/SHA 并对其测试（frontend API 是流动的；pin + 一致性
  测试是正常卫生）。

**engine 适配器 = 耦合的缓冲垫（shock-absorber）。** `capsule/adapters/engines/qwen36.py` 是*唯一*
触碰 FlashRT frontend 内部的地方（`snapshot_capsule`、`prefill_own_…`、graph/buffer 句柄）。
FlashRT frontend 变了，你只改那一个适配器 —— FlashRT 保持干净，`capsule/core/` 永远看不到 frontend
方法。**不稳定面被吸收在 `capsule` 里，而不是甩给 FlashRT。**

**延后（仅当真有需求）：** 拆一个极小的 **`flashrt-exec`** wheel（`libflashrt_exec.so` + `exec.h` +
ctypes 包装, 零 kernel），让*纯编排*节点或 CI 能在不编译任何 kernel 的情况下跑 `capsule` core。接缝
已经在了（exec 独立）；仅当真出现纯协调/边缘节点时再拆 —— 现在不做。

---

## 3. 硬件 / 编译 / 选择性 —— 三方归属切分

这干净地化解了"选择性编译, 不全量引用库"的担忧：

| 关注点 | 归属 | 机制 |
|---|---|---|
| **arch** (sm_120/121/89) | **FlashRT build** | `-DGPU_ARCH=` + per-model build targets（已有） |
| **要哪些模型** | **capsule 部署 manifest** | 声明式 `{models × arch × precision}` → 驱动 FlashRT 选择性构建 / 选预构建 wheel |
| **设备 / 放置**（哪块 GPU、端 vs 云） | **capsule placement** | `Node` = 绑定一个设备、托管若干 engine 的进程 |
| **兼容性**（加载的 `.so` 对不对？） | **capsule 加载时检查** | build-fingerprint 守卫（§5） |

所以：**arch → FlashRT；设备 → capsule placement；选择 → capsule manifest；兼容 → capsule 加载检查。**
`capsule` 什么都不编译；它只*选择*和*校验*。部署 manifest 是那个唯一的声明式旋钮：

```yaml
# capsule 部署 manifest（示例）
arch: sm_120
engines:
  - model: qwen36   precision: nvfp4
  - model: pi05     precision: fp8
# → 解析为最小的 flash_rt[...] 安装 + GPU_ARCH 构建, 仅此而已
```

---

## 4. FlashRT 要"规范化"什么 —— 只做打包卫生（已定：zero-touch）

需要的范围近乎为零；FlashRT 保持冻结。

**必需（打包卫生）：**
- `flash_rt` 能干净地作为依赖安装；exec `.so` 被打包且可经 `flash_rt.runtime.exec` 加载；
  **版本可查询**（让 `capsule` 在加载时能 pin + check）。
- 选择性构建文档化：`GPU_ARCH=<sm>` × 模型→target 映射表（大多已在 CMake 里 —— 只是写下来）。

**明确不做（延后/范围外）：**
- 可选的"servable frontend 描述符"（声明 capability/graph/buffer 的 manifest + 一致性测试）**延后。**
  没有它，`capsule` 的 per-model engine 适配器**手工**吸收所有 frontend 变化 —— 这让 FlashRT*完全*
  冻结, 契合不扩张的偏好。仅当手写适配器成为社区瓶颈时再回头看。
- FlashRT 里不做 serving/ 迁移、不重构、不加新抽象。

净结果：FlashRT 上唯一的"前置工作"就是确认它能作为干净依赖安装 —— 而这大部分已经成立。

---

## 5. capsule 兼容性守卫（选择性/多 arch 构建的安全网）

一个序列化的 Capsule（状态 blob）绑定到精确的 `{权重, 量化, kernel 版本, arch, graph 分桶}`。所以每个
Capsule 都**盖上一个 build fingerprint**（由 engine 适配器报告的身份 + exec ABI 版本 + arch 派生），
`restore` 在**不匹配时拒绝**。

这把论文诚实的边界（§8："capsule 是绑定到某部署的二进制 blob"）从一个坑变成强制不变量 —— 一旦构建按
arch 选择性进行、且 capsule 在节点间运输（云边端），这就是必需的。

---

## 6. `capsule` 如何承接生态（向上的表面）

`capsule` 是一个**双面适配器**：*向下*一个稳定的 engine 契约（接 FlashRT，经缓冲垫适配器），*向上*三个
集成层级（接生态）。合作方按耦合紧密度选层级：

```
  Level 1  PROTOCOL  ── 跟 capsule server 对话 (OpenAI/SSE · gRPC · ROS2 · shm-link)
           最松         生态里零 capsule 代码
           → agent 应用, omni, 远端机器人大脑

  Level 2  LIBRARY   ── import capsule, 组合 5 抽象 / 用一个 Mode
           中等         自定义 serving 逻辑, 进程内
           → 北大 (异步 + 多子图 + RL rollout + 确定性回放 + fork),
             机器人公司自定义编排

  Level 3  EMBED     ── link (未来的) C++ Runtime, 循环里无 Python
           最紧         完全嵌入式 on-robot / 边缘
           → lerobot 实时 actor, 端侧部署
```

映射真实合作方：

| 生态 | 层级 | capsule 提供 | 合作方带来 |
|---|---|---|---|
| **lerobot / 机器人公司部署** | 1 (ROS2/shm) 或 3 (embed) | episode-reset=restore · 多频率 planner→actor handoff · 中断 | 传感器 · 驱动 · 动作节拍 · 设备 |
| **北大（异步, 多子图, 训练）** | 2 (library) | `Schedule`（异步多子图）· `CapsuleStore`（fork + 确定性 RL 数据回放）· `Runtime`（异步） | 训练循环 · 数据管线 · 算法 |
| **agent 应用 / 工作站 / omni** | 1 (transport) 或 2 (embed) | 热启动 capsule · session/context · barge-in · 流式 | UX · 工具 · 产品逻辑 |

双向契约都小而稳：**向下**，每个模型家族一个 engine 适配器（唯一耦合 FlashRT 内部的地方）；**向上**，
Modes + transport 适配器 + 一个薄 SDK。`capsule` 的全部工作就是让两个契约都保持薄, 其余一切皆组合。

---

## 7. 有界的 v1 路径（前置工作, 已圈定范围）

1. `capsule` 仓库骨架：`core/`（5 抽象, 全新）+ `adapters/engines/qwen36.py`（第一个缓冲垫）+
   `pyproject.toml` 依赖一个 **pin 住的 `flash_rt`**。
2. FlashRT 打包卫生：确认 `flash_rt` 能作为依赖安装、exec `.so` 可加载、版本可查询。（大概已 ~90% 成立。）
3. `capsule` **部署 manifest** + **build-fingerprint 守卫**。
4. 跑通一个 mode（`agent`）E2E 经 `capsule → flash_rt`，**token 逐位一致**。然后加 `pi05` 适配器 +
   `rollout` / `handoff` mode。

没有 FlashRT 重构, 没有 `capsule` CUDA 编译, 没有全量库引用。选择性的故事正是：
**manifest → FlashRT 选择性构建 → capsule 加载 + 校验。**

---

## 8. 一句话总结

> `capsule` 是一个**纯 Python 的生态层**：pip 依赖一个 pin 住、按需构建的 `flash_rt`，通过一个薄的
> per-model 适配器（耦合缓冲垫）拿到它的 exec C ABI，给每个 Capsule 盖 build fingerprint 以安全
> restore，并以三种方式（protocol / library / embed）承接生态 —— 而 FlashRT 始终是一个冻结的后端
> 兼 demo playground。
