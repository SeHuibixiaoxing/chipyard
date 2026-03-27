# Pipeline Runtime Handoff

日期：2026-03-26

## 当前目标

在不修改硬件、不中断多 manager 执行语义、不恢复 `host_addr` 特判的前提下，完成 `bertmini` 在 Linux on FireSim F2 上的 end-to-end correctness closure。

固定验收口径：

- 模型：`bertmini`
- 方法：`ours2 / gemini2 / tangram2`
- 平台：Linux on FireSim F2
- 判据：`uartlog` 中出现 `BERTMINI_PIPELINE_RUNTIME_PASS`

## 当前状态快照

- 2026-03-26 最新静态审计已经把 “16 个 cfg / 4 个 opcode / 16 个 Gemmini core acquire” 之间的关系彻底厘清。
  - 当前 ReRoCC client 硬件里：
    - `nCfgs <= 16`
    - `rropc` 只有 `4` 个
  - 但当前软件栈里：
    - Gemmini 指令固定走 `custom3`
    - DMA 指令固定走 `custom2`
  - 因此当前真正成立的结论是：
    - 单 hart 可以按时间复用去管理很多 Gemmini manager
    - 但单 hart 当前只有 `1` 条 live Gemmini route lane
    - 所以单 hart 若管理一个 action，它当前最多只能“持续直接驱动”`1` 个 Gemmini manager / `1` 个 Gemmini stage issue 流
    - 不存在“因为有 4 个 opcode，所以当前单 hart 自动就能并发驱动 4 个 Gemmini core”的结论
  - 当前 runtime 还把 `cfg` 固定映射成：
    - `cfg = ((stage_id * 2) + lane) % 16`
    - `lane=0` 给 DMA
    - `lane=1` 给 Gemmini
  - 这又带来一个独立上限：
    - 单 hart 当前最多只有 `8` 组互不冲突的 stage-context
    - 超过后会发生 `cfg` alias
- 2026-03-26 当前 active blocker 没变，仍是 Linux/F2 guest runtime 内部的 functional deadlock。
  - 目前文档里最深的 live boundary 仍在 `segment 0 / stage 0 / canonical pointwise fallback / OS path`
  - 新近的 `bias id=2 + mvin3 + sizeof_D/low_D` 修补没有把 live boundary 推过去
  - 所以当前主线仍是继续定位卡死点，而不是讨论数值正确性
- baremetal correctness gate 已闭环。
  - 结果目录：
    `/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-24--12-47-02-rerocc-lc-baremetal-coupleddma-explicit-interleaved-small-f2-rerocc-baremetal-explicit-interleaved-small`
  - guest 最终输出：
    - `ALL_TESTS_PASS`
    - `*** PASSED *** after 27505375802 cycles`
- 当前 active blocker 已切回 Linux `pipeline-runtime` 主线。
- 当前不应假定有 active run farm。
  - 上一轮 on-demand F2 实例已经 terminate。
- `f2.6xlarge spot` 已验证当前不可用。
  - AWS 持续返回 `insufficient capacity`
  - 当前默认继续使用 on-demand

## 已锁定结论

以下结论没有新证据前不要重开：

1. shared-spad `1KB` interleaved alias translation 不是当前主问题。
2. `mvin2` 不是“天然不能读 interleaved shared-spad”。
3. standard Gemmini WS resadd 在当前硬件和别名设计下可用。
4. pointwise `J=128` 不是被硬件上限直接卡死。
5. `MAX_BLOCK_LEN` 不是“整层 `J` 的合法性上限”，而是 DMA 单次块宽约束。
6. 当前已闭环问题都不需要 RTL 改动。
7. 多 manager 路径不需要降级成 single manager。
8. `cfg` 数量不等于 live Gemmini 路由槽数量。
9. 当前软件栈里，单 hart 当前实际上只有 `1` 条 Gemmini route lane 和 `1` 条 DMA route lane。
10. 当前 runtime 的 `cfg` 选择规则最多只支持单 hart `8` 组互不冲突的 stage-context。
11. `rr_set_opc()` 可以在远端执行结束前重绑；已发出的请求不会被重定向，但 `rr_release()` 也不会顺带清掉 opcode 映射。

