# Pipeline Runtime Handoff

日期：2026-03-24

## 当前目标

在不修改硬件、不中断多 manager 执行语义、不恢复 `host_addr` 特判的前提下，完成 `bertmini` 在 Linux on FireSim F2 上的 end-to-end correctness closure。

固定验收口径：

- 模型：`bertmini`
- 方法：`ours2 / gemini2 / tangram2`
- 平台：Linux on FireSim F2
- 判据：`uartlog` 中出现 `BERTMINI_PIPELINE_RUNTIME_PASS`

## 当前状态快照

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
