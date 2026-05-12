# P12 Pair-Manager 代码阅读清单

这份文档只服务于一条主线：

- 硬件配置：`GemminiLearningConfigSpadReRoCCGlobalNoC4C2x2P12x4x3CoupledDMAPairManagerDummy16x16Sbus128`
- 软件/运行主线：`pairdummy / sbus128 / bertmini batch8 / file-only sync`
- 阅读目标：**把 “12 个 pair manager 的硬件 + 软件 + pipeline runtime” 一次读顺**

这版不是“只告诉你看什么”的清单，而是“先把逻辑讲清楚，再告诉你去哪里验证”的导读。
你可以：

- 先只读本文，建立完整心智模型；
- 再按本文给出的文件顺序去对照代码；
- 或者只在某个阶段卡住时，把本文当作“解释词典”回查。

---

## 0. 先记住的 12 个结论

如果你现在只能记住最少的信息，请先记住下面 12 条。后面整份文档，其实都在展开这 12 条。

1. `P12` 表示 **12 个 pair manager**，不是 `12 个 Gemmini manager + 12 个 DMA manager`。
2. 每个 pair manager 背后都包着 **一对硬件**：`Gemmini + CoupledDMA`。
3. 这对硬件对外只暴露成 **一个 ReRoCC manager 编号**。
4. 软件看到的是 **同一个 pair id 空间**；Gemmini 和 DMA 的区别主要靠 **opcode**，不是靠“另一套 manager id”。
5. 在这条路线里，`custom3` 走 Gemmini / SPM xlate，`custom2` 走 CoupledDMA。
6. `GemminiCoupledDMAPairWrapper.scala` 的本质，就是“**一个入口，内部按 opcode 分流到 Gemmini 或 DMA**”。
7. `ReRoCC manager` 的本质，是“**先 acquire 某个 manager，再把某个 opcode 绑定到它，然后发 RoCC 指令**”。
8. `preserveIncomingOpcode = true` 是 pair 路线成立的关键；否则 manager 会把 `custom2/custom3` 改写掉，wrapper 就分不清 DMA 和 Gemmini。
9. `buildPairLayout()` 决定了 P12 机器在 NoC 上怎么摆；`useCompactPairManagerLayout = true` 让 12 个 pair manager 被紧凑地摆成 `4 x 3`。
10. `shared scratchpad + spm xlate` 的含义是：软件可以先建立一段 alias 虚拟地址窗口，再让 Gemmini / DMA 通过页表把它翻译到共享 SPM 页。
11. pipeline runtime 在 `main.c` 和 `prt_types.h` 里，把历史上“两套编号空间”的软件参数，折叠成“**同一 pair id 空间**”。
12. pipeline runtime 真正运行时，核心流程是：**读 artifact → 分配 pair manager → 分配 SPM 页 → 安装 xlate → 建 stage 拓扑 → worker 发 DMA/Gemmini 指令**。

---

## 1. 第一阶段：先把“这台 P12 机器到底是什么”读明白

这一阶段的目标，不是先看 runtime，而是先回答：

> 这条配置到底往 SoC 里塞了什么？
> 它为什么叫 `P12 pair-manager`？
> 它和 separate 路线的根本不同点是什么？

### 1.1 这一阶段你应该先得到的结论

先不要进代码。先把下面几句话读顺：

- 这条配置做的事，不是“放一堆 Gemmini，再放一堆 DMA”，而是“放 12 个复合加速器节点”。
- 每个复合节点都同时拥有：
  - Gemmini 计算能力；
  - CoupledDMA 搬运能力；
  - shared scratchpad / spm xlate 能力。
- 这 12 个复合节点再通过 ReRoCC manager 平面和 ReRoCC NoC 暴露给软件。
- 软件 acquire 的，不是“Gemmini 3 号”或“DMA 3 号”，而是“**pair manager 3 号**”。
- 之后软件如果发 `custom3`，这个 pair manager 里的 **Gemmini** 接；如果发 `custom2`，这个 pair manager 里的 **DMA** 接。

这就是整条路线最核心的抽象。

### 1.2 必读文件

- `generators/chipyard/src/main/scala/config/GemminiLearningReRoCCPairManagerConfigs.scala`
- `generators/chipyard/src/main/scala/config/GemminiLearningReRoCCCoupledDMAConfigHelpers.scala`
- `generators/chipyard/src/main/scala/config/fragments/ReRoCCGemminiCoupledDMAPairFragments.scala`
- `generators/gemmini/src/main/scala/gemmini/GemminiCoupledDMAPairWrapper.scala`
- `generators/rerocc/src/main/scala/manager/Parameters.scala`
- `generators/rerocc/src/main/scala/manager/Manager.scala`

### 1.3 从配置类开始：这台机器是怎么“拼”出来的

先读 `GemminiLearningReRoCCPairManagerConfigs.scala`，只抓一条主线：
`GemminiLearningConfigSpadReRoCCNoCPairManagerParametric` 如何把一个 P12 系统组装出来。

#### 你要先直接读出来的参数含义

目标配置
`GemminiLearningConfigSpadReRoCCGlobalNoC4C2x2P12x4x3CoupledDMAPairManagerDummy16x16Sbus128`
的关键参数是：

| 参数 | 含义 | 当前值 |
| --- | --- | --- |
| `numCores` | CPU 核数 | `4` |
| `cpuX`, `cpuY` | CPU 在 NoC 上的二维排布 | `2 x 2` |
| `numPairs` | pair manager 个数 | `12` |
| `pairX`, `pairY` | pair manager 逻辑排布目标 | `4 x 3` |
| `sbusWidthBits` | system bus 宽度 | `128` |
| `nMemoryChannels` | 内存通道数 | `2` |
| `meshRows`, `meshColumns` | Gemmini 阵列大小 | `16 x 16` |
| `useGlobalNoC` | 是否启用 global NoC | `true` |
| `useDeterministicGlobalNoCRouting` | 是否使用确定性全局路由 | `true` |
| `useDummyGemmini` | 是否使用 dummy Gemmini 配置 | `true` |
| `sharedSpadBytes` | shared scratchpad 容量 | `1 MiB` |
| `useCompactPairManagerLayout` | 是否使用紧凑 pair 布局 | `true` |
| `pairTlMaxInFlight` | TL in-flight 上限 | `64` |
| `pairAtlMaxInFlight` | ATL in-flight 上限 | `64` |

#### 这个配置类实际做了 6 件事

1. **检查参数合法性**
   - `numPairs <= pairX * pairY`
   - `sbusWidthBits` 必须是正数、8 的倍数
   - `sbusWidthBits / 8` 必须是 2 的幂，因为 shared scratchpad 的 beat bytes 需要这样组织

2. **根据 `sbusWidthBits` 推导 Gemmini/Shared-SPM 的 beat 大小**
   - `gemminiBeatBytes = sbusWidthBits / 8`
   - 在这条配置里，`128 / 8 = 16B`