baremetal 已验证的两个关键语义：

1. `3 << (ADDR_LEN - 2)` 不是独立 B buffer。
   - 它是 accumulator local address，并且带 `accumulate-on-write` 语义。
   - 若把 `mvin2` 直接打到这里，目标 acc 行必须先初始化。
2. `gemmini_fence()` 不是 ReRoCC manager-visible completion barrier。
   - 需要 manager-visible completion 时，用 `rr_fence(cfg_id)`。
   - 已验证显式依赖链应写成：
     `A mvin -> rr_fence -> B mvin2 -> rr_fence -> mvout`

## 当前对 pipeline-runtime 的主判断

当前最强的软件侧怀疑不是硬件，而是 runtime 自己的 contract 仍然没有完全统一：

1. 负载划分是多 manager 的，但 stage-local page placement 可能还没有完全按多 manager 统一处理。
2. fixed-weight 页分配已经明显更接近多 manager 视图。
3. 但 entry/export tensor 的 local slot page placement、exec-view rebase、manager binding 仍可能残留 `stage_acc` 偏置。
4. 这会导致：
   - 编排上要求双 manager 并行
   - 地址空间和页分配却没有真正以双 manager 一致语义构建

## pipeline-runtime 改进方向

### 1. 统一 stage-local page placement / manager contract

重点检查这些点是否完全一致：

- `fixed-weight` 页分配
- `entry/export` local slot 页分配
- shared pipebuf canonical 页分配
- exec-view base address / rebase 规则
- stage manager 绑定与 `stage_acc` 使用

目标 contract 应该是：

- shared-spad 虚拟地址位于与 DRAM 不重叠的独立 alias range。
- 用户程序或 runtime 先为这段连续虚拟地址安装 PTE。
- 每个 Gemmini manager 都能识别这段虚拟地址范围，并独立完成页表翻译。
- 同一个 stage 的多 manager 可以在相同连续 VA 视图上访问 shared-spad。
- 物理页仍保持 `all-bank / 1KB interleaved / tile-交错` 的原始策略，不因软件修复而被打破。

### 2. 收紧 HybridMapper -> runtime 元数据契约

当前已经做过、但还需要继续守住的方向：

- 不再“猜 stride”。
- HybridMapper 已开始导出 `tensorStride`。
- runtime loader / descriptor builder 已开始读取 `tensorStride` 并校验 tensor size。

接下来要继续做的是：

- 确认 `input / weight / output size` 与 `in_stride / weight_stride / out_stride` 一致。
- 明确哪些层的 `pad` 仍在 runtime 里推导，而不是显式导出。
- 对需要 padding 的层，确认推导逻辑是否与 model definition 一致。

换句话说：

- `stride` 不应再靠经验值或 channel 数猜出来。
- `pad` 若暂时还不能从 HybridMapper 显式导出，就必须在 runtime 中做可验证的一致性推导，而不是隐式假设。

### 3. Gemmini 路径优先回归标准语义

- 能用 standard WS path 的地方，优先用 standard WS path。
- 手写 explicit 路径只在标准路径无法满足语义时保留。
- 手写 explicit 路径必须严格遵守 baremetal 已验证的语义：
  - accumulator 地址位语义
  - `rr_fence(cfg_id)` completion 语义
  - 不能默认 `mvin2` 是纯覆盖写
- 不要回到 `host_addr` 特判。
- 尽量不要引入 pack/repack 之类改变张量布局的 workaround。

### 4. 先查软件 contract，再怀疑硬件

继续调试时的默认顺序：

1. page placement / manager contract
2. HybridMapper metadata contract
3. Gemmini ISA 使用语义
4. 最后才是硬件怀疑

### 5. 面向未来负载的并发优化方向

