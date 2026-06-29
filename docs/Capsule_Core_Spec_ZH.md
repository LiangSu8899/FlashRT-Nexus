# Capsule —— 核心规范（主权的、零依赖的推理定义）

> 状态：权威规范 · v2 · 2026-06-29 · `capsule` 仓库的脊梁
> 配套：`Capsule_Serving_Design_ZH.md`（设计动机）、`Capsule_Serving_Integration_ZH.md`（FlashRT
> *backend* 打包）。本文在**核**这一层权威。协议的权威*形式*是头文件
> `capsule/core/include/capsule/capsule.h`；本文解释它。

---

## 0. 一个想法

> 造一个**主权的、零依赖的 C++ 核**,由它*定义*推理；其余一切——scheduler、协议、mode、transport、
> 甚至 GPU backend——都作为**可拔插的上层**,从外部*适配进*核。核执行,上层决策。核守住,框架活在它之上。
> 核小到能装进一个头文件,不依赖任何东西,永不认识场景,并在 v1 后**冻结**,让生态在坚实地基上生长。

---

## 1. 新推理定义

> **推理不是一个请求流过一个 scheduler。** 推理是:*在命名的状态 buffer 上 replay 一张捕获好的 graph；
> 状态是一等对象,可 snapshot / restore / 分支 / 搬运；一个命令式循环决定哪张 graph、在哪条 stream、何时发
> ——并能在 graph 边界中断。*

旧范式让**请求管线**居中,把状态藏进内存管理器。新范式让**状态**居中,管线退化成"发一次 replay"。这个反转,
就是贡献。

---

## 2. 分层 —— 核执行,上层决策

```
 L3  生态 / 应用      agents · lerobot · 北大 · omni · 机器人产品              自带依赖, 适配 IN
                            │  组合 / 说协议
 ───────────────────────────▼──────────────────────────────────────────────────────────────────────
 L2  框架（可拔插"前端" —— 解耦、可换；框架会写这些）
       schedulers:      robot-async · multi-model · multi-hardware    ◀── 生成/改写 Schedule,
       state-services:  CapsuleStore(注册/分层/LRU) · Session/journal      拥有决策循环
       adapters:        transport(openai/grpc/ros2/shm) · backends
       modes:           agent · rollout · handoff · duplex
 ───────────────────────────┬──────────────────────────────────────────────────────────────────────
 L1  capsule 核  (C++ · C ABI · 零依赖 · 主权 · v1 后冻结)              ◀── 推理定义
       Capsule (状态)         ·   命令式 Drive 动词（无 loop）
       Schedule (静态数据)    ·   Backend 接缝 (抽象)
 ───────────────────────────▲──────────────────────────────────────────────────────────────────────
 L0  BACKENDS         flashrt (第一个) · raw-cuda · cpu-edge · future            实现接缝
```

- **L1 不依赖任何东西。** 不依赖 L0/L2/Python。它定义一个 backend 接缝、提供命令式动词；**不拥有 loop、
  不拥有线程**。
- **L2 是框架价值所在**——完全可拔插。两类 L2 组件:**schedulers**(拥有决策循环)与 **state-services**
  (CapsuleStore 注册/分层/LRU、Session/journal —— 从核里降下来的那些)。框架会写一套；集成者可换、可自写。
  L1 永不 import L2。
- **L0 从下方插进来**,FlashRT 是第一个。**L3 从上方组合。** 所有箭头都指*向* L1。

---

## 3. 核心 C ABI（协议边界）

头文件为权威；这里是形状。~35 个函数;依赖 = `<stdint.h>`、`<stddef.h>`。跨边界的只有**不透明句柄**、
**POD 结构**、**字节缓冲**(serialize/load)、**backend vtable**。没有 C++ 类型、STL、异常跨越 C ABI。

### 3.1 Backend 接缝 —— 核唯一触碰的外部东西

backend 填一张 vtable；核来调。vtable 带**版本**(`abi_version` + `struct_size`),让核能校验/拒绝不匹配的
backend——稳定性铰链。FlashRT 的 `libflashrt_exec` 对它约 1:1 映射。