3. **调用 `buildPairLayout()` 生成 NoC 布局**
   - 这是 pair 路线最关键的版图函数
   - 它决定 CPU、pair manager、system/pbus/serial_tl 分别落在哪些 NoC 节点

4. **构造 shared scratchpad 配置**
   - `enable = true`
   - `global_base_addr = 0x40000000`
   - `use_page_table_xlate = true`
   - `share_xlate_with_coupled_dma = true`

5. **生成 12 个 pair wrapper**
   - 通过 `WithReRoCCGemminiCoupledDMAPairManagers(...)`
   - 每个 wrapper 都拿到一个 `pairId`

6. **把这些东西挂到 SoC**
   - `WithReRoCCNoC(...)`
   - `WithReRoCC(...)`
   - `pairManagerConfig`
   - `WithNBigCores(4)`
   - `WithSystemBusWidth(128)`

你读这个文件时，最重要的不是记住每个 mixin，而是看清楚这条因果链：

> 参数 → NoC 布局 → shared scratchpad → 12 个 pair wrapper → ReRoCC manager/NoC → SoC

### 1.4 为什么 `buildPairLayout()` 比别的 helper 更值得先看

`GemminiLearningReRoCCCoupledDMAConfigHelpers.scala` 里同时有 `buildLayout()` 和 `buildPairLayout()`。

- `buildLayout()` 更偏向 old separate 路线
- `buildPairLayout()` 才是这条 P12 主线真正的布局函数

你应该先把 `buildPairLayout()` 理解成一句话：

> **给 CPU、12 个 pair manager、system endpoints、pbus、serial_tl 分配 NoC 坐标。**

#### `buildPairLayout()` 的默认逻辑

默认布局里：

- `defaultNocCols = max(cpuX, pairX, infraEndpointCount)`
- CPU 从第 0 行开始排
- pair manager 从 `cpuY` 这一行开始排
- infra 节点排在 pair rows 的后面

对当前配置：

- `cpuX = 2`
- `pairX = 4`
- `infraEndpointCount = nMemoryChannels + extraInfraEndpoints = 2 + 2 = 4`

所以：

- `defaultNocCols = 4`

如果用默认布局，系统会倾向于：

- 前 2 行给 CPU
- 接下来 3 行给 12 个 pair manager
- 再额外留 infra 行

#### `useCompactPairManagerLayout = true` 到底改了什么

这条配置打开了 `useCompactPairManagerLayout = true`。
它不是改变“谁有哪些节点”，而是改变“**这些节点怎么挤得更紧**”。

紧凑布局的核心策略是：

1. 仍然保持列数 `compactNocCols = defaultNocCols`
2. CPU 仍放在最上面
3. 12 个 pair manager 从 `cpuY` 行开始，按 row-major 紧凑铺开
4. **优先把 infra 节点塞进 CPU 顶部区域还没用掉的空位**
5. 只有顶上空位不够时，才把 infra 继续追加到 pair rows 后面

#### 对这条 P12 配置，最后会形成一个非常直观的布局

因为：

- CPU 是 `2 x 2`，但 mesh 列数是 `4`
- 所以前两行会留下 4 个空槽
- infra 也正好需要 4 个端点（2 个 memory channel + pbus + serial_tl）

所以最终会形成这样的直观布局：

```text
row 0: Core0  Core1  system0  system1
row 1: Core2  Core3  pbus     serial_tl
row 2: Pair0  Pair1  Pair2    Pair3
row 3: Pair4  Pair5  Pair6    Pair7
row 4: Pair8  Pair9  Pair10   Pair11
```

这就是你理解 `useCompactPairManagerLayout = true` 的最好方式：

- **不是让 pair manager 数量变少**
- **不是让 manager 合并**
- 而是让 NoC 布局更紧凑，避免把 infra 单独再拖出一整排**

### 1.5 `WithReRoCCGemminiCoupledDMAPairManagers`：12 个 wrapper 是怎么批量生成的

读 `ReRoCCGemminiCoupledDMAPairFragments.scala` 时，只抓一句话：

> 这个 fragment 把 `numPairs` 个 `GemminiCoupledDMAPairWrapper` 追加进 `BuildRoCC`。

关键点：

- `Seq.tabulate(numPairs) { i => ... }`
- 每个 wrapper 都拿到 `gemminiIdBase + i`

在当前配置里：

- `gemminiIdBase = 0`
- 所以软件最终看到的 pair manager 编号就是 `0..11`

你可以把这个文件理解为“批量实例化工厂”：

- 前一个配置类说“我要 12 个 pair”
- 这个 fragment 真正把 12 个 wrapper 一个个建出来

### 1.6 `GemminiCoupledDMAPairWrapper.scala`：这份文件是整条 pair 路线的核心

如果你今天只精读一个硬件文件，就应该精读它。

它的本质可以先概括成一句话：

> **把 Gemmini 和 CoupledDMA 打包成一个对外只暴露一个 RoCC 入口的复合加速器。**

#### 先别怕 `tilelink node`，你可以先这样理解

你现在不需要先懂 Diplomacy 的所有历史，只要先把下面的比喻记住：

- `tlNode / atlNode / stlNode`：可以先把它们看成“这个模块对外暴露的几组 TileLink 插座”
- `TLXbar`：可以先把它看成“把多个子模块的总线口合并到一起的交换器”
- `TLBuffer`：可以先把它看成“在总线上加一个缓冲层”
- `TLSourceShrinker`：可以先把它看成“限制 in-flight source ID 数量的节流器”
- `TLIdentityNode()`：可以先把它看成“先占位，表示这里有一个连接口”

对初学者来说，这样理解已经足够读顺这份文件。

#### 这个 wrapper 对外宣称自己支持两个 opcode

它继承的是一个 `LazyRoCC`，而且：

- `opcodes = OpcodeSet.custom2 | OpcodeSet.custom3`

这句话非常重要。它的含义是：

- 这个 wrapper 不是“只接一种指令”
- 它明确告诉系统：“`custom2` 和 `custom3` 都可以送到我这里”

#### 它内部实际包了两个子模块

- 一个 `Gemmini(...)`
- 一个 `GemminiCoupledDMA(...)`

而且二者共用同一个 `pairId`：

- `gemminiConfig.gemmini_id = pairId`
- `dmaParams.gemmini_id = pairId`

也就是说，从“对外编号”的角度，这一对硬件天然就是一组。

#### 最关键的内部逻辑：按 opcode 分流

在 `module` 里：

- `custom3` 被识别成 Gemmini 命令
- `custom2` 被识别成 DMA 命令

然后：

- Gemmini 只在 `isGemminiCmd` 时接 `io.cmd`
- DMA 只在 `isDmaCmd` 时接 `io.cmd`

所以 pair wrapper 的本质不是“同时广播一条命令给两个子模块”，而是：

> **先看 opcode，再把命令只送给真正该接收的那个子模块。**

这就是为什么 pair manager 模式的软件心智模型应该是：

- “同一个 manager id”
- “不同 opcode 选不同子模块”

#### 为什么它要合并一堆 TL / PTW / DCache 接口

