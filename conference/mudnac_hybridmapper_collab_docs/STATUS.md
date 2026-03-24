# Pipeline Runtime Integration Status

日期：2026-03-24

先读：

- `HANDOFF.md`
- `PLAN.md`
- `LESSONS_LEARNED.md`

## 当前状态

- baremetal correctness gate 已闭环。
  - 结果目录：
    [2026-03-24--12-47-02-rerocc-lc-baremetal-coupleddma-explicit-interleaved-small-f2-rerocc-baremetal-explicit-interleaved-small](/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-24--12-47-02-rerocc-lc-baremetal-coupleddma-explicit-interleaved-small-f2-rerocc-baremetal-explicit-interleaved-small)
  - run log：
    [2026-03-24--12-47-02-runworkload-5IF794KZ0KCBVPPB.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-24--12-47-02-runworkload-5IF794KZ0KCBVPPB.log)
  - terminate log：
    [2026-03-24--13-34-01-terminaterunfarm-410L4U0QVSM9JZSH.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-24--13-34-01-terminaterunfarm-410L4U0QVSM9JZSH.log)
  - guest 最终输出：
    - `ALL_TESTS_PASS`
    - `*** PASSED *** after 27505375802 cycles`

- 当前 active blocker 已重新切回 Linux `pipeline-runtime` 主线。
  - baremetal 不再是 blocker。
  - 后续任何 runtime 修改，只要影响 shared-spad / resadd / pointwise 路径，都应回归这份 baremetal gate。

- 当前不应假定仍有 active run farm。
  - 最近一轮 on-demand 实例已经 terminate。

- `spot` 试跑已经给出结论，但当前不可用。
  - runtime config：
    [config_runtime_f2_rerocc_lc_baremetal_explicit_interleaved_small_spot.yaml](/home/ubuntu/chipyard/sims/firesim/deploy/config_runtime_f2_rerocc_lc_baremetal_explicit_interleaved_small_spot.yaml)
  - 结论：
    AWS 当前对 `f2.6xlarge spot` 持续返回 `insufficient capacity`
  - 当前判断：
    不是 workload / AGFI / FireSim manager 配置错误。

## 已闭环问题

### 1. pointwise `J=128` 问题已闭环

- 现象：
  - 大 `J=128` pointwise case 曾经系统性失败。
- 根因：
  - baremetal harness 的 chunk-bias 虚拟页落进了 `C` region 的 VA 覆盖范围。
  - 后续 `spm_xlate_map_region(c_region)` 覆盖了 bias PTE。
- 修复：
  - 把 chunk-bias vpage 移出 `C` 的 VA 范围。
  - 增加按 case 的 xlate-layout 校验。
- 结论：
  - 这是软件页表 / VA 布局错误。
  - 不是 RTL 问题。
  - 不是 shared-spad 跨 tile 访问硬件问题。
  - 不是 `MAX_BLOCK_LEN` 导致 `J=128` 本身非法。

### 2. `copy_explicit_cross_1kb_interleaved_b_mvin2` 已闭环

- 现象：
  - `mvin2_clean` PASS，但 `mvin2` FAIL。
- 根因：
  - `3 << (ADDR_LEN - 2)` 对应的 accumulator local address 不是“独立 B buffer”。
  - 它包含 `accumulate-on-write` 语义。
  - 之前没有先初始化目标 acc 行，导致旧值被一并累加。
- 修复：
  - 先用 bit30=0 的 acc 写地址显式写入零值。
  - 再用 `mvin2` accumulate-on-write。
  - 最后从 bit30=0 视图 `mvout`。
- 结论：
  - 根因是 accumulator address / acc-row init 语义。
  - 不是 generic `mvin2` 读取 interleaved shared-spad 失败。

### 3. `resadd_explicit_cross_1kb_interleaved` 已闭环

- 现象：
  - serialized explicit PASS，standard WS PASS，但 no-fence explicit FAIL。
- 根因：
  - ReRoCC manager 模式下，手写 explicit overlap 序列缺少完整 completion chain。
  - 只补 `B -> mvout` 不够，`A -> B` 同样需要 manager-visible completion。
- 修复：
  - 使用完整依赖链：
    `A mvin -> rr_fence -> B mvin2 -> rr_fence -> mvout`
- 结论：
  - 根因是 manager-visible completion 语义。
  - 不是 standard WS resadd 不可用。
  - 不是 shared-spad interleaved 设计本身错误。

## 已确认非根因

- 不是 shared-spad `1KB` interleaved 页表设计错误。
- 不是 `mvin2` 读 interleaved shared-spad 普遍坏掉。
- 不是 Gemmini RTL / shared-spad 硬件需要修改。
- 不是 pointwise `J=128` 主线回归。
- 不是 `MAX_BLOCK_LEN` 把整层 `J > MAX_BLOCK_LEN` 直接判为非法。
- 不是必须把多 manager 路径降级成 single manager。

## 当前活跃主线

- 当前最值得继续推进的是 Linux `pipeline-runtime` 主线：
  - 保持 no CPU fallback。
  - 保持 no hardware change。
  - 保持 shared-spad `all-bank / 1KB interleaved / multi-manager` 设计目标。

### 当前主怀疑

- 当前最强软件侧怀疑，仍然是 runtime 自己的 page placement / manager contract：
  - fixed-weight 页放置已经更接近多 manager 视图。
  - 但 stage-local entry/export tensor 页放置、exec-view rebase、manager binding 仍可能残留 `stage_acc` 偏置。
  - 这会让 Linux runtime 中的多 manager 负载划分与地址空间管理不一致。

### 当前改进方向

1. 统一 stage-local page placement / manager contract
   - 统一 fixed-weight、entry/export、shared canonical 页分配语义
   - 保持 shared-spad alias range 连续、且与 DRAM 不重叠
   - 保持每个 Gemmini manager 都能独立翻译并访问这段连续 alias VA
2. 收紧 HybridMapper -> runtime 元数据契约
   - `stride` 不再猜
   - `tensorStride` 导出和消费必须持续保持一致
   - `pad` 若仍在 runtime 推导，需继续用 size/shape 关系校验
3. 优先回归标准 Gemmini 语义
   - standard WS path 优先
   - 手写 explicit path 只能在必要时保留，并严格遵守 baremetal 已验证语义

## 文档索引

- handoff 摘要：
  [HANDOFF.md](/home/ubuntu/chipyard/conference/mudnac_hybridmapper_collab_docs/HANDOFF.md)
- 当前计划：
  [PLAN.md](/home/ubuntu/chipyard/conference/mudnac_hybridmapper_collab_docs/PLAN.md)
- 经验教训与接口语义：
  [LESSONS_LEARNED.md](/home/ubuntu/chipyard/conference/mudnac_hybridmapper_collab_docs/LESSONS_LEARNED.md)
- pointwise `J=128` 详细尝试归档：
  [SEG0_LAYER0_POINTWISE_ATTEMPTS.md](/home/ubuntu/chipyard/conference/mudnac_hybridmapper_collab_docs/SEG0_LAYER0_POINTWISE_ATTEMPTS.md)
- 更早的 stall 取证归档：
  [SEG0_LAYER0_STALL.md](/home/ubuntu/chipyard/conference/mudnac_hybridmapper_collab_docs/SEG0_LAYER0_STALL.md)
