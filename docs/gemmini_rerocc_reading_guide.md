# Gemmini / ReRoCC 改动导读

这份导读是给“从 0 开始读这个仓库”的读者准备的。目标不是重复所有历史文档，而是先告诉你：

1. 这套改动在硬件上到底加了什么；
2. 除 `pipeline-runtime/` 之外，软件栈为了把这些硬件跑起来改了什么；
3. `pipeline runtime` 本体应该按什么顺序读，读每一层时要抓什么重点。

本文的对照基线是本地 reference 仓库：

- `/home/ubuntu/reference/chipyard`

如果你把当前仓库看作“改动后”，把 reference 看作“比较接近原始上游基线”，那这份导读讲的就是两者之间最值得先理解的那一部分。

---

## 0. 怎么用这份导读

建议阅读顺序就是本文的三个部分：

1. **先看硬件改动**：先建立“系统长什么样”的整体图。
2. **再看软件改动**：理解 baremetal / Linux 用户态是怎么把硬件接口用起来的。
3. **最后看 pipeline runtime**：这部分最复杂，最好在前两部分打好底以后再读。

有一个我这里先替你校正过的点：

- 本文硬件部分真正聚焦的目标配置是
  `GemminiLearningConfigSpadReRoCCGlobalNoC4C2x2P12x4x3CoupledDMAPairManagerDummy16x16Sbus128`

也就是说，本文后面默认采用的是：

- **硬件主读物**：看 `12 pair-manager` 这条配置线；
- **runtime 主读物**：看当前已经围绕 `pairdummy / sbus128` 形成体系的 runtime 实现与工作流；
- **阅读时的默认假设**：manager 空间优先按 **pair manager** 理解，而不是先按“分离的 Gemmini manager / DMA manager”理解。

---

## 1. 硬件改动

### 1.1 这一部分在解决什么问题

相对 reference，这个仓库的硬件改动不是“在单个 Gemmini 上做小修小补”，而是把系统扩成了：

- 多核 CPU；
- 多个 pair manager；
- ReRoCC manager 平面；
- shared scratchpad；
- shared-spad 页表翻译（xlate）；
- Global NoC / SBus 连接上的系统级组织。

换句话说，reference 更接近“单加速器 / 单软件接口”的直觉；当前仓库已经变成“**一个能同时管理多对 Gemmini+DMA pair，并且让软件显式编排 manager / DMA / SPM 地址空间的系统**”。

### 1.2 先拆配置名

建议先把目标配置名拆开读：

- `GemminiLearningConfigSpadReRoCC...`
  - 说明这不是普通 Gemmini config，而是围绕 `scratchpad + ReRoCC` 展开的学习/实验配置族。
- `GlobalNoC`
  - 说明系统总线侧用了 Global NoC 组织，而不是只走简单本地连接。
- `4C2x2`
  - 4 个 CPU core，按 `2 x 2` 摆放。
- `P12x4x3`
  - 12 个 pair manager，按 `4 x 3` 摆放；每个 pair 内部同时包含一个 Gemmini 和一个 Coupled DMA。
- `CoupledDMA`
  - 这里不是原始独立 DMA，而是 pair 内部和 shared scratchpad / Gemmini 地址语义耦合的 DMA 路线。
- `PairManager`
  - 说明软件和 ReRoCC manager 平面看到的是 **pair wrapper**，而不是两个独立 manager。
- `Dummy16x16`
  - 使用 dummy Gemmini 配置，阵列尺寸是 `16 x 16`。
- `Sbus128`
  - system bus 宽度是 `128-bit`。

只要把这个名字读顺了，后面的代码组织就会顺很多。

### 1.3 先看哪些文件

硬件部分建议先看这 8 个入口文件：

1. `generators/chipyard/src/main/scala/config/GemminiLearningReRoCCPairManagerConfigs.scala`
2. `generators/chipyard/src/main/scala/config/GemminiLearningReRoCCCoupledDMAConfigHelpers.scala`
3. `generators/chipyard/src/main/scala/config/fragments/RoCCFragments.scala`
4. `generators/chipyard/src/main/scala/config/fragments/ReRoCCGemminiCoupledDMAPairFragments.scala`
5. `generators/gemmini/src/main/scala/gemmini/GemminiCoupledDMAPairWrapper.scala`
6. `generators/rerocc/src/main/scala/manager/Parameters.scala`
7. `generators/rerocc/src/main/scala/manager/Manager.scala`
8. `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/PAPER_HARDWARE_ARCHITECTURE.md`

读法建议是：

- 先看配置类；
- 再看配置 helper 如何生成 pair 的 NoC 拓扑；
- 再看 pair fragment 怎么把 pair wrapper 挂进 `BuildRoCC`；
- 再看 pair wrapper 怎么在一个 ReRoCC manager 内部同时接住 `custom2` 和 `custom3`；
- 最后再回头用 `PAPER_HARDWARE_ARCHITECTURE.md` 做第二轮整理。

### 1.4 相对 reference，新增了哪些硬件层次

#### 第一层：一整套新的 GemminiLearning / ReRoCC 配置家族

reference 的 `generators/chipyard/src/main/scala/config/` 里没有这整批文件；当前仓库新增了至少几条成体系的配置线：

- `GemminiLearningConfigs.scala`
- `GemminiLearningReRoCCConfigs.scala`
- `GemminiLearningReRoCCCoupledDMAConfigs.scala`
- `GemminiLearningReRoCCPairManagerConfigs.scala`
- 以及对应 fragment

