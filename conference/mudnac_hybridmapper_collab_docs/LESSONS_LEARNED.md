# Lessons Learned

日期：2026-03-24

本文件只记录已经被验证过、后续不应反复踩的经验教训。

## 1. Gemmini accumulator 地址语义不能想当然

- `bit31` 表示 accumulator address。
- `bit30` 表示 accumulate-on-write。
- `bit29` 表示 full-acc-row read selector。

因此：

- `3 << (ADDR_LEN - 2)` 不是“独立 B buffer”。
- 它表示：
  - accumulator address
  - 并且 accumulate-on-write = 1

直接后果：

- `mvin2` 到这类地址时，如果目标 acc 行不是干净零值，旧值会被一并累加。
- `copy_explicit_cross_1kb_interleaved_b_mvin2` 失败而 `..._clean` 通过，证明问题在这里，而不在 shared-spad 翻译本身。

工程准则：

- 若手写路径里希望 `mvin2` 表现为“纯 load”，先初始化目标 acc 行。
- 不能把 `mvin2` accumulator 地址当作“和 bit30=0 完全无关的独立窗口”。

## 2. `gemmini_fence()` 不是 ReRoCC manager 可见的 completion barrier

- `gemmini_fence()` 只是普通 RISC-V fence。
- 在 ReRoCC manager 模式下，真正用于 manager-visible completion 的是：
  `rr_fence(cfg_id)`

直接后果：

- 手写 explicit overlap 路径里，只靠 `gemmini_fence()` 不能证明：
  - `A mvin` 已经对后续 `B mvin2` 可见
  - `B mvin2` 已经对后续 `mvout` 可见

工程准则：

- 只要问题涉及 manager ownership / overlap / completion，优先考虑 `rr_fence(cfg_id)`。
- 不要把 `gemmini_fence()` 和 `rr_fence(cfg_id)` 混为一谈。

## 3. shared-spad `1KB` interleaved 页表设计本身没有被证明有问题

已被验证通过的路径包括：

- generic `mvin` 访问 interleaved shared-spad
- `mvin2_clean` 访问 interleaved shared-spad
- standard WS resadd
- serialized explicit resadd
- pointwise `J=128`

因此：

- 不能再把“interleaved 页翻译坏了”作为默认解释。
- 不能再把“跨 tile shared-spad 访问必须禁止”作为默认修复方向。

工程准则：

- 先查软件 contract：
  - VA 布局
  - PTE 安装顺序
  - manager/page placement contract
- 最后才考虑硬件假设。

## 4. shared-spad xlate 的软件 contract 必须显式收尾

- xlate table 不会自己恢复默认状态。
- `cfg` / `range` / `flush` / `reset` 都要显式做。
- case-local mapping 之间不能默认“天然隔离”。

直接后果：

- pointwise `J=128` 主线的真实根因不是硬件，而是：
  chunk-bias VA 覆盖进了 `C` region 的 VA 范围，后续 `C` 的 PTE 安装把 bias PTE 覆盖掉了。

工程准则：

- 每个 case 单独验证 installed xlate set 的 VA overlap。
- 不要只看“全局 region 命名上不冲突”；要看“当前实际安装集合是否冲突”。

## 5. `MAX_BLOCK_LEN` 不是“整层 J 的合法性上限”

- `MAX_BLOCK_LEN` 来自 DMA 单次宽度上限。
- 它约束的是单段传输的 block 宽度，不等于：
  “整个 op 的 `J > MAX_BLOCK_LEN` 就非法”。

直接后果：

- pointwise `J=128` 曾经失败，并不意味着硬件天然不支持大 `J`。
- 后续闭环已经证明：
  大 `J=128` 失败的主根因是软件 VA / PTE 覆盖，而不是 `MAX_BLOCK_LEN`。

工程准则：

- 遇到 `J > MAX_BLOCK_LEN` 的失败，不要先下硬件结论。
- 先查：
  - 分段 load/store 行为
  - chunk 边界 drain
  - case-local VA / PTE 布局

## 6. 显式 overlap 序列必须写成完整依赖链

已验证通过的经验是：

- 若手写 explicit resadd，需要把依赖链写完整：
  `A mvin -> rr_fence -> B mvin2 -> rr_fence -> mvout`

只补其中一半不够：

- 只补 `B -> mvout` 不够。
- `A -> B` 同样要保证 manager-visible completion。

工程准则：

- 写 explicit overlap 时，先把数据依赖图画清楚。
- 每一条真正依赖链都要有对应的 manager-visible completion。

## 7. standard WS path 优先级高于手写 workaround

baremetal 已证明：

- standard WS resadd 在当前 interleaved shared-spad 设计下是可用的。
- dual-manager split 也已有正向证据。

因此：

- 如果 standard path 已经满足语义，应优先使用 standard path。
- 手写 explicit workaround 只能在必要时存在，而且必须严格遵守本文件的语义约束。

## 8. FireSim 运行流程的工程教训

- manager 环境必须从：
  [sims/firesim](/home/ubuntu/chipyard/sims/firesim)
  下 `source sourceme-manager.sh --skip-ssh-setup`
- 长任务必须通过：
  [firesim-tmux-run.sh](/home/ubuntu/chipyard/scripts/firesim-tmux-run.sh)
  或
  [firemarshal-tmux-run.sh](/home/ubuntu/chipyard/scripts/firemarshal-tmux-run.sh)
  在 `tmux` 中执行
- FireSim 固定流程是：
  `launchrunfarm -> infrasetup -> runworkload -> terminaterunfarm`
- manager exit code 不可靠，必须看 guest `uartlog`。
- Linux 启动阶段只要 heartbeat 在涨、没有明确错误，就继续等，不要误判成卡死。
- 一旦已经拿到决定性结果，就先停 runworkload / terminaterunfarm，不要让 F2 空转。

## 9. `spot` 失败不等于配置错误

当前已验证：

- `f2.6xlarge spot` 在当前 AWS 容量下持续报 `insufficient capacity`。

因此：

- 当前 `spot` 不可用的结论已经成立。
- 这不是 workload / AGFI / FireSim config 本身的错误。

工程准则：

- `spot` 如果持续是 capacity fail，就先切回 on-demand 完成 correctness closure。
- 不要在 capacity fail 上浪费调试时间。

## 10. 后续调试时，默认优先级

出现新问题时，优先级应是：

1. 先查软件 contract 是否自洽：
   - 地址语义
   - manager completion
   - page placement
   - PTE 安装顺序
2. 再查 runtime 是否误用了 Gemmini ISA。
3. 最后才考虑硬件问题。

当前已经明确不应反复重开的假设：

- shared-spad interleaved 设计天然错误
- `mvin2` 天然不能读 interleaved shared-spad
- pointwise `J=128` 天然不被支持
- 必须改单 manager 或改硬件才能闭环

## 11. 不要重新回到 guessed stride

当前已经验证并落地的方向是：

- HybridMapper 导出 `tensorStride`
- runtime loader 读取 `tensorStride`
- descriptor builder 用 `tensorStride + tensorSize` 做一致性校验

直接后果：

- `stride` 不应再靠 channel 数或经验值临时猜出来。
- 当前 layer-0 stall 也已经不能再简单归因成“只是 stride 没导出”。

工程准则：

- 后续若改 `HybridMapper -> YAML -> runtime` 契约，优先保持显式 stride 语义。
- 若 layer 还依赖 runtime 推导 `pad`，要继续用 tensor size / shape 关系交叉校验。
- 不要把 guessed stride 当成长期方案。