因为它对外表现成一个加速器，但内部有两个会访问内存、会做地址翻译、会访问 cache 的模块。

所以 wrapper 还要做这些“合并工作”：

- 用 `tlXbar` 合并 Gemmini 和 DMA 的 `tlNode`
- 用 `atlXbar` 合并 Gemmini 和 DMA 的 `atlNode`
- 用 `stlXbar` 把系统来的 `stlNode` 分给 Gemmini 和 DMA
- 用 `HellaCacheArbiter(2)` 把两个子模块的 DCache 请求合并
- 把 PTW 端口按数量切开，前半给 Gemmini，后半给 DMA
- 响应用 `RRArbiter` 汇总回来

所以这份文件的真正角色不是“控制逻辑很复杂”，而是：

> **它是“外面看起来像一个 RoCC，加速器内部其实包了两个子模块”的总装/汇流器。**

#### 读这份文件时，你应该在脑中画出这张图

```text
          incoming RoCC cmd
                 |
            看 opcode
           /         \
    custom3           custom2
      |                  |
   Gemmini           CoupledDMA
      |                  |
      +---- TL / ATL / STL / PTW / DCache ----+
                         |
                 pair wrapper 对外接口
```

如果这张图你已经能在脑中复述出来，说明你已经抓住这份文件最重要的 80%。

### 1.7 ReRoCC manager 为什么必须保留 incoming opcode

读 `rerocc/manager/Parameters.scala` 和 `rerocc/manager/Manager.scala` 时，只看一件事：

- `preserveIncomingOpcode`

#### 先理解 ReRoCC manager 在干什么

你可以先把 ReRoCC manager 理解成：

> **位于“软件发出的 ReRoCC 请求”和“真正的 RoCC 加速器”之间的一个远程控制前端。**

它做的事包括：

- 接收 acquire / release / inst / ptbr / status 等协议消息
- 把 `mInst` 拼成真正的 `RoCCCommand`
- 再送给后面的 RoCC 模块

#### 默认情况下，manager 可能会把 opcode 改写成默认值

在 `Manager.scala` 里，如果：

- `!reRoCCTileParams.preserveIncomingOpcode`

那么 manager 在把命令塞进内部队列时，会把 `inst.opcode` 改成 `defaultOpcode`。

这在单一 RoCC 模块场景里通常不是问题，因为：

- 后面的模块本来只支持一种 opcode

#### 但 pair wrapper 恰恰依赖“opcode 不能丢”

因为 pair wrapper 内部需要靠 opcode 决定：

- `custom3` → Gemmini
- `custom2` → DMA

如果 manager 把 opcode 改写成同一个默认值，那么：

- wrapper 再也分不清这条命令是给 Gemmini 还是给 DMA
- 整个 pair 路线就失效了

所以这条配置里：

- `WithReRoCC(... preserveIncomingOpcode = true)`

不是一个“调试选项”，而是 **pair 设计成立的必要条件**。

### 1.8 第一阶段读完后，必须能回答的题

- [ ] 为什么 `P12` 不是 `12G + 12D`，而是 `12 个 pair manager`
- [ ] 为什么一个 pair manager 可以同时接 Gemmini 和 DMA 指令
- [ ] 为什么 `custom2/custom3` 是 pair 路线的软件可见分流键
- [ ] 为什么 `preserveIncomingOpcode = true` 是 pair 路线的硬约束
- [ ] 为什么 `buildPairLayout()` 比 `buildLayout()` 更值得先看
- [ ] 为什么 `useCompactPairManagerLayout = true` 会让当前 NoC 摆成“上面 CPU+infra，下面 12 pair”的样子

如果这些题还答不顺，不建议先冲 runtime。

---

## 2. 第二阶段：把“软件眼里的 pair manager”读明白

这一阶段的目标是回答：

> 软件到底怎么使用这台硬件？
> 软件眼里的 “manager id / opcode / acquire / release” 分别是什么？

### 2.1 先给出最重要的软件心智模型

对软件来说，P12 pair-manager 路线可以先压缩成一句话：

> **先 acquire 某个 pair manager id，再用 `custom2` 或 `custom3` 去驱动这个 id 背后的 DMA 或 Gemmini/SPM xlate。**

更具体地说：

- `manager id = 7`
  - 发 `custom2`：你在用 **pair 7 里的 DMA**
  - 发 `custom3`：你在用 **pair 7 里的 Gemmini 或 SPM xlate**

所以这不是：

- “Gemmini manager 7”和“DMA manager 7”是两个独立东西

而是：

- “**pair manager 7 这个入口**，根据 opcode 把命令送给不同子模块”

### 2.2 必读文件

- `generators/gemmini/software/gemmini-rocc-tests/include/rerocc_coupleddma.h`
- `generators/gemmini/software/gemmini-rocc-tests/include/rerocc_gemmini_spm_xlate.h`
- `generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests/rerocc_control.h`
- `generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests/run_rerocc_lc_linux.sh`

并行参考下面几份更聚焦的接口文档：

- `docs/software_rerocc_api_guide.md`
- `docs/software_spm_xlate_api_guide.md`
- `docs/software_coupleddma_api_guide.md`
- `docs/software_gemmini_api_pipeline_runtime_guide.md`

### 2.3 先理解 ReRoCC 软件接口在做什么

`rerocc_control.h` 这类接口，本质不是 Gemmini 或 DMA 功能本身，而是“**先取得 manager 的使用权**”。

你可以先把 ReRoCC 的软件用法想成三步：

1. **acquire**
   - 我要占用某个 manager
2. **bind opcode / 发指令**
   - 现在我要让这个 manager 处理某类 opcode，并送具体命令进去
3. **fence / release**
   - 用完后保证前面的命令都完成，再把 manager 让出来

pair 路线下，真正变化的是第 2 步的理解：

- opcode 不只是“指令编码”
- 它还是“进入 pair wrapper 后，走 Gemmini 还是 DMA 的选择键”

### 2.4 `rerocc_coupleddma.h`：软件如何直接编程 DMA

这份头文件的重点，不是让你背所有宏，而是先记住 DMA 编程的基本模式：

- 配源地址 `src`
- 配目标地址 `dst`
- 发起传输
- 等待完成
- 读 monitor / 状态

在 pair 路线里，它们走的是：

- `custom2`

所以当软件传入某个 manager id 时，含义不是：

- “去找一颗独立 DMA”

而是：

- “去找这个 pair manager，并把命令按 `custom2` 送进它内部的 DMA”

### 2.5 `rerocc_gemmini_spm_xlate.h`：软件如何编程 shared-spad xlate

这份头文件重点要理解四类动作：

- `cfg`
- `range`
- `flush`
- `fault`

它们走的是：

- `custom3`

也就是说，SPM xlate 在 pair 路线里并不是走 DMA 那条通路，而是和 Gemmini 共用“Gemmini 这侧”的 opcode 入口。

所以对软件来说：

- 同一个 pair manager id
- 发 `custom3`
- 有时是在调 Gemmini 计算
- 有时是在调 Gemmini 侧的 xlate 相关控制