这说明当前仓库的硬件改动不是零散 patch，而是已经形成了“**按系统形态命名和扩展的配置家族**”。

#### 第二层：Multi-RoCC / Multi-manager 的挂接方式被扩展了

`RoCCFragments.scala` 相对 reference 不是简单复用，而是被明确扩展为：

- 支持 `WithMultiRoCCDirectDMA`
- 让 `WithMultiRoCCGemmini` 不再覆盖已有 RoCC 列表，而是**追加**到已有列表
- 给 Gemmini 实例显式注入 `gemmini_id`

这三点的意义很大：

- 说明软件不会再默认“只有一个 Gemmini”
- manager id 已经成为软件/硬件接口合同的一部分
- Gemmini 和 DMA 都可以作为 ReRoCC 生态里的可枚举 manager

#### 第三层：pair manager 被做成独立 fragment 与配置路线

`ReRoCCGemminiCoupledDMAPairFragments.scala` 负责：

- `WithReRoCCGemminiCoupledDMAPairManagers`

`GemminiLearningReRoCCPairManagerConfigs.scala` 负责：

- 生成 `P2`、`P8`、`P12` 等不同规模的 pair-manager 配置

它们共同表达的是：

- 软件和 ReRoCC manager 平面看到的是 **pair manager**
- pair manager 通过 `BuildRoCC` 被批量纳入系统
- 一个 manager id 同时代表一对 `Gemmini + CoupledDMA`

#### 第四层：pair wrapper 是这条主线的关键新增组件

`generators/gemmini/src/main/scala/gemmini/GemminiCoupledDMAPairWrapper.scala` 是这条路线最值得盯住的新硬件组件之一。它做了三件关键事：

- 把 Gemmini 和 Coupled DMA 封在同一个 `LazyRoCC` 里；
- 在 wrapper 内同时接受 `custom2 | custom3` 两种 opcode；
- 在 TL / ATL / STL / DCache / PTW 这些接口上，把 pair 内部两个子模块整合成一个对外 manager。

所以 pair-manager 路线不是“只是把两个 manager 编号绑在一起”，而是**真的在硬件结构上包成了一个 ReRoCC manager endpoint**。

### 1.5 目标配置实际拼装了什么

把 `GemminiLearningConfigSpadReRoCCGlobalNoC4C2x2P12x4x3CoupledDMAPairManagerDummy16x16Sbus128` 展开后，最关键的是下面这些事实：

#### CPU / pair 的规模

- `numCores = 4`，`cpuX = 2`，`cpuY = 2`
- `numPairs = 12`，`pairX = 4`，`pairY = 3`

也就是说，这个配置不是“4 核 + 1 对加速器”，而是：

- 4 个 CPU tile
- 12 个 pair manager
- 每个 pair manager 内部包含 1 个 Gemmini 和 1 个 Coupled DMA

软件看到的是一整组 **pair manager**，而不是一组分离的 Gemmini manager 与 DMA manager。

#### 总线 / 存储 / 阵列参数

- `sbusWidthBits = 16 * 8 = 128`
- `nMemoryChannels = 2`
- `meshRows = 16`
- `meshColumns = 16`
- `sharedSpadBytes = 1024 * 1024`

这里最关键的不是数字本身，而是它们表达的设计倾向：

- 用更窄的 `SBus128` 压缩总线宽度
- 仍然保留较大的 pair 数量
- shared scratchpad 统一设置为 `1 MiB`

这使得“总线/NoC/manager 组织方式”成为比单个算子本身更值得优先理解的对象。

#### NoC 组织方式

这个配置打开了：

- `useGlobalNoC = true`
- `useDeterministicGlobalNoCRouting = true`
- `useCompactPairManagerLayout = true`
- `globalNoCVirtualChannelDepth = 4`
- `pairTlMaxInFlight = Some(64)`
- `pairAtlMaxInFlight = Some(64)`

这里最值得先理解的是 `GemminiLearningReRoCCCoupledDMAConfigHelpers.buildPairLayout()`：

- 默认布局会把 CPU、pair manager、基础设施端点分层排布；
- `useCompactPairManagerLayout = true` 时，会尽量把 pair manager 区域压紧，把基础设施端点塞进顶部空位，不够时再往下扩行。

这就是为什么 helper 文件值得单独读：**NoC 布局不是写死在配置类里的，而是通过 helper 统一生成的。**

#### shared scratchpad + xlate

目标配置里的 `SharedScratchpadConfig` 打开了这些关键项：

- `enable = true`
- `global_base_addr = 0x40000000`
- `local_size_bytes = sharedSpadBytes`
- `use_page_table_xlate = true`
- `share_xlate_with_coupled_dma = true`

这意味着：

- shared scratchpad 不只是一个“本地 SRAM”
- 它对软件暴露了**可编程的页表翻译语义**
- pair 内的 Gemmini 与 Coupled DMA 共享这套 xlate 语义

这也是后面软件和 runtime 里反复出现 `spm_xlate`、`alias window`、`PTE backing` 的根源。

#### ReRoCC manager 平面

配置类里组合了：

- `new rerocc.WithReRoCCNoC(...)`
- `new rerocc.WithReRoCC(...)`
- `pairManagerConfig`

同时打开了：

- `filterDmaVisibleManagers = true`
- `connectSbusSlaveToStl = true`
- `preserveIncomingOpcode = true`