```c
typedef struct cap_backend_s {
    uint32_t abi_version;   /* = CAP_ABI_VERSION（稳定性闸门） */
    uint32_t struct_size;   /* = sizeof(cap_backend) */
    void*    self;          /* backend ctx, 传给每个 fn */

    /* buffer: 命名内存；状态住在这, 由 BACKEND 持有（dev 或 host） */
    cap_buffer (*buffer_alloc)(void* self, const char* name, size_t bytes, int space);
    cap_buffer (*buffer_wrap )(void* self, const char* name, void* ptr, size_t bytes, int space);
    void*      (*buffer_ptr  )(void* self, cap_buffer);
    size_t     (*buffer_bytes)(void* self, cap_buffer);
    int        (*buffer_copy )(void* self, cap_buffer dst, size_t doff,
                               cap_buffer src, size_t soff, size_t n, int stream);   /* D2D */
    int        (*buffer_upload  )(void* self, cap_buffer dst, size_t off, const void* src, size_t n, int stream); /* H2D */
    int        (*buffer_download)(void* self, cap_buffer src, size_t off, void* dst,       size_t n, int stream); /* D2H */
    void       (*buffer_free )(void* self, cap_buffer);
    /* graph: ShapeKey -> backend 捕获/adopt 的可 replay 变体 */
    int  (*graph_replay)(void* self, cap_graph, cap_shape_key, int stream);
    int  (*graph_has   )(void* self, cap_graph, cap_shape_key);
    int  (*graph_bind  )(void* self, cap_graph, const char* port, cap_buffer);  /* 仅 SETUP 时 */
    /* stream + event: 唯一的并发机制 */
    int       (*stream)(void* self, int priority);
    cap_event (*event )(void* self);
    int  (*event_record)(void* self, cap_event, int stream);
    int  (*event_query )(void* self, cap_event);   /* 0=ready >0=pending <0=err; 非阻塞 */
    int  (*stream_wait )(void* self, int stream, cap_event);
    int  (*sync        )(void* self, int stream);
    void (*event_free  )(void* self, cap_event);
    /* identity: capsule 指纹 = hash{权重,量化,kernel,arch} */
    uint64_t (*fingerprint)(void* self);
} cap_backend;
```

### 3.2 Capsule —— 真正新增的状态原语

核只拷贝*命名的 region*；它从不解释 KV / recurrent / conv 状态*是什么意思*。机制,非策略。所有状态操作都带
`stream`、都**异步**——完成用 `cap_capsule_ready` 轮询,状态永不卡住控制循环。

```c
typedef struct { cap_buffer buf; size_t off; size_t bytes; } cap_region;
typedef struct {
    const cap_region* regions; int n_regions;   /* 要冻结的命名状态 */
    const void* meta; size_t meta_len;            /* 小元数据（pos, digest, ...）—— 对核不透明 */
} cap_boundary;
typedef struct { void* ptr; size_t bytes; } cap_region_view;   /* 给零拷贝 transport */

cap_capsule cap_snapshot   (cap_ctx, const cap_boundary*, int tier, int stream);
int         cap_capsule_ready(cap_ctx, cap_capsule);                 /* 非阻塞 */
int         cap_restore    (cap_ctx, cap_capsule, int stream);       /* 拷回 ORIGIN 活 buffer */
int         cap_restore_into(cap_ctx, cap_capsule, const cap_region* dst, int n, int stream); /* 分支 / 接收 */
int         cap_regions    (cap_ctx, cap_capsule, cap_region_view* out, int* n); /* 零拷贝 transport 访问 */
int         cap_tier_move  (cap_ctx, cap_capsule, int to_tier, int stream);  /* GPU↔HOST↔DISK */
int         cap_serialize  (cap_ctx, cap_capsule, void* out, size_t* len);   /* out=NULL → 查大小 */
cap_capsule cap_load       (cap_ctx, const void* blob, size_t len);          /* 指纹校验 */
void        cap_capsule_drop(cap_ctx, cap_capsule);
```