### 2.6 为什么软件参数里还有 `NUM_GEMMINI` / `NUM_DMA`

这是读软件最容易被坑的点。

在 workflow、脚本、CLI 参数里，你仍然会看到：

- `NUM_GEMMINI`
- `NUM_DMA`
- `GEMMINI_BASE_ID`
- `DMA_BASE_ID`

这是 **历史命名遗留**，不等于当前硬件仍然是 separate 路线。

对 pair 路线，正确理解是：

- 这些参数的名字保留了历史外形
- 但 runtime 会在内部把它们折叠成同一 pair id 空间

也就是说，名字还叫：

- Gemmini count / DMA count

但真正生效的语义是：

- “pair manager 总共有多少个”
- “Gemmini 和 DMA 在 pair 模式下如何映射到同一组 manager id”

### 2.7 第二阶段读完后，必须能回答的题

- [ ] 软件 acquire/release 的对象到底是什么
- [ ] 为什么 pair 路线下，manager id 比“设备类型”更基础
- [ ] 为什么 `custom2` 和 `custom3` 对软件是重要分界线
- [ ] 为什么 `NUM_GEMMINI/NUM_DMA` 在 pair 路线下不能按字面硬解释成两套独立 manager

---

## 3. 第三阶段：把 pairdummy / sbus128 这条固定工程主线读明白

这一阶段回答的问题是：

> 这条 P12 路线在工程上到底是怎么被固定下来的？
> 哪个文件是“语义合同”，哪个文件是“执行脚本”，哪个文件把环境真正灌进 guest？

### 3.1 必读文件

- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/scripts/pairdummy_sbus128_fixed_env.sh`
- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/scripts/pairdummy_sbus128_workflow.sh`
- `generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests-coupleddma/workload/host-init-fileonly-sync-pairdummy.sh`
- `generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests-coupleddma/workload/host-init.sh`
- `generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests-coupleddma/workload/rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-batch8-fileonly-sync-pairdummy.json`
- `sims/firesim/deploy/config_runtime_f2_gemmini_rerocc_pairmanager_dummy16x16_4c12p12_sbus128_linux_bertmini_pipeline_runtime_batch8_fileonly_sync.yaml`
- `sims/firesim/deploy/config_hwdb_f2_gemmini_rerocc_pairmanager_dummy16x16_4c12p12_sbus128_bertmini.yaml`

### 3.2 `pairdummy_sbus128_fixed_env.sh`：这条主线的“语义合同”

这份文件最重要的角色不是“设置一堆环境变量”，而是：

> **把这条路线允许什么、不允许什么、默认怎么跑，固定成一个 profile。**

你至少要先抓住这些变量：

- `PIPELINE_RUNTIME_PROFILE_ID="pairdummy-sbus128-fixed-v24"`
- `TARGET_KEY=...sbus128`
- `NUM_CORES="4"`
- `NUM_GEMMINI="12"`
- `NUM_DMA="12"`
- `GEMMINI_BASE_ID="0"`
- `DMA_BASE_ID="0"`
- `PAIR_MANAGER_MODE="1"`
- `DUMMY_GEMMINI_MODE="1"`
- `PIPELINE_RUNTIME_MLOCKALL_MODE="2"`

#### 这里最关键的一条不是 `NUM_DMA=12`，而是 `PAIR_MANAGER_MODE=1`

为什么？

因为它告诉整个 runtime：

- 不要再按 separate 模式去理解 DMA manager 空间
- 应该把 DMA 理解成和 Gemmini 共用同一组 pair manager id

#### `TARGET_KEY` 里还有 `g12_d12`，为什么不能误读

这只是 artifact / 命名上的历史遗留。

你在这条主线里应该这样理解它：

- 这是名字
- 不是当前硬件拓扑事实

当前硬件事实应该回到第一阶段的结论：

- 不是 `12 Gemmini + 12 DMA`
- 而是 `12 pair manager`

### 3.3 `pairdummy_sbus128_workflow.sh`：这条主线的“执行外壳”

这份脚本的作用，不是定义语义，而是把“固定 profile + 固定文件路径 + 固定 FireSim 命令”打包成一套稳定工作流。

你读这份脚本时，要重点抓 4 件事：

1. 它一上来就 `source pairdummy_sbus128_fixed_env.sh`
2. 它固定了 workload json / runtime yaml / hwdb yaml
3. 它把 guest entrypoint、freshness 检查、run farm 生命周期都串起来了
4. 它提供 `show / debug-preflight / image-closure / launch / infrasetup / run / terminate` 这些固定命令

所以这份脚本是：

- **“怎么执行” 的外壳**

而 `pairdummy_sbus128_fixed_env.sh` 是：

- **“这次执行到底代表什么语义” 的内核**

### 3.4 `host-init-fileonly-sync-pairdummy.sh`：把固定 profile 灌进 workload 的最薄一层

这份脚本很薄，但非常关键。

它做的事可以压缩成两句：

- 先 `source` 固定 profile
- 再 `exec host-init.sh`

也就是说，这个文件的角色就是：

> **把 “我这次要跑的就是 pairdummy/sbus128 这条固定主线” 这件事，明确注入 host-init。**

### 3.5 `host-init.sh`：真正做 staging 的地方

你可以把 `host-init.sh` 看成：

> **把 host 侧环境、runtime 二进制、overlay 文件、guest 环境文件，真正打包进 FireMarshal 镜像的总装脚本。**

它里面最值得关注的是：

- 生成 guest 环境脚本
- 编译/准备 runtime 二进制
- 准备 overlay
- 校验 runtime artifact 是否齐全
- 把最终环境变量写进 guest

如果你想知道“为什么 guest 里最后真的跑成了 pairdummy/sbus128”，答案不在 workflow 外壳，而在这里。

### 3.6 第三阶段读完后，必须能回答的题

- [ ] 哪个文件是这条路线的固定 profile
- [ ] 哪个文件是执行 workflow 的命令外壳
- [ ] 哪个文件负责把固定 profile 真正灌进 guest
- [ ] 为什么 `PAIR_MANAGER_MODE=1` 比 `NUM_DMA=12` 更重要

---

## 4. 第四阶段：先把 runtime 的“控制平面”读明白

这一阶段要回答的问题是：

> runtime 在哪一层进入 pair-manager 模式？
> 它在哪一层把 Gemmini/DMA 的历史双空间，折叠成 pair-manager 单空间？
> 它在哪一层完成 manager 分配、SPM 分配和拓扑绑定？

### 4.1 必读文件