所以这套系统不是“把 Gemmini 和 DMA 分别挂在系统里”，而是：

- 先建立 ReRoCC manager 平面；
- 再把 `GemminiCoupledDMAPairWrapper` 批量纳入这个平面；
- 让每个 pair manager 在内部按 incoming opcode 把请求分发给 Gemmini 或 DMA；
- 再决定哪些 manager 可见、哪些 sbus slave 需要回连到 STL。

这里 `preserveIncomingOpcode = true` 特别关键，因为 pair manager 想在同一个 ReRoCC manager id 下同时支持 Gemmini 的 `custom3` 和 DMA 的 `custom2`，就不能在 manager 入口把 opcode 改写掉。

### 1.6 relative to reference，最值得你盯住的“修改而不是新增”点

如果你只想看“在已有文件上改了什么”，最值得盯的是：

- `generators/chipyard/src/main/scala/config/fragments/RoCCFragments.scala`
- `generators/rerocc/src/main/scala/manager/Parameters.scala`
- `generators/rerocc/src/main/scala/manager/Manager.scala`

原因是 reference 里也有它，因此你能直接感受到改动前后的差异：

- reference 更像“单 Gemmini / 简单多 RoCC”工具；
- 当前版本已经把它推进到“多 manager、追加挂接、显式 id”的系统接口层；
- 而 ReRoCC manager 自身又新增了 `preserveIncomingOpcode`，以支持 pair manager 在同一个 manager 端点上复用两类 opcode。

这是理解整个分叉的最好入口之一。

### 1.7 第一轮硬件阅读顺序

建议按这个顺序读：

1. `GemminiLearningReRoCCPairManagerConfigs.scala`
   - 先只看目标配置类和它继承的 parametric 基类。
2. `GemminiLearningReRoCCCoupledDMAConfigHelpers.scala`
   - 理解 `buildPairLayout()` 怎么生成 NoC 节点。
3. `RoCCFragments.scala`
   - 理解多 RoCC / 多 manager 是怎么挂进系统的。
4. `ReRoCCGemminiCoupledDMAPairFragments.scala`
   - 看 pair manager 如何批量实例化。
5. `GemminiCoupledDMAPairWrapper.scala`
   - 看一个 pair manager 内部如何接住 Gemmini 与 Coupled DMA。
6. `rerocc/manager/Parameters.scala` 和 `rerocc/manager/Manager.scala`
   - 看 `preserveIncomingOpcode` 为什么是 pair 路线必需项。
7. `PAPER_HARDWARE_ARCHITECTURE.md`
   - 用文档回收细节，特别是 pair manager、shared-spad / xlate、Global NoC 的整体解释。

如果你读到这里已经顺了，再去看分离的 loose-coupled 路线，把它当作 pair 路线之前的一条历史/对照线来理解。

---

## 2. 软件改动（不含 pipeline runtime）

### 2.1 这一部分在解决什么问题

相对 reference，当前 `generators/gemmini/software/gemmini-rocc-tests/` 已经不再只是“Gemmini 算子测试集合”，而是扩成了四层软件栈：

1. **公共头文件接口**
2. **baremetal 回归 / 复现程序**
3. **Linux 用户态 ReRoCC / CoupledDMA 测试**
4. **FireMarshal / FireSim 工作流打包脚本**

你可以把它理解成：

- reference 更偏“算子级 Gemmini 软件”
- 当前仓库更偏“**多 manager 硬件 bring-up + runtime 前置验证 + 工作流封装**”

### 2.2 先看哪些目录

软件部分建议先看这些目录：

- `generators/gemmini/software/gemmini-rocc-tests/include/`
- `generators/gemmini/software/gemmini-rocc-tests/bareMetalC/learn-gemmini/`
- `generators/gemmini/software/gemmini-rocc-tests/rerocc-baremetal-tests/`
- `generators/gemmini/software/gemmini-rocc-tests/rerocc-baremetal-tests-coupleddma/`
- `generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests/`
- `generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests-coupleddma/`

这六个目录已经覆盖了 “接口定义 -> 小程序验证 -> Linux bring-up -> workload 打包” 这条主线。

### 2.3 公共头文件：软件编程模型在哪些地方变了

#### `include/rerocc_coupleddma.h`

这是 reference 里没有的新头文件。它做的事情很直接：

- 把 Coupled DMA 的 `custom2` 指令接口封装成 C helper
- 暴露：
  - `set_src`
  - `set_dst`
  - `wait`
  - `read_monitor`

所以如果你想知道“软件怎么直接编程 Coupled DMA”，先看这个文件。

#### `include/rerocc_gemmini_spm_xlate.h`

这也是 reference 里没有的新头文件。它把 shared-spad xlate 的软件接口封装成了 C helper：

- `cfg`
- `range`
- `flush`
- `fault`

这说明当前软件侧已经不满足于“把一个裸地址喂给 Gemmini”，而是开始显式管理：

- 页表基址
- 有效 PTE 数量
- page shift
- alias 范围
- xlate fault

这正好和硬件里 `use_page_table_xlate = true` 对上。

#### `include/gemmini.h`

这个文件相对 reference 的变化非常大。最值得先注意的是：

- 增加了 Linux 侧的低层日志输出辅助；
- 引入了 `PRT_ENABLE_PROGRESS_*`、`PRT_ENABLE_CRITICAL_UART_PROBE` 一类的编译期宏；
- 把一些原本纯 Gemmini 头文件内部不可见的调试/可观测性逻辑暴露了出来。