- **`restore`** 把冻结 region 拷回*原始*活 buffer(同进程热启动/撤销)。**`restore_into`** 拷进*调用者给定*的
  活集合——这是 **fork**(分支到 N 个 engine 实例)与**接收运来的 capsule**的机制(`cap_load` 出来的 capsule
  无原始绑定,只能 `restore_into`)。
- **fork / time-travel 是 L2 语法糖**,建在 `snapshot` + `restore_into` 之上——不是核动词。

### 3.3 Schedule —— 一份静态描述（数据,不是 scheduler）

```c
typedef struct {
    cap_graph graph; cap_shape_key key;
    int stream; int priority;
    int cadence_num, cadence_den;   /* 每 cadence_den 个 tick 发 cadence_num 次；1/1 = 每 tick */
    int trigger;                    /* CAP_EVERY | CAP_ON_EVENT | CAP_ON_DEMAND */
} cap_stage;
typedef struct {
    const cap_stage* stages; int n_stages;
    const int (*deps)[2]; int n_deps;   /* {after, before} 跨 stage event 依赖 */
} cap_schedule;
```

### 3.4 Drive —— 命令式动词（loop 在 L2）

核提供动词；**它永不拥有 `while` 循环或线程。** loop 永远是 scheduler(L2)。`cap_fire`/`cap_swap`/`cap_sync`
是不可再约的、**零分配**热路径动词；`cap_drive_tick` 是*薄的可选 helper*(schedule × clock 的纯函数),
scheduler 可用可换。

```c
cap_ctx cap_ctx_create (const cap_backend*);   /* 校验 abi_version; 绑定一个 backend */
void    cap_ctx_destroy(cap_ctx);

int cap_fire      (cap_ctx, const cap_stage*);                       /* 一次 replay（零分配） */
int cap_drive_tick(cap_ctx, const cap_schedule*, uint64_t clock, int* failed_stage); /* helper; failed_stage 定位故障 */
int cap_swap      (cap_ctx, cap_buffer dst, const void* src, size_t n, int stream);  /* µs 改内容: subgoal/obs */
int cap_sync      (cap_ctx, int stream);
```

**Capsule + Drive 动词是相对 FlashRT exec 契约唯一真正新增的内容；** Schedule ≈ Plan + cadence,Backend
接缝 ≈ exec 原语,只是重述成由核自己拥有。

---

## 4. 核 vs scheduler —— 精确切分

| | 核 (L1) | scheduler (L2, 可拔插) |
|---|---|---|
| 角色 | **执行**动词 / 一份 Schedule 描述 | **生成/改写**描述；**拥有**循环 |
| 知道什么 | stream、event、replay、region | 节拍、优先级、放置、中断、背压 |
| 拥有 loop/线程? | **否** | **是——它*就是*循环** |
| 依赖什么 | 什么都不依赖 | 依赖核（调它的 C ABI） |

- **中断**不是核特性——它是 scheduler *选择不发下一个 tick*、转而发高优先 stage。核只保证 **graph 边界粒度**
  (一次 replay 原子；循环在 replay 之间重新决策)。mid-replay 中止天生不可能(保持 graph 短)。
- **条件子图路由**(B 仅当 A 输出满足条件)= scheduler `buffer_download` 几个字节读结果、`cap_fire` B 或 C。
  故意的 host 返回接缝,有已知延迟代价——不是核缺口。
- **外部/host 触发**(消息到、传感器就绪)= scheduler 命令式调 `cap_fire`。`CAP_ON_EVENT` 只指 GPU event
  (跨 stream);核不认识任何 host 信号。

**框架会写的三个 scheduler（全 L2、全可拔插）:** **robot-async**(动作节拍；vision/ASR 侧 stream；中断 =
`cap_swap` subgoal；reset = `cap_restore`)、**multi-model**(共托 N engine；节拍/并发/填谷-vs-p99)、
**multi-hardware**(放置；绑 backend；云边端经 Link 送 capsule)。各自核 C ABI 之上几百行,可独立测试、可替换,
且**对 L1 不可见**。

