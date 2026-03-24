# Pipeline Runtime Tactical Plan

日期：2026-03-24

先读：

- `HANDOFF.md`
- `STATUS.md`
- `LESSONS_LEARNED.md`

## 目标

在不修改硬件、不恢复 `host_addr` 特判、不破坏 shared-spad `all-bank / 1KB interleaved / multi-manager` 设计目标的前提下，完成 `bertmini` 在 Linux on FireSim F2 上的 end-to-end correctness closure。

最终验收固定为：

- 模型：`bertmini`
- methods：`ours2 / gemini2 / tangram2`
- 平台：Linux on FireSim F2
- 判据：`uartlog` 中出现 `BERTMINI_PIPELINE_RUNTIME_PASS`

## 当前起点

- baremetal correctness gate 已闭环。
  - baremetal 当前不再是 active blocker。
  - 后续它只作为 regression gate。

- `spot` 试跑已经完成。
  - 当前结论是 AWS 容量不足。
  - 在没有新容量信号之前，不继续在 `spot` 上耗时。

- 当前 active 主线重新回到 Linux `pipeline-runtime`。
- 当前不应假定还有 active run farm。

## 固定约束

- 不改 RTL / 不改硬件。
- 不恢复 `host_addr` 特判。
- 不把多 manager 路径降级成 single manager。
- 不破坏 shared-spad 的 `all-bank / 1KB interleaved` 分配策略。
- 不引入 bertmini 路径上的 CPU fallback。
- 尽量不引入 pack/repack 之类改变张量布局的 workaround。
- FireSim manager 固定流程仍然是：
  `marshal build -> marshal install -> launchrunfarm -> infrasetup -> runworkload -> terminaterunfarm`
- `launchrunfarm` / `infrasetup` / `runworkload` / `terminaterunfarm` 必须通过：
  [firesim-tmux-run.sh](/home/ubuntu/chipyard/scripts/firesim-tmux-run.sh)
- manager 环境必须从：
  [sims/firesim](/home/ubuntu/chipyard/sims/firesim)
  下执行 `source sourceme-manager.sh --skip-ssh-setup`
- Linux 启动阶段只要没有明确错误，且 heartbeat 继续增长，就按“慢启动”处理，不按“卡死”处理。

## 当前已经固定的技术结论

### 1. baremetal 已证明 shared-spad 设计本身不是当前问题

- standard WS resadd 可用。
- dual-manager split resadd 可用。
- interleaved shared-spad alias translation 可用。
- `mvin2` 访问 interleaved shared-spad 也不是普遍坏掉。

### 2. 不能重新打开的旧假设

- 不要重新怀疑 shared-spad 页表设计本身。
- 不要重新怀疑 pointwise `J=128` 硬件上限。
- 不要重新怀疑必须禁止跨 tile shared-spad 访问。
- 不要重新怀疑必须把多 manager 路径收缩成单 manager。

### 3. 当前 runtime 更强的可疑点仍是 page placement contract

- fixed-weight 页分配已经按多 manager 视图处理。
- 但 entry/export tensor 的 local slot page 分配、exec-view rebase、manager binding 仍可能偏向单一 `stage_acc`。
- 这会让：
  - 负载划分是多 manager 的
  - 地址空间管理却不是多 manager 一致的
- 因而当前最值得优先排查的是：
  runtime 自己的 stage-local page placement contract 是否统一。

### 4. stride 语义已经进入主线 contract，但 pad 仍需继续盯住

- `stride` 不应再猜。
- HybridMapper 已开始导出 `tensorStride`，runtime 也已消费这份元数据。
- 但 `pad` 仍可能依赖 runtime 从 size/shape 关系中推导。
- 因而当前要继续检查：
  - `input / weight / output size`
  - `in_stride / weight_stride / out_stride`
  - `pad`
  - layer definition
  这几者之间是否完全一致。

## 执行计划

### Phase 1. 固化 baremetal regression gate

- 保留当前 baremetal workload 作为 shared-spad / resadd / pointwise 的回归门：
  [rerocc-lc-baremetal-coupleddma-explicit-interleaved-small.json](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/rerocc-baremetal-tests-coupleddma/workload/rerocc-lc-baremetal-coupleddma-explicit-interleaved-small.json)
- 后续只要改动下列任一语义，就必须回归 baremetal：
  - shared-spad xlate
  - explicit `mvin/mvin2/mvout`
  - resadd path
  - pointwise fallback path

### Phase 2. 回到 Linux runtime 主线复现当前 blocker

- 在当前 head 上重新做 Linux FireSim replay。
- 只要 Linux boot 没有明确错误且 heartbeat 在前进，就继续等。
- 判定 live blocker 时：
  - 优先看 guest `uartlog`
  - 不信 manager exit code
  - 若真卡死，先回收 runfarm 再改代码

### Phase 3. 先审 runtime 的 manager/page contract，不先怀疑硬件

- 审查 runtime 中以下两类页分配是否统一：
  - fixed-weight tensor
  - stage-local entry/export tensor
- 审查以下语义是否一致：
  - manager 绑定
  - local exec view
  - page placement
  - tile/subview rebase
- 目标是把同一 stage 的多 manager 执行视图收敛到统一 contract：
  - 连续 shared-spad alias VA
  - 每 manager 独立页表翻译
  - 保持 all-bank / 1KB interleaved 物理页策略不变

### Phase 4. 补全 HybridMapper -> runtime 元数据 contract

- 保持 `tensorStride` 导出和消费一致。
- 不再回到 guessed stride。
- 对有 padding 的层，明确：
  - 继续推导，还是导出显式 metadata
- 用模型尺寸关系验证：
  - input size
  - weight size
  - output size
  - stride
  - pad

### Phase 5. 若 runtime 仍需手写 explicit path，必须遵守 baremetal 已验证的语义

- `mvin2` 到 accumulator 不是“独立 buffer load”。
- 若想得到“纯覆盖写入”的效果，必须先初始化目标 acc 行。
- `gemmini_fence()` 不是 ReRoCC manager 可见的 completion barrier。
- 需要 manager-visible completion 时，使用 `rr_fence(cfg_id)`。
- 显式依赖链若存在，必须类似：
  `A mvin -> rr_fence -> B mvin2 -> rr_fence -> mvout`
- 能回到 standard WS path 的地方，优先回到 standard WS path。

### Phase 6. 完整 closure

- Linux `bertmini` on FireSim F2 输出正确。
- `BERTMINI_PIPELINE_RUNTIME_PASS` 出现在 `uartlog`。
- baremetal regression 继续保持全绿。

## 成功标准

- Linux runtime 路径闭环。
- baremetal gate 不回归。
- 无需 RTL 改动。
- 无需 `host_addr` 特判。

## 参考文档

- handoff 摘要：
  [HANDOFF.md](/home/ubuntu/chipyard/conference/mudnac_hybridmapper_collab_docs/HANDOFF.md)
- 当前状态：
  [STATUS.md](/home/ubuntu/chipyard/conference/mudnac_hybridmapper_collab_docs/STATUS.md)
- 经验教训：
  [LESSONS_LEARNED.md](/home/ubuntu/chipyard/conference/mudnac_hybridmapper_collab_docs/LESSONS_LEARNED.md)
- pointwise 详细归档：
  [SEG0_LAYER0_POINTWISE_ATTEMPTS.md](/home/ubuntu/chipyard/conference/mudnac_hybridmapper_collab_docs/SEG0_LAYER0_POINTWISE_ATTEMPTS.md)