直观理解就是：

- 现在的 `gemmini.h` 不只是在定义 Gemmini 指令；
- 它还承担了“在 Linux / FireSim 环境里把关键执行边界打出来”的职责。

#### `include/gemmini_nn.h`

这个文件的变化重点不是 API 形状，而是：

- 在 matmul / conv 一类热路径前后加了 raw / phase log 标记；
- 让 Gemmini 内核调用可以和 runtime / workload 侧日志对齐。

所以如果你发现软件里到处有 `graw`、`gemmini-phase` 之类的字符串，不要惊讶；这是当前仓库为了调 runtime / FireSim 工作流刻意加进去的观测层。

#### `include/gemmini_params.h`

当前 checked-in 的 `gemmini_params.h` 相对 reference 也变了，例如：

- `DIM`
- `BANK_ROWS`
- `ACC_ROWS`
- accumulator 读取宽度相关宏

你不需要在第一轮就追所有数字，但要有这个意识：

- 当前仓库默认的软件头文件参数，已经不再等同于 reference 的默认 Gemmini 参数；
- 这说明软件默认假设的硬件形状也跟着变了。

#### `include/prt_log_gate.h`

虽然名字里有 `prt`，但它不只属于 `pipeline runtime`。

它的作用是：

- 给深度日志加一个“按 segment / stage / subbatch 过滤”的 gate；
- 避免在 Linux / FireSim 下被海量串口日志淹没。

因为它被 `gemmini.h` 直接 include，所以它已经进入了“公共软件基础设施”层，而不只是 runtime 私有工具。

### 2.4 baremetal：它在做什么

#### `bareMetalC/learn-gemmini/`

这个目录是当前仓库区别于 reference 的一个关键阅读点。reference 的 `bareMetalC/` 更像常规算子测试；当前仓库新增的 `learn-gemmini/` 更像：

- 硬件接口实验台
- alias / xlate 复现台
- export DMA / pointwise / grouped conv 的问题最小复现集合

你会看到很多名字很“问题导向”的程序，例如：

- `rerocc_lc_matrix_baremetal_coupleddma.c`
- `rerocc_lc_nonblocking_baremetal_coupleddma.c`
- `rerocc_lc_export_dma_bertmini_repro.c`
- `rerocc_lc_pointwise_stage0_runtime_linuxphys_focus.c`
- `rerocc_lc_pointwise_stage0_runtime_grouped_linuxphys_focus.c`

第一轮阅读时不用全跑，只要知道这些文件的角色：

- 它们不是普通 benchmark；
- 它们是把具体 runtime / DMA / alias / xlate 问题切成小 case 的 baremetal 入口。

#### `rerocc-baremetal-tests/` 与 `rerocc-baremetal-tests-coupleddma/`

这两个目录把 baremetal 小程序进一步包装成 FireMarshal workload：

- `README.md` 说明目录角色；
- `workload/host-init.sh` 负责在 host 侧准备 payload；
- `workload/*.json` 负责 FireMarshal workload 描述。

理解它们的最好方式是：

- `bareMetalC/learn-gemmini/` 是“程序本体”
- `rerocc-baremetal-tests*` 是“把程序送进仿真 / FPGA 流程的外壳”

### 2.5 Linux 用户态：它在做什么

#### `rerocc-linux-tests/`

这是 Linux 用户态 ReRoCC / Gemmini bring-up 的主目录。

最值得先看的文件是：

- `rerocc_control.h`
  - 定义 ReRoCC CSR、acquire / release / cfg 访问语义。
- `rerocc_dma_matrix.c`
  - 用矩阵搬运/执行路径测 ReRoCC + DMA 资源获取与并发。
- `rerocc_gemmini_conv_matrix.c`
  - 用 conv/matrix 工作负载测 Gemmini 路径；在 pair-manager 模式下，它最终会落到同一个 pair manager 空间。
- `run_rerocc_lc_linux.sh`
  - 把 manager 数量、base id、目标 profile 等参数组织起来。

这部分的重点是先理解：

- manager id 是怎么被用户态程序拿来用的；
- Linux 程序如何做 acquire / dispatch / release；
- “多 manager”如何反映到命令行参数和测试矩阵里。

#### `rerocc-linux-tests-coupleddma/`

这是当前软件栈相对 reference 最有代表性的新增目录之一。

它负责把 Linux 用户态测试推进到：

- Coupled DMA
- alias export
- page map / Linux 物理地址
- shared-spad xlate

你可以重点看这些文件名的含义：

- `rerocc_dma_matrix_linux_coupleddma.c`
- `rerocc_lc_gemmini_matrix_linux_coupleddma.c`
- `rerocc_lc_nonblocking_linux_coupleddma.c`
- `rerocc_dma_export_alias_uartprobe_linux.c`
- `rerocc_linux_pagemap.h`
- `rerocc_linux_spm_xlate.h`

这组文件体现出一个非常明确的软件转向：

- 软件不再只是调用 Gemmini 算子；
- 它开始显式处理 Linux 虚实地址、DMA alias、shared-spad xlate 等系统问题。

### 2.6 workload / FireMarshal glue：为什么脚本会变这么大

这一层是很多初读者最容易忽略，但实际上非常关键的一层。

#### `rerocc-linux-tests/workload/host-init.sh`

这个脚本已经不只是“拷一个二进制进镜像”了，它会负责：