未来目标已经明确为：

- 最多 `64` 个 Gemmini core
- 最多 `6` 个 action 同时 active
- `6` 个 CPU / hart 各自管理 action
- 每个 hart 可能希望同时驱动多个 stage，并并发发 Gemmini + DMA 指令

基于当前静态审计，这个目标不能直接建立在现有单-hart 路由模型上。

短期准则：

- 若要保留真正的 stage 并发，优先继续沿用“多 stage worker / 多 hart”模式。
- 不要把“一个 hart 管整个 action”当作当前已经可扩展到多 stage 并发的实现。

中期软件改进方向：

- 先把 `cfg` 分配从当前 `stage_id * 2 mod 16` 的固定散列，改成显式的 hart-local / action-local allocator。
- 把 Gemmini 发射路径改造成可选择多个 custom opcode，而不是硬编码只走 `custom3`。
- 在现有 `4` 个 `rropc` 不变时，若保留 `1` 条给 DMA，理论上单 hart 最多也只会有 `3` 条 Gemmini live route lanes。

长期硬件改进方向：

- 若最终坚持“单 hart 管一个 action，同时高并发驱动多个 stage”，需要增加 route lane 资源，而不是只增加 manager/core 数量。
- 单纯把 Gemmini core 扩到 `64` 个，并不会自动解决单 hart 的 opcode/cfg 竞争问题。

## 最值得先看的代码

优先顺序：

1. `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_runtime.c`
   - `build_topology_from_pipeline(...)`
   - `runtime_stage_local_page_accs(...)`
   - `register_shared_plan(...)`
   - `stage_prepare_exec_views(...)`
   - `stage_tensor_exec_addr(...)`
   - `build_stage_conv_desc(...)`
   - `build_stage_resadd_desc(...)`
2. `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_gemmini_adapter.c`
   - pointwise / grouped-conv / resadd path选择
   - 是否仍有手写路径绕开 baremetal 已验证语义
   - 是否仍有 direct-strided / fallback path 偏离 pipeline buffer contract
3. `conference/HybridMapper/HybridMapper/Model.py`
   - `tensorStride` 导出
4. `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_yaml_loader.c`
   - `tensorStride` / `tensorSize` / address metadata 读取

## 当前工作树注意事项

当前工作树不是干净的。不要随手回退这些本地修改：

- `conference/HybridMapper/HybridMapper/Model.py`
- `conference/mudnac_hybridmapper_collab_docs/*.md`
- `scripts/firesim-tmux-run.sh`
- `scripts/firemarshal-tmux-run.sh`
- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/*`
- baremetal interleaved regression 相关新增文件

## FireSim / FireMarshal 纪律

- 固定流程：
  `marshal build -> marshal install -> launchrunfarm -> infrasetup -> runworkload -> terminaterunfarm`
- 长任务必须通过 `tmux` wrapper：
  - `scripts/firemarshal-tmux-run.sh`
  - `scripts/firesim-tmux-run.sh`
- FireSim manager 命令前必须：
  - `cd /home/ubuntu/chipyard/sims/firesim`
  - `source sourceme-manager.sh --skip-ssh-setup`
- Linux 启动阶段，只要没有明确错误、heartbeat 还在增长，就继续等。
- 如果确认 run 真卡死，先停 run farm，再改代码。
- 不信 manager exit code；以 guest `uartlog` 为准。

## 建议的起手动作

1. 先读：
   - `STATUS.md`
   - `PLAN.md`
   - `LESSONS_LEARNED.md`
   - `PROCESS.md`
   - 本文件
   - `pipeline-runtime/NEXT_SESSION_PROMPT.md`
   - `pipeline-runtime/TESTPLAN.md`
2. 然后重新复现当前 Linux `bertmini` blocker。
3. 先从 runtime 的 page-placement / manager-contract 审计开始，不要一上来改硬件假设。
4. 若 runtime 改动碰到 shared-spad / resadd / pointwise 语义，再回归 baremetal gate。