---

## 5. 稳定性与协议边界契约（约束）

核是别人依赖的基建,所以它的边界与保证是契约。

### 5.1 跨边界的只有这些 —— 别无其他
只有**不透明句柄**(`cap_ctx/cap_capsule/cap_buffer/cap_graph/cap_event`)、**POD 结构**
(`cap_region/cap_boundary/cap_stage/cap_schedule/cap_region_view`)、**字节缓冲**(serialize/load)、
**backend vtable**。绝无 C++ 对象、STL 类型、智能指针所有权、异常跨越。错误是 `int` 状态码
(`CAP_OK=0`,负值见 `enum cap_status`)。

### 5.2 核的保证（L2/L0 可依赖）
- **热路径动词零隐藏分配。** `cap_fire/cap_swap/cap_sync/cap_capsule_ready` 不分配。(`snapshot`/`tier_move`/
  `load` 会分配——它们是 setup/边界操作,不在控制循环。)
- **永不加锁。** 核内无 mutex。(见 5.4。)
- **指纹拒绝。** `restore`/`restore_into`/`load` 在 capsule 指纹 ≠ 绑定 backend 的 `fingerprint()` 时以
  `CAP_ERR_FINGERPRINT` 拒绝。restore 永不静默出错。
- **故障定位。** `cap_drive_tick` 报告出错的 stage 下标；backend 错误绝不污染核状态。

### 5.3 Backend 契约（backend 必须保证）
确定性 `graph_replay`；stream 有序的 `buffer_copy/upload/download`；**非阻塞** `event_query`；给定 build 下
**稳定**的 `fingerprint`；句柄在 free 前有效；**绝不跨 ABI 抛异常**(返回状态)。设置 `abi_version`/`struct_size`。

### 5.4 线程模型 —— 零锁,一 ctx 一线程
一个 `cap_ctx` 由**一条线程**驱动；核**不内部同步**(无热路径 mutex——这是零开销的代价)。多设备/多模型 =
多 `cap_ctx` 多线程。跨线程/跨设备/跨节点协调在 **L2**(ctx 内用 event；ctx/节点间用 transport)。这一条要遵守,
不能假装它线程安全。

### 5.5 运行期改动纪律
`cap_swap` 改 buffer **内容**(µs)。`graph_bind` **仅 setup 时**——绝不在运行时重绑 graph 的 buffer *指针*
(captured graph 已 baked 绝对指针,重绑=重捕获)。任何在中断节拍可变的东西(subgoal、位姿、模式 flag)都是被
`cap_swap` 覆写的绑定 buffer,绝不烤进 graph。

### 5.6 ABI 稳定性 —— 冻结,然后仅新增
v1 后 C ABI **冻结**。演进**仅新增**:追加新函数、追加新枚举值、`CAP_ABI_VERSION` 与 vtable `struct_size` 自增。
绝不重排/删除/改义。旧核通过版本/大小字段优雅拒绝更新更大的 vtable。这就是让生态无 churn 地依赖核的东西。

---

## 6. 我们不做什么（非目标 —— 负空间）

1. **核里不做 scheduler。** 它执行 Schedule 描述、不拥有 loop。scheduler 是可拔插 L2。
2. **不编译模型逻辑。** 没有 engine-as-artifact(反 TensorRT)。模型 = 捕获 graph(数据)+ 命名状态；核里零模型代码。
3. **不管理 GPU 内存或计算。** 没有 KV manager、paged/block 分配器、radix tree、内存池。状态 = backend 持有的命名
   buffer；核只持有句柄、拷贝 region。