- 选择 / 检查运行时 artifact 目录；
- 编译 Linux 二进制；
- 重编 `rerocc_pipeline_runtime-linux`；
- 校验生成出来的 runtime binary 里是否带有预期 marker；
- 把二进制和 artifact staged 到 overlay。

即使你现在不读 runtime，本脚本也值得看，因为它把“软件如何进入 FireMarshal 镜像”说清楚了。

#### `rerocc-linux-tests-coupleddma/workload/host-init.sh`

这是当前整个工作流 glue 层最重的脚本之一。它做的事情包括：

- 区分 pipeline runtime 是否启用；
- 收集 / 生成 bertmini runtime artifact；
- 生成 guest 环境变量；
- 处理 mapping cache；
- stage overlay；
- 准备 capture / breadcrumb / trigger log 相关路径；
- 把 host 侧的 runtime 配置传给 guest。

第一轮阅读时你不用把所有环境变量都记住，但要理解一个事实：

- 当前仓库的软件栈已经把“编译程序”扩展成了“**编译 + artifact 组织 + guest 环境注入 + FireMarshal 镜像 staging**”。

### 2.7 第一轮软件阅读顺序

建议按这个顺序读：

1. `include/rerocc_coupleddma.h`
2. `include/rerocc_gemmini_spm_xlate.h`
3. `rerocc-linux-tests/rerocc_control.h`
4. `rerocc-linux-tests/rerocc_dma_matrix.c`
5. `rerocc-linux-tests/rerocc_gemmini_conv_matrix.c`
6. `rerocc-linux-tests-coupleddma/` 下几类 Linux 测试
7. `bareMetalC/learn-gemmini/` 下跟 `coupleddma / alias / pointwise / export-dma` 相关的小程序
8. `rerocc-baremetal-tests*` 和 `rerocc-linux-tests*/workload/host-init.sh`

读完这一轮，你应该能回答三个问题：

1. 软件怎么拿到 manager id，并在 pair-manager 模式下把 Gemmini / DMA 映射到同一个 pair id 空间？
2. 软件怎么设置 Coupled DMA 和 SPM xlate？
3. 为什么 host-init / overlay / FireMarshal 脚本会变成当前这个复杂度？

---

## 3. pipeline runtime 运行时

### 3.1 先给它一个准确定位

`pipeline runtime` 不是“自动搜索最优调度”的框架；它更像：

- 一个 **artifact consumer**
- 一个 **按既定 pipeline/segment/stage 执行的 runtime**
- 一个把 `HybridMapper` 导出的 mapping 落到
  `Gemmini + ReRoCC + CoupledDMA + shared-spad xlate`
  上的执行器

所以读它时最重要的心智模型是：

- 它不负责重新发明 mapping；
- 它负责**解析 artifact、准备资源、按 segment/stage 执行、维护 SPM / DMA / manager 状态，并把结果带回**。

对当前这条主线，还要多补一句：

- runtime 的工作流默认应该和 `pair-manager / pairdummy / sbus128` 这条硬件线一起理解；也就是 **Gemmini 与 DMA 共享同一个 pair manager id 空间**。

### 3.2 第一轮不要从哪里开始

建议你第一轮**不要**先从这些地方开始：

- `pipeline-runtime/debug_records/`
- `pipeline-runtime/change_records/`
- `pipeline-runtime/docs/archive/`

这些文件对历史问题很有用，但不适合第一次建立代码心智模型。

第一轮只建议把它们当“以后查案底的地方”。

### 3.3 runtime 的顶层入口

runtime 的入口非常薄：

- `generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests/rerocc_pipeline_runtime_linux.c`

它只做一件事：

- 调 `prt_main_entry(argc, argv)`

真正的主入口在：

- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/main.c`

你读 `main.c` 时应该重点抓这几件事：

- CLI 参数长什么样；
- 默认 runtime 配置是什么；
- 默认 backend / manager 数 / xlate / watchdog / log gate 是什么；
- `pair_manager_mode` 如何把 Gemmini / DMA 两套局部编号折叠成同一个 pair manager 空间；
- 程序在 very-early 阶段做了哪些事情，例如：
  - live stdio
  - `mlockall`
  - build-config / progress / critical marker 打印

也就是说，`main.c` 主要负责：

- **接命令行**
- **装默认值**
- **把运行参数组装成 `prt_runtime_cfg_t` + `prt_run_args_t`**
- **调用 `prt_runtime_init()` / `prt_runtime_run()` / `prt_runtime_destroy()`**

### 3.4 runtime 的输入合同

runtime 的输入不是单个二进制，而是几类 artifact 的组合：

- `model yaml`
- `pipeline yaml`
- `layer mapping yaml`
- 可选 `model bin`
- 可选 `input`
- 可选 `golden`

这一层的入口文件是：

- `pipeline-runtime/include/prt_yaml_loader.h`
- `pipeline-runtime/src/prt_yaml_loader.c`
- `pipeline-runtime/include/prt_gemmini_artifacts.h`
- `pipeline-runtime/src/prt_gemmini_artifacts.c`

读法建议：

1. 先看 `prt_load_model_yaml()` / `prt_load_pipeline_yaml()`
2. 再看 `prt_validate_gemmini_artifacts()`

这一步要搞清楚的是：

- runtime 内部的 `model`、`pipeline`、`segment`、`stage`、`tensor` 到底长什么样；
- runtime 会不会在执行期重新推导 mapping；
- layer mapping 是如何被拿来校验 pipeline/stage 合同的。

如果你只抓一句话，那就是：

- `yaml_loader` 负责**读结构**
- `gemmini_artifacts` 负责**校验语义并把 Gemmini 相关映射补全/核对**

### 3.5 先理解对象模型，再读执行细节

runtime 最重要的公开类型主要在两个头文件里：

- `pipeline-runtime/include/prt_types.h`
- `pipeline-runtime/include/prt_runtime.h`

建议你按下面这个顺序认识对象：

#### 1) `prt_runtime_t`

这是全局 runtime 对象。你可以把它看成“本次运行的总控制块”，里面装着：

- 配置 `cfg`
- DMA / Gemmini backend ops
- model / pipeline / model blob
- 当前 active action
- 多 action 队列
- page allocator / PTE backing / alias map
- trace / stats / pending DMA / completion flag

第一轮阅读重点：

- 它有哪些资源是“全局共享”的；
- 哪些字段是和 SPM/page table/xlate 相关的；
- 哪些字段是和 trace / watchdog / DMA completion 相关的。

#### 2) `prt_schedule_action_t`

这是“**一个 segment 被 runtime 展开后的调度对象**”。

你可以把它理解成：

- 一个 segment 对应一个 action；
- action 负责保存这个 segment 执行所需的 manager / SPM / alias / xlate / topology 视图。

这个类型定义在：

- `pipeline-runtime/include/prt_schedule_action.h`

重点字段包括：

- `segment_idx`
- `acc_source`
- `spm_source`
- `spm_xlate`
- `alias_base_va`
- `alias_page_count`
- `assigned_hart_id`

#### 3) `prt_action_exec_t`

这是 action 的“执行态展开”对象，定义在：

- `pipeline-runtime/include/prt_runtime.h`

它里面装的是更贴近执行器的数据结构：

- stage threads
- pipebufs
- ringbufs
- isolate/shared pair
- stage-local shadow / bounce buffer

所以可以这样记：

- `action` 偏“调度与资源拥有者”
- `action_exec` 偏“真正执行时的缓冲和线程视图”

#### 4) `prt_pipebuf_t` / `prt_ringbuf_t`

这两个类型在：

- `pipeline-runtime/include/prt_types.h`

它们是理解 scheduler 的核心。

你不需要一开始记住每个字段，但一定要先看懂：

- `C1 ~ C8` 这八类 pipebuf kind 的语义划分；
- entry/export/isolate/shared/ring 之间的关系；
- double buffer、full/empty、DMA token、fanout 等状态为什么放在 buffer 对象上。

一句话总结：

- `runtime` 不是直接把 stage 连起来；
- 它是先把 stage 之间的数据流物化成 buffer / ring / pair，然后再推进它们的状态机。

### 3.6 runtime 的主执行流程

最值得读的主流程在：

- `pipeline-runtime/src/prt_runtime.c`

你第一轮阅读时，建议直接找 `prt_runtime_run()`，然后按顺序看它做了什么。主流程大致是：

1. 清理 TLS / trace 状态
2. load `model yaml`
3. 收集 model input / output id
4. load `pipeline yaml`
5. validate artifacts
6. 加载或合成 `model bin`
7. 加载 input / golden（如果配置要求）
8. 逐个 `segment` 处理：
   - `prt_action_generate()`
   - `prt_action_alloc_acc()`
   - `prt_action_alloc_spm()`
   - build topology
   - prepare stage SPM windows
   - `prt_action_bind_topology()`
   - flush / install xlate context
   - 启动 stage worker
   - 盯 sink buffer 进度
   - 收尾 / release action

第一次看时，不要急着钻进每个 helper；先把这条骨架顺下来。

### 3.7 action 生命周期：从 segment 到可执行对象

这一层最关键的文件是：

- `pipeline-runtime/include/prt_schedule_action.h`
- `pipeline-runtime/src/prt_schedule_action.c`
- `pipeline-runtime/src/prt_action_exec.c`
- `pipeline-runtime/src/prt_action_queue.c`
- `pipeline-runtime/src/prt_action_spm_context.c`

你可以把这一层分成四步来读：

#### 第一步：`prt_action_generate()`

作用：

- 为一个 segment 生成 action 壳子；
- 把 pipeline/model 引用挂进去；
- 初始化 action 的基本状态。

#### 第二步：`prt_action_alloc_acc()`

作用：

- 给 stage 分配 Gemmini manager / DMA manager；
- 决定 action 能看到哪些 manager；
- 形成 `acc_source`。

这一步是“硬件 manager 平面”和“软件调度对象”第一次真正接上。

但对当前目标配置，需要按 pair-manager 口径来读：

- `prt_runtime_cfg_t` 里虽然同时保留了 `num_gemmini_mgrs`、`num_dma_mgrs`、`gemmini_mgr_base_id`、`dma_mgr_base_id`；
- 可是一旦 `pair_manager_mode = 1`，DMA manager 的计数和 base id 会回收为 Gemmini 那套空间；
- 也就是说，stage 级别看到的 Gemmini / DMA 分配，最终会折叠到同一个 pair manager id 空间。

#### 第三步：`prt_action_alloc_spm()`

作用：

- 为 action 分配 SPM page；
- 组织 weight / in-stage / ring page；
- 准备 alias / page table / xlate 上下文。

这一步是整个 runtime 与 shared-spad 机制耦合最深的一层之一。

#### 第四步：`prt_action_bind_topology()`

作用：

- 把 pipeline 里的 stage/tensor 拓扑，绑定到执行期的 pipebuf / ringbuf / pair / page list 上；
- 让后续 worker 能按统一的数据流视图推进。

如果你在阅读时感觉“action 很抽象”，那就把注意力放回这四个函数；它们就是从“描述”走到“可执行实体”的关键桥梁。

### 3.8 stage worker 与 scheduler：runtime 真正怎么跑

这一层的关键文件是：

- `pipeline-runtime/src/prt_runtime.c`
- `pipeline-runtime/src/prt_stage_worker_multi_action.c`
- `pipeline-runtime/include/prt_scheduler.h`
- `pipeline-runtime/src/prt_scheduler.c`
- `pipeline-runtime/src/prt_async_dma.c`

建议你这样理解它们的分工：

#### `prt_stage_worker_multi_action.c`

这个文件现在很薄，主要负责：

- 找到当前 thread / hart 对应的 action；
- 把 TLS 上的 current action 绑好。

它更像 worker 上下文接线层，而不是全部执行逻辑本体。

#### `prt_runtime.c`

这是执行总控：

- 初始化 runtime；
- 组织每个 segment 的执行；
- 创建 stage worker 线程；
- 看 sink buffer 是否完成；
- 跑 watchdog / trace / stop/fatal 错误控制；
- 最后做 destroy / 收尾。

#### `prt_scheduler.c`

这是 buffer 状态机核心。

它公开了：

- `prt_process_c1()` 到 `prt_process_c8()`
- `prt_progress_export_dma()`

所以第一轮阅读 `scheduler` 时，重点不是逐行追，而是先回答：

- C1~C8 分别对应什么 transport / materialization 语义？
- 哪些步骤是 load / export DMA？
- 哪些步骤是 isolate/shared pair 或 pure ring 传播？

#### `prt_async_dma.c`

这个文件是对 scheduler 的补丁层：

- 当 DMA backend 不是 blocking fence，而是 progress thread 轮询时，给 `C1` / `C3` 之类路径提供 async 版本。

也就是说，`async_dma.c` 不是另一套 scheduler，而是“给既有 scheduler 加异步 DMA 分支”。

### 3.9 硬件适配层：runtime 怎样碰硬件

这一层建议按下面顺序读：

1. `pipeline-runtime/include/prt_rerocc.h`
2. `pipeline-runtime/src/prt_rerocc.c`
3. `pipeline-runtime/include/prt_dma.h`
4. `pipeline-runtime/src/prt_dma.c`
5. `pipeline-runtime/include/prt_page_table.h`
6. `pipeline-runtime/src/prt_page_table.c`
7. `pipeline-runtime/include/prt_gemmini_adapter.h`
8. `pipeline-runtime/src/prt_gemmini_adapter.c`

#### `prt_rerocc.*`

负责：

- acquire / release ReRoCC scope
- fence scope
- 编程 Gemmini shared-spad xlate
- 读取 fault

它是 runtime 与 ReRoCC CSR/manager 协议的最薄接口层。

#### `prt_dma.*`

负责：

- DMA backend 初始化
- DMA submit / wait / cleanup
- DRAM ↔ SPM、SPM ↔ SPM 的复制
- completion flag / token / trace / probe

如果你想理解：

- Linux 下 DMA completion 为什么要有 token；
- export DMA 为什么需要 timeout；
- doneflag / probe / trace 为什么这么多；

那就重点看这里。

#### `prt_page_table.*`

负责：

- SPM PTE backing
- vpage 分配 / 回收
- tensor page 映射
- alias window 翻译
- `virt_to_phys`
- hugetlb / contiguous backing 相关逻辑

这层是理解 shared-spad xlate 的核心。第一次读时，重点抓：

- 为什么 runtime 需要 action-local xlate context；
- 为什么 alias window、PTE backing、PTBR 会被单独维护；
- 为什么这部分和普通匿名页/普通 malloc 不是一回事。

#### `prt_gemmini_adapter.*`

负责：

- 把 runtime 的 conv / resadd 描述转成 Gemmini 调用；
- 处理 blocking / async Gemmini 模式；
- 处理 grouped conv / pointwise / fallback；
- 在 Gemmini 热路径上打 breadcrumb / trigger / alias 观测点。

你可以把它理解成：

- runtime 的“算子执行后端”

它是软件抽象和 Gemmini 具体调用之间最厚的一层。

### 3.10 可观测性：为什么有这么多 log / breadcrumb / trigger

这一层建议专门单独看一次。关键文件是：

- `pipeline-runtime/include/prt_breadcrumb.h`
- `pipeline-runtime/src/prt_breadcrumb.c`
- `pipeline-runtime/include/prt_trigger_log.h`
- `pipeline-runtime/src/prt_trigger_log.c`
- `pipeline-runtime/src/prt_log_gate.c`
- `pipeline-runtime/include/prt_progress.h`

当前 runtime 的可观测性不是单一日志，而是几层叠加：

1. `progress log`
   - 粗粒度进度
2. `critical/raw marker`
   - 热路径边界标记
3. `breadcrumb`
   - 二进制环形槽位，低扰动记录关键阶段
4. `trigger log`
   - 命中特定 family / stage / manager / tensor / token 后才放大的窄日志
5. `trace`
   - 运行期统计和事件流

这套设计的阅读重点不是“API 好不好看”，而是要理解它在解决什么问题：

- FireSim / Linux 下高频文本日志太贵；
- 但如果没有细粒度观测，又抓不到卡住的位置；
- 所以 runtime 逐步演化成“breadcrumb + gate + trigger”的组合方案。

### 3.11 runtime 与 workload / 工作流脚本如何接起来

要把 runtime 看成“完整可运行系统”，还需要读这几层外壳：

- `generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests/Makefile`
- `generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests/run_rerocc_pipeline_runtime_bertmini.sh`
- `generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests-coupleddma/workload/host-init.sh`
- `generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests-coupleddma/workload/run_rerocc_pipeline_runtime_bertmini_capture.sh`
- `generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests-coupleddma/workload/run_rerocc_pipeline_runtime_bertmini_fileonly_sync_capture.sh`
- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/scripts/`