- `generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests/rerocc_pipeline_runtime_linux.c`
- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/main.c`
- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/include/prt_types.h`
- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/include/prt_schedule_action.h`
- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_schedule_action.c`
- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_runtime.c`

### 4.2 `rerocc_pipeline_runtime_linux.c`：只是入口转发

这份文件非常薄：

- Linux 可执行入口最后进入 `prt_main_entry(argc, argv)`

它的意义只是：

- 告诉你 pipeline runtime 真正的主入口在 `main.c`

### 4.3 `main.c`：runtime 从这里“宣布自己进入 pair 模式”

这份文件里最关键的不是 CLI 细节，而是配置语义。

#### 先看默认配置

它先给 `cfg` 填默认值，例如：

- `cfg.num_cores = 4`
- `cfg.num_gemmini_mgrs = 4`
- `cfg.num_dma_mgrs = 4`
- `cfg.gemmini_mgr_base_id = 0`
- `cfg.dma_mgr_base_id = 0`
- `cfg.pair_manager_mode = 0`

然后通过 CLI 覆盖这些值。

#### 真正关键的逻辑：pair 模式下回收 `dma_mgr_base_id`

在参数解析之后，如果用户没有显式指定 `--dma-base-id`，就会执行：

- pair 模式：`cfg.dma_mgr_base_id = cfg.gemmini_mgr_base_id`
- separate 模式：`cfg.dma_mgr_base_id = cfg.gemmini_mgr_base_id + cfg.num_gemmini_mgrs`

这段逻辑非常关键，因为它说明：

> **runtime 在主入口层就已经明确：pair 模式下 DMA 不再使用独立编号区间，而是回收到 Gemmini 同一基址。**

你可以把 `main.c` 对 pair 路线的贡献概括成一句话：

> **它先把“历史上长得像两套 manager 参数”的 CLI，解释成“pair 模式下共享同一组 base id”的内部配置。**

### 4.4 `prt_types.h`：pair 语义真正被固定成“公共合同”的地方

如果说 `main.c` 是入口，那么 `prt_types.h` 就是：

> **整个 runtime 内部都要遵守的 pair-aware 语义合同层。**

最重要的 helper 有 4 个：

- `prt_cfg_pair_manager_mode_enabled(cfg)`
- `prt_cfg_gemmini_mgr_count(cfg)`
- `prt_cfg_dma_mgr_count(cfg)`
- `prt_cfg_dma_manager_id(cfg, local_idx)`

#### 这几个 helper 实际上在说什么

1. `prt_cfg_pair_manager_mode_enabled()`
   - 判定当前是不是 pair 模式

2. `prt_cfg_gemmini_mgr_count()`
   - Gemmini manager 数量，直接取 `num_gemmini_mgrs`

3. `prt_cfg_dma_mgr_count()`
   - 在 pair 模式下，**DMA manager 数量直接返回 `num_gemmini_mgrs`**
   - 这等价于说：DMA 也共享同一组 pair manager 编号空间

4. `prt_cfg_dma_manager_id()`
   - 在 pair 模式下，**DMA manager id = `gemmini_mgr_base_id + local_idx`**
   - 不再走独立的 `dma_mgr_base_id + local_idx`

这其实就是整个软件视角的 pair 语义核心：

> **pair 模式下，DMA 的“数量”和“编号映射”都向 Gemmini 靠齐。**

### 4.5 `prt_schedule_action.c`：控制平面最重要的文件

这份文件是 runtime 的控制中枢之一。
如果你只想知道“runtime 如何把 segment 变成一组可执行 stage”，这份文件要反复看。

#### 先理解 `action` 是什么

在 runtime 里，一个 `action` 可以先粗略理解成：

> **“当前 segment 的执行计划书”**

这本计划书至少包括三类东西：

- 这段 segment 要用哪些加速器
- 这些 stage 的 SPM 页怎么分
- 真正运行时的 buffer / ring / alias / xlate 拓扑怎么绑定

#### 4.5.1 `assign_stage_manager_slot()`：pair 语义落地的第一现场

这段 helper 非常值得读，因为它把 pair 模式的“同一 id 空间”写得非常直接。

它做的事情是：

- 给某个 stage 的某个 slot 选一个 `gm_local`
- 然后先设 `dm_local = gm_local`
- 再通过
  - `prt_cfg_gemmini_manager_id(...)`
  - `prt_cfg_dma_manager_id(...)`
 变成真正的全局 id

于是 pair 模式下就出现了最关键的结果：

- `gm_local` 和 `dm_local` 是同一个局部下标
- `gm_id` 和 `dm_id` 也会映射到同一组 base id 空间

所以 pair 语义不是“后面某个角落顺手兼容一下”，而是：

> **从 manager 分配的 helper 开始，就已经被当成第一性原则。**

#### 4.5.2 `prt_action_alloc_acc()`：给每个 stage 分配 pair manager

这一步的核心任务是：

- 根据 segment 里每个 stage 的 `acc_util`
- 给它分配 `gemmini_mgr_ids[]`
- 也同时分配 `dma_mgr_ids[]`

你应该把这一步理解成：

> **先把“每个 stage 想用几块 accelerator”翻译成“它实际绑定哪几个 pair manager id”。**

它做的关键检查包括：

- `acc_util` 不能为 0
- `acc_util` 不能超过可用 Gemmini 数量
- `virtual_acc_ids` 的数量必须和 `acc_util` 对得上
- separate 模式下会额外要求 `dma_mgr_count >= gemmini_mgr_count`
- 但 pair 模式下，这个限制会自然折叠掉

最后的结果是：

- 每个 stage 都有一份 `gemmini_mgr_ids`
- 也有一份 `dma_mgr_ids`
- 在 pair 模式下，两者指向同一组 pair manager id 空间

#### 4.5.3 `prt_action_alloc_spm()`：给当前 segment 建一套 action-local SPM 视图

这一步要回答的问题是：

> 这段 segment 需要多少 SPM 页？
> 哪些页给 weight？哪些给 pipe buffer？哪些给 ring？
> alias window 和 page-table xlate 怎么建？

它可以分成 4 个层次理解：

1. **先算当前 segment 总共需要多少页**
   - `segment_spm_page_span`
   - 或累加各 stage 的 `local_spm_page_span`

2. **如果启用了 xlate，就给这个 action 建一个 alias window**
   - `alloc_action_alias_window(...)`
   - `prt_spm_xlate_ctx_alloc(...)`

3. **给不同种类的 buffer 分页**
   - `WEIGHT`
   - `PIPE`
   - `RING`

4. **如果多个 pipe buffer 共享 alias group，就让它们共用同一套 slot pages 计划**

你可以把这一步想成：

> **在当前 segment 内部，给所有 stage 预先划好一张“共享 SPM 地图”。**

#### 为什么它要先建 alias window

因为 shared-spad xlate 的硬件视角不是“直接拿 host 指针”，而是：

- 给一段 alias 虚拟地址范围
- 用 PTE 表去解释这段范围里每个虚页该指向哪个 SPM 页

所以 `prt_action_alloc_spm()` 做的其实是：

- 先申请 alias VA 范围
- 再申请一张 PTE 表
- 再把这张表准备好给后面的 Gemmini / DMA 使用

#### 4.5.4 `prt_action_bind_topology()`：把静态计划书变成运行时执行视图

这一阶段是很多人第一次读 runtime 时容易跳过去的，但它非常关键。

它做的事情是：

1. 把 `stage_assign` 里的分配结果拷进 `exec`
   - `stage_acc_ids[i]`
   - `stage_dma_ids[i]`
   - `stage_mgr_ids[i][k]`
   - `stage_tile_counts[i]`

2. 给 pipebuf / ringbuf 挂上它们真正使用的 page list

3. 校验 shared aliasing 是否有效

4. 调 `configure_action_spm_xlate(...)`
   - 真正把 PTBR / PTE 数量 / alias base / alias bytes 安装到 manager 视图里

这一步的含义是：

> **前面 `alloc_acc` 和 `alloc_spm` 只是“算好了”；`bind_topology` 才是“把这些结果装进 live runtime 结构里”。**

### 4.6 `prt_runtime.c`：segment 级主流程

如果你第一次读 `prt_runtime.c`，很容易被里面大量 worker / log / trace 细节淹没。
第一轮不要这样读。第一轮只读它的骨架。

#### 你应该先把 `prt_runtime_run()` 读成一句话

> **逐个 segment：载入 artifact → 生成 action → 分配 pair manager → 分配 SPM → 建拓扑 → 安装 xlate → 启动 worker 或串行执行 → 等 sink 完成。**

#### 更具体一点，它的主循环大致是

1. 载入 pipeline yaml
2. 校验 artifact
3. 对每个 segment：
   - `prt_action_generate(...)`
   - `prt_action_alloc_acc(...)`
   - `prt_action_alloc_spm(...)`
   - `build-topology`
   - `prepare-stage-spm`
   - `prt_action_bind_topology(...)`
   - `flush-spm-xlate`
   - 找 sink buffer
   - host 模式串行跑，或者 FPGA 模式启动 stage worker 线程
   - 等 sink 推进到目标 subbatch

#### 你应该怎么理解这里的角色分工

- `main.c`
  - 解释 CLI 和 pair-mode 基础语义
- `prt_types.h`
  - 提供 pair-aware 公共 helper
- `prt_schedule_action.c`
  - 生成 action、分配资源、绑定拓扑
- `prt_runtime.c`
  - 驱动“这一整套计划”真正被执行

### 4.7 第四阶段读完后，必须能回答的题

- [ ] runtime 在哪里打开 pair-manager 模式
- [ ] runtime 在哪里把 DMA 编号空间回收到 Gemmini 同一空间
- [ ] `assign_stage_manager_slot()` 为什么是 pair 语义的关键 helper
- [ ] `prt_action_alloc_acc()`、`prt_action_alloc_spm()`、`prt_action_bind_topology()` 分别负责什么
- [ ] 为什么 `prt_runtime_run()` 应该先按“segment 级执行骨架”来读，而不是一上来追线程细节

---

## 5. 第五阶段：再把 runtime 的“执行平面”读明白

这一阶段回答的问题是：

> 当 stage 真正开始跑时，pair manager 语义是如何落到 acquire / DMA / Gemmini / SPM xlate 上的？

### 5.1 必读文件

- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/include/prt_rerocc.h`
- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_rerocc.c`
- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/include/prt_dma.h`
- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_dma.c`
- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/include/prt_page_table.h`
- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_page_table.c`
- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_scheduler.c`
- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_gemmini_adapter.c`

### 5.2 先理解 `scope`：ReRoCC 在 runtime 里的基本用法

如果你没有 ReRoCC 背景，`scope` 可以先理解成：

> **一段“当前这条代码临时占用了某个 manager，并把某个 opcode 绑定到它”的上下文对象。**

一个 scope 至少记住：

- 我占的是哪个 `manager_id`
- 我用了哪个 `cfg_id`
- 我绑定的是哪个 `opcode_id`
- 这个 scope 现在是否 still valid

### 5.3 `prt_rerocc.c`：runtime 如何 acquire / fence / release manager

这是执行平面里最该先读的文件，因为 DMA 和 Gemmini 都要靠它。

#### 先理解 acquire 的逻辑

`prt_rr_acquire_scope_cfg(...)` 做的事情可以先抽象成：

1. 选择一个 `cfg_id`
2. 向 `CSR_RRCFGx` 写入“我要 acquire manager Y”
3. 轮询直到 acquire 成功
4. 再做 `rr_set_opc(opcode_id, cfg_id)`
5. 从此这个 scope 代表“manager Y + opcode Z”的一段临时绑定关系

也就是说，ReRoCC 的软件用法不是“直接向 manager 7 发 custom2 指令就完了”，而是：

> **先通过 ReRoCC 控制面拿到 manager 7，再告诉系统这个 scope 现在要处理某个 opcode，之后那类指令才能稳定地送到对应设备。**

#### SPM xlate 为什么也要 acquire scope

因为它也走 ReRoCC/Gemmini 侧控制面。

在 `prt_spm_xlate_acquire_scope(...)` 里，会：

- acquire 指定 manager
- 使用 opcode 3

所以对 SPM xlate 来说：

- 它也是 pair manager 的一个“通过 `custom3` 使用的功能”

#### 为什么 release 时还要 restore opcode binding

`prt_spm_xlate_release_scope(...)` 里有一段逻辑：

- 先读旧的 opcode 绑定
- release 结束后再 restore

这是因为 xlate 这类控制可能会暂时占用某个 opcode 绑定关系。
release 后要恢复现场，避免破坏后续正常 Gemmini 使用。

### 5.4 `prt_dma.c`：pair 路线下 DMA 到底怎么被调用

这份文件非常大，但你第一次读只需要抓住“**一笔 DMA 提交的骨架**”。

#### 先抓一条最重要的事实

在很多 DMA 路径里，runtime 会把：

- `req.src_acc = manager_id`
- `req.dst_acc = manager_id`

也就是说，pair 路线下 DMA 请求不是在找另一套独立 DMA id，而是：

> **直接拿当前 pair manager id 作为 DMA 的 manager id。**

#### 一笔 DMA 提交通常经历什么

可以粗略读成：

1. acquire 一个 `custom2` 的 ReRoCC scope
2. 记录/分配 done flag
3. 编程 DMA 的目标地址 `dst`
4. 编程 DMA 的源地址 `src`
5. 触发 DMA 开始传输
6. 等 `hw_dma_fence()` 或 done flag 完成
7. 对共享 scope 再做 fence/release

所以你应该把 `prt_dma.c` 的本质理解成：

> **先通过 ReRoCC 拿到 pair manager 的 DMA 入口，再把一笔 copy 的寄存器和完成同步流程组织起来。**

#### pair 路线下 DMA 和 Gemmini 的关系是什么

它们不是：

- 完全独立、互不相关的两组 manager

而是：

- 共享同一个 pair manager id
- 只是 DMA 用 `custom2`
- Gemmini/XLATE 用 `custom3`

### 5.5 `prt_page_table.c`：shared-spad xlate 的软件侧真正实现

如果你之前没接触过这块，建议先把它理解成：

> **runtime 自己维护了一张“alias 虚拟页 → shared SPM 物理页”的软件页表，并把这张表交给硬件 xlate。**

#### 5.5.1 `prt_spm_xlate_ctx_alloc()` 在做什么

它做的事很明确：

- 为当前 action 申请一块 PTE 表内存
- 清零
- 设置 `ctx->pte`
- 设置 `ctx->pte_count`
- 设置 `ctx->ptbr_pa`
- 初始化自由虚页区间

你可以把它理解成：

> **给当前 action 建一个“小页表上下文”。**

#### 5.5.2 `prt_spm_map_tensor_ctx()` 在做什么

它做的事情是：

1. 为某个 tensor 找一段空闲虚页
2. 把这些虚页的 PTE 逐项写成“指向 shared SPM 某些物理页”
3. 发布这张表
4. 返回 alias 虚拟地址基址

这句话非常重要：

> **tensor 在 runtime 里不是“直接住在某个 host 指针上”，而是“被映射进当前 action 的 alias window 里”。**

#### 5.5.3 `prt_spm_bind_vpages_ctx()` / `prt_spm_unbind_vpages_ctx()`

这两个 helper 的角色是：

- 把某一段虚页显式绑定到某些 SPM 页
- 或解绑

这正是 `prt_action_bind_topology()` 和 stage 执行时需要的能力：
把当前 stage 真正要看的那一组页面，放进 action-local alias 视图里。

很多人第一次读到这里会疑惑：

> **既然 `prt_action_alloc_spm()` 已经给 tensor 分过页了，为什么 stage 运行前还要再 bind 一次？**

答案是：

- `alloc_spm` 决定的是“哪些 shared SPM 页归这个 action / buffer 使用”；
- `stage_prepare_exec_views()` 决定的是“当前这一轮 stage 看到的是哪一组页”。

也就是说，前者是**资源归属**，后者是**当前执行视图**。

##### `stage_prepare_exec_views()` 在做什么

你可以把它理解成：

> **在 stage 真正发 Gemmini 之前，把本轮要用的 tensor pages 写进 action-local xlate 页表。**

它不是重新申请 SPM 页，而是从当前执行态里取出“这轮有效页”，再写进 PTE。

##### 程序到底怎么确定“当前 tensor pages”

核心 helper 是 `stage_tensor_current_pages()`。它不是“拿 `tensor_id` 直接算地址”，而是分情况从 runtime 当前执行拓扑里找：

1. **fixed tensor**
   - 用 `runtime_find_weight_pages(rt, stage_id, tensor_id)`
   - 实际来源是 `exec->topo_weight_pages`

2. **entry tensor**
   - 用 `find_stage_pipebuf(rt, stage_id, tensor_id, 1)`
   - 默认取 `buf->slot_pages[buf->in_use_idx]`

3. **export tensor**
   - 用 `find_stage_pipebuf(rt, stage_id, tensor_id, 0)`
   - 默认也取 `buf->slot_pages[buf->in_use_idx]`

4. **ring fallback**
   - 如果当前 pipebuf slot 没有直接 pages
   - 且它属于 `C7/C8` 这种 all-ring 路线
   - 就退回 `buf->ring->slot_pages[buf->subbatch_offset % buf->ring->size]`

所以真正决定 pages 的，不只是 tensor 编号，而是：

- 这个 tensor 在当前 stage 是 fixed / entry / export 哪一种角色
- 当前 pipebuf 的 `in_use_idx`
- 当前 ring 的 `subbatch_offset`

这就是为什么这一步必须发生在“stage 真正要运行之前”。

##### 这些 pages 又是更早从哪里来的

再往前追两步，你会看到：

1. `prt_action_alloc_spm()`
   - 分配 `weight_pages`
   - 分配 `in_stage_pages`
   - 分配 `ring_pages`

2. `prt_action_bind_topology()`
   - 把它们复制进执行态：
     - `exec->topo_weight_pages`
     - `exec->pipebufs[*].slot_pages`
     - `exec->ringbufs[*].slot_pages`

所以 `stage_prepare_exec_views()` 不是“创建 page list”，而是“选择当前 page list”。

##### bind 时虚页起点怎么算

选到当前 pages 后，runtime 会算：

```text
exec_vpage = stage->exec_base_vpage + stage->local_spm_first_vpage[slot]
```

你要把这三个量分清：

- `exec_base_vpage`
  - 这个 stage 在 action alias window 里的起点
- `local_spm_first_vpage[slot]`
  - 这个 tensor 在 stage 局部布局里的第一页
- `local_spm_page_count[slot]`
  - 这个 tensor 需要映射几页

然后它调用 `prt_spm_bind_vpages_ctx()`，把这几页写进 PTE。

这一步的意义是：

> **让 Gemmini 接下来访问 alias VA 时，正好命中当前这轮 stage 应看到的 shared SPM 页。**

##### 为什么 bind 完还要 flush

因为软件只是改了 PTE backing memory；
Gemmini manager 这一侧是否立刻看到新映射，还要靠 xlate flush。

所以 `stage_prepare_exec_views()` 最后会调用 `runtime_flush_stage_spm_xlate()`。

读到这里时，你最好能自己复述一遍：

- action 分配阶段决定“页归谁”
- topology 绑定阶段把 page list 带到执行态
- stage 开跑前再从执行态选“当前页”
- 写进 PTE
- flush 后再让 Gemmini 用 alias VA 访问

#### 5.5.4 你应该怎样理解 alias window

你可以把 alias window 想成：

- guest 虚拟地址空间里的一段“专门留给 shared SPM 的窗口”

这段窗口本身不直接存数据；它的意义是：

- 硬件看到这个地址范围时
- 会去查 PTE
- 再决定应该访问 shared SPM 的哪几个页

所以 alias window 不是“另一份内存副本”，而是：

> **一个让软件能用普通虚拟地址视角去组织 shared SPM 访问的翻译窗口。**

### 5.6 `prt_gemmini_adapter.c`：pipeline runtime 怎样使用 Gemmini 接口

这是 Gemmini 执行平面最重要的文件之一。
你应该先把它理解成：

> **把 runtime 的 stage/task 描述翻译成真正的 Gemmini 调用序列。**

#### 先抓一个最重要的事实

这个文件并不需要“知道我是 separate 还是 pair”。

为什么？

因为到它这里时，runtime 已经把 `task->manager_ids[]` 分配好了。
它只要按这些 `manager_ids[]` 发 Gemmini 相关调用即可。

pair 模式的意义在这里表现为：

- `task->manager_ids[]` 本身就是 pair manager id
- 所以 Gemmini 命令天然会打到 pair wrapper 的 `custom3` 侧

#### 它最值得先看的三类逻辑

1. **普通 / 分块 / grouped conv 的分发**
   - `gemm_issue_conv_task(...)`
   - `gemm_issue_grouped_conv_task(...)`

2. **pointwise fallback**
   - 这部分代码很长，但本质是：为某些 1x1 / canonical pointwise 情况选一条更稳的 Gemmini 调用路径

3. **fence / drain**
   - `fence_task_managers(...)`
   - `scope-drain`

#### 为什么它频繁检查 alias/xlate 信息

因为对 Gemmini 来说，`input / weights / bias / output` 这些指针可能不是普通 DRAM 地址，而是：

- 落在当前 action alias window 里的地址

所以它会：

- 缓存当前 xlate 配置
- 检查某个指针是否落入 alias 范围
- 在日志里记录 alias region / ptbr / pte_count

你应该把这件事理解成：

> **pipeline runtime 使用 Gemmini 接口时，不只是“调一次卷积函数”，而是要确保 Gemmini 看见的地址，与当前 action 的 shared-spad xlate 视图一致。**

#### grouped conv 在这里为什么特别值得看

因为它能最清楚地展示 runtime 怎么把一个较复杂的高层算子拆成多个 Gemmini 子调用：

- 先按 group 切分输入、权重、偏置、输出指针
- 每个 group 变成一个 sub-conv
- 再按 stage 当前拿到的 `manager_ids[]` 发下去
- 需要时在 group 间做 fence

这能帮助你理解：

- runtime 不是“拿到 layer 就一把梭”
- 而是会把 layer 继续拆成 Gemmini 真正能执行的调用单元

### 5.7 `prt_scheduler.c`：别一上来读细节，先认清 C1~C8 是什么

如果你第一次打开 `prt_scheduler.c` 就想把所有状态机看懂，通常会很痛苦。

第一轮你只需要先知道：

- `C1`：entry，来自 DRAM 或前驱依赖
- `C2`：export，写回 DRAM 或后继依赖
- `C3/C4`：不带 ring 的 pair 交互
- `C5/C6`：带 ring 的 isolate 交互
- `C7/C8`：更彻底的 ring 化 entry/export

这份文件本质是在回答：

> **stage worker 在什么时机可以读 entry、什么时候能发 compute、什么时候能 export。**

先认清“它在协调 buffer 生命周期”，就够了。

### 5.8 第五阶段读完后，必须能回答的题

- [ ] ReRoCC 的 `scope` 在 runtime 里到底代表什么
- [ ] DMA 为什么也是先 acquire pair manager，再走 `custom2`
- [ ] Gemmini 为什么不需要显式再区分 pair/separate，而只需要拿 `manager_ids[]`
- [ ] alias window / PTE / PTBR 三者分别是什么关系
- [ ] `prt_page_table.c` 里的页表为什么是“当前 action 私有”的

---

## 6. 如果你只想最省时间地读：推荐两个路线

### 6.1 90 分钟速通版

如果你时间很紧，只看下面这些文件，并严格按顺序：

1. `generators/chipyard/src/main/scala/config/GemminiLearningReRoCCPairManagerConfigs.scala`
2. `generators/chipyard/src/main/scala/config/GemminiLearningReRoCCCoupledDMAConfigHelpers.scala`
3. `generators/gemmini/src/main/scala/gemmini/GemminiCoupledDMAPairWrapper.scala`
4. `generators/rerocc/src/main/scala/manager/Manager.scala`
5. `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/scripts/pairdummy_sbus128_fixed_env.sh`
6. `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/main.c`
7. `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/include/prt_types.h`
8. `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_schedule_action.c`
9. `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_runtime.c`

读完这 9 个文件，你应该能讲清：

- 什么是 P12 pair manager
- 为什么 pair wrapper 必须保留 incoming opcode
- runtime 在哪里把 DMA/Gemmini 编号空间折叠成 pair 空间
- 一个 segment 如何变成一组 stage worker 可执行的 action

### 6.2 推荐的一天阅读版

如果你有完整半天到一天，推荐顺序：

1. 本文第 1 阶段所有文件
2. 本文第 2 阶段所有文件
3. 本文第 3 阶段所有文件
4. 本文第 4 阶段所有文件
5. 本文第 5 阶段所有文件
6. 最后再回头读：
   - `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/docs/workflows/pairdummy_sbus128.md`
   - `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/docs/project_guide.md`
   - `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/docs/CURRENT_STATUS.md`
   - `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/docs/pair_wrapper_manager_plan_20260405.md`

这样顺序的好处是：

- 先把骨架读懂
- 再回头读项目文档时，你不会被历史背景带偏

---

## 7. 读这条主线时，最容易混淆的 10 个点

1. **看到 `NUM_GEMMINI=12`、`NUM_DMA=12`，不要立刻脑补成 separate**
   - 先看 `PAIR_MANAGER_MODE=1`

2. **看到 `TARGET_KEY` 里有 `g12_d12`，不要把它当硬件事实**
   - 它首先是 artifact 命名遗留

3. **pair manager != “Gemmini 和 DMA 物理完全融合成一个单元”**
   - 更准确地说，是“一对加速器被包在一个对外统一编号的 wrapper 里”

4. **ReRoCC manager != pair wrapper**
   - manager 是控制/路由前端
   - pair wrapper 是后面的实际 RoCC 复合加速器

5. **opcode 在 pair 路线里不是无关细节**
   - 它决定命令进 Gemmini 还是 DMA

6. **`preserveIncomingOpcode = true` 不是优化项**
   - 它是 pair 路线的必要条件

7. **shared-spad xlate 不是普通软件页表替代**
   - 它是“alias VA → shared SPM page”的专用翻译机制

8. **`prt_action_alloc_spm()` 不是单纯“申请内存”**
   - 它还在建立 action-local alias/xlate 视图

9. **`prt_action_bind_topology()` 不是无聊的 copy**
   - 它是真正把静态计划装进 live 执行视图

10. **`prt_gemmini_adapter.c` 不需要显式知道 pair/separate**
   - 因为 pair 语义早在更上层就已经体现在 `manager_ids[]` 上了

---

## 8. 如果你完全不看代码，也应该先记住的最终版本

最后再把整条链压缩成一段话：

> 这条 P12 路线的硬件，不是 12 个 Gemmini 加 12 个 DMA，而是 12 个 pair manager。每个 pair manager 背后是一对 `Gemmini + CoupledDMA`，由 `GemminiCoupledDMAPairWrapper` 封装成一个对外统一编号的 RoCC 复合节点。软件通过 ReRoCC 先 acquire 某个 pair manager，再用 opcode 区分要走内部的 Gemmini/SPM xlate 还是 DMA：`custom3` 走 Gemmini 和 xlate，`custom2` 走 DMA。为了让这种分流成立，ReRoCC manager 必须保留 incoming opcode，所以配置里强制 `preserveIncomingOpcode = true`。在 pipeline runtime 里，`main.c` 和 `prt_types.h` 会把历史上看起来像“两套 manager 参数”的接口折叠成“同一 pair manager id 空间”；`prt_schedule_action.c` 继续基于这个语义给 stage 分配 manager、SPM 页和 alias/xlate 视图；`prt_runtime.c` 再把这些 action 真正驱动起来；执行时由 `prt_rerocc.c` 管 acquire/fence/release，`prt_dma.c` 用 `custom2` 驱动 pair 内 DMA，`prt_page_table.c` 维护 shared-spad xlate 页表，`prt_gemmini_adapter.c` 用 `custom3` 驱动 Gemmini。把这条链读顺，你就真正读懂了这台 P12 pair-manager 机器。