4. **核不依赖任何东西。** 循环里无 Python、无框架、无第三方库。依赖由 L0/L2 在边缘承担。
5. **核不编码策略。** 没有协议、agent 循环、机器人节拍、cache 淘汰、batching、背压策略。只有机制,递归。
6. **不为抽象而抽象。** 没有深层级、plugin/DI 框架、配置驱动间接、反射。最少对象,每个具体,全可审计。
7. **不为数据中心吞吐/多租户优化。** 延迟优先、小 batch、高频、单/少 session、可上端侧。
8. **不假设部署形态。** 不以请求/响应、无状态 worker、大 GPU/云、单模型为中心。
9. **不重造执行/kernel 层。** 核里无 graph 捕获、kernel、CUDA context 所有权——那在 backend 接缝之后。我们定义
   *serving*,不是 *execution*。
10. **不让 capsule 跨部署可移植。** capsule 是盖指纹、绑定 {权重,量化,kernel,arch} 的二进制 blob。无可移植跨版本格式。
11. **热路径无锁、无隐藏分配、无 callback。** 用 poll(`event_query`),不用 callback；核绝不起线程。

仓库的形态*就是*这张清单的反面:一个极小的 C++ 核,"不做"清单上的一切要么在它之上、要么根本不存在。

---

## 7. 依赖姿态与语言

- **部署热循环是 C++。** 跑亚毫秒控制的路径里无 GIL/GC/runtime。Drive 动词、Capsule 操作、backend 接缝全是
  C/C++ over C ABI。
- **核不依赖任何东西**(依赖反转):它链接抽象 backend vtable,而非一个 backend。FlashRT `libflashrt_exec` 是*第一个*
  实现；raw-CUDA / CPU-edge 之后可实现同一接缝。这是"未来生态适配进来"的能力——长在形态里。
- **Python 只是边缘的一个可选绑定**——原型/setup/测试;绝不在部署循环。
- **FlashRT 保持冻结 backend**(`Capsule_Serving_Integration_ZH.md` 讲 FlashRT *backend 适配器* L0 打包)。

---

## 8. capsule 兼容性守卫

每个 capsule 带 backend 的 `fingerprint()`；`restore`/`restore_into`/`load` 不匹配则拒绝。这把论文诚实的边界
("capsule 是绑定部署的 blob")变成*强制不变量*——一旦按 arch 选择性构建、且 capsule 在节点间运输,这就是必需的。

---

## 9. 推进 —— 核优先

- **P0 —— 核 C ABI + 参考实现（~1 周）:** 头文件(§3) + Capsule + Drive 在 backend 接缝之上的参考实现,加一个
  **桩 backend(host memcpy)**。门禁:**核以零依赖完成构建与测试**,验证 snapshot/restore/restore_into/swap/fire/
  指纹拒绝。(就是现在起的本地 `capsule/` 仓库内容。)
- **P1 —— flashrt backend（~1 周）:** `backends/flashrt` 适配器,拿 `libflashrt_exec` 实现接缝。门禁:**经核驱动
  一个真实模型 E2E,token/cos 逐位一致。**
- **P2 —— 第一个 scheduler + mode（~1–2 周）:** `agent` mode + 一个 L2 scheduler;热启动 capsule restore。门禁:
  **token 逐位一致;核 C ABI 零新增字段。**
- **P3 —— robot-async + multi-model scheduler（~1–2 周）:** rollout/handoff 跑在同一核上。门禁:**episode-reset
  cosine 1.0;多频率 handoff 逐字节相等;核零新增字段。**
- **P4 —— multi-hardware + 云边端（later）:** 放置 + capsule 经 Link 送出。门禁:**node A 算出的 capsule 在
  node B restore,指纹校验,逐位一致。**

核 C ABI **在 P0 后冻结**;只让它的 backends/schedulers/modes 生长。

---

## 10. 一句话总结

> 一个主权的、零依赖的、v1 后冻结的 C++ 核（**Capsule + 命令式 Drive 动词 + Schedule-数据 + backend 接缝**）把
> 推理*定义*为**在可搬运状态上 replay、命令式驱动**。scheduler、state-services、协议、mode、GPU backend 全是
> **适配进核的可拔插上层**——框架会写一套,集成者自由替换,而核永不依赖、不为之加锁、不替它跑循环。