这几层分别负责：

- **Makefile**
  - 把 `pipeline-runtime/src/*.c` 编成 `rerocc_pipeline_runtime-linux`
- **guest runner**
  - 在 guest 里组织 runtime 命令行
- **host-init**
  - 构建二进制、收集 artifact、生成 guest env、stage overlay
- **capture 脚本**
  - 控制 stdout / file-only / sync / procdiag 等不同捕获策略
- **scripts/**
  - 提供 artifact audit、mapping cache 生成、breadcrumb 解码等辅助工具

你如果只读 `pipeline-runtime/src/` 而不看这一层，会知道“runtime 怎么写”，但不会知道“runtime 怎么跑起来”。

### 3.12 第一轮 runtime 阅读顺序

我建议你严格按下面顺序读：

1. `rerocc-linux-tests/rerocc_pipeline_runtime_linux.c`
2. `pipeline-runtime/src/main.c`
3. `pipeline-runtime/include/prt_types.h`
4. `pipeline-runtime/include/prt_runtime.h`
5. `pipeline-runtime/include/prt_schedule_action.h`
6. `pipeline-runtime/src/prt_yaml_loader.c`
7. `pipeline-runtime/src/prt_gemmini_artifacts.c`
8. `pipeline-runtime/src/prt_schedule_action.c`
9. `pipeline-runtime/src/prt_action_exec.c`
10. `pipeline-runtime/src/prt_runtime.c`
11. `pipeline-runtime/src/prt_scheduler.c`
12. `pipeline-runtime/src/prt_dma.c`
13. `pipeline-runtime/src/prt_page_table.c`
14. `pipeline-runtime/src/prt_gemmini_adapter.c`
15. `pipeline-runtime/src/prt_breadcrumb.c` / `prt_trigger_log.c`
16. `rerocc-linux-tests/run_rerocc_pipeline_runtime_bertmini.sh`
17. `rerocc-linux-tests-coupleddma/workload/host-init.sh`

这个顺序的核心原则是：

- 先看入口
- 再看数据结构
- 再看 artifact 合同
- 再看 action 生命周期
- 再看主执行流程
- 最后看硬件适配与可观测性

### 3.13 第二轮再看哪些文档

当你把代码路径走过一遍以后，再看这些文档会非常有帮助：

- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/docs/project_guide.md`
- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/PAPER_SOFTWARE_RUNTIME_ARCHITECTURE.md`
- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/PAPER_HARDWARE_ARCHITECTURE.md`
- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/docs/architecture/runtime_mechanisms.md`
- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/docs/testing/observability.md`

推荐顺序是：

1. 先读代码
2. 再用 `project_guide.md` 收拢全局图
3. 再用 `PAPER_*_ARCHITECTURE.md` 收拢系统与论文表达
4. 最后查 `debug_records/` / `change_records/` / `archive/`

---

## 4. 一页版总结：你应该先建立什么心智模型

如果你只能记住最少的几句话，我建议记住下面这组：

- **硬件上**：当前仓库已经不是“单 Gemmini 测试平台”，而是“多 Gemmini + 多 CoupledDMA + shared-spad xlate + ReRoCC + Global NoC”的系统。
- **软件上**：当前 `gemmini-rocc-tests` 已经从单纯算子测试库，扩成了接口头文件、baremetal 回归、Linux bring-up、FireMarshal/FireSim glue 的完整栈。
- **runtime 上**：`pipeline runtime` 不是自动规划器，而是 artifact consumer + action/segment 执行器；核心是 `action`、`pipebuf/ringbuf`、`SPM xlate`、`DMA/Gemmini adapter`、`breadcrumb/trigger` 这几层。

如果你按本文建议的顺序读，第一轮就能先把“系统怎么搭起来”读顺；之后再进入具体实验记录、pair-manager 路线和历史问题，会轻松很多。

如果你只想沿着 `P12 pair-manager / pairdummy / sbus128` 这一条主线读代码，优先使用更聚焦的清单：

- `docs/p12_pair_manager_code_reading_checklist.md`
- 如果你正在逐文件精读 `GemminiCoupledDMAPairWrapper.scala`，再配合：
  - `docs/gemmini_coupleddma_pair_wrapper_reading_guide.md`
- 如果你正在从软件接口角度单独读 runtime / API，再配合：
  - `docs/software_rerocc_api_guide.md`
  - `docs/software_spm_xlate_api_guide.md`
  - `docs/software_coupleddma_api_guide.md`
  - `docs/software_gemmini_api_pipeline_runtime_guide.md`
