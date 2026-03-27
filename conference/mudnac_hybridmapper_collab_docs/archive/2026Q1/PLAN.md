# Pipeline Runtime Tactical Plan

日期：2026-03-26

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

- host closure gate 已闭环。
  - `ours2 / gemini2 / tangram2` 当前都已通过 CPU golden 与 host `fpga` backend。
  - host runtime 当前不再是 active blocker。

- HybridMapper -> pipeline-runtime 的 pre-orchestrated contract 已落地。
  - pipeline YAML 已直接导出 segment 资源与逻辑 shared-spad 布局。
  - runtime 执行期只兑现 alias VA / vpage / physical page / manager xlate。

- `spot` 试跑已经完成。
  - 当前结论是 AWS 容量不足。
  - 在没有新容量信号之前，不继续在 `spot` 上耗时。

- 当前 active 主线已推进到 Linux packaging / FireSim F2。
- 当前不应假定还有 active run farm。

- 2026-03-26 新增一个必须与“当前卡死修复”分开看的未来负载结论：
  - 当前单 hart 并不具备“同时直接驱动多个 Gemmini stage issue 流”的能力
  - 当前软件栈里，单 hart 实际只有：
    - `1` 条 Gemmini live route lane
    - `1` 条 DMA live route lane
  - 当前 runtime 的 `cfg` 散列方式最多只提供 `8` 组无冲突 stage-context
  - 因此“一个 hart 管一个 action，并发多个 stage / 多个 Gemmini core”不是当前实现已具备的能力

- 2026-03-26 当前 Linux/F2 最新 live boundary 已经比旧的 `matmul-args` 结论更深。
  - 已验证：
    - Linux boot 正常
    - `S99run` 正常
    - runtime outer init 正常
    - `segment=0` 的 `alloc-pages / exec-bind` 新日志完整出现
    - `pointwise-inner ... matmul-call-enter`
    - `matmul-os spaddrs-dc`
    - `matmul-os-biascfg-enter`
    - `matmul-os-biascfg-pre-ld`
  - 当前新的 active boundary：
    - 已进入 bias `config_ld`
    - 但始终未看到 `matmul-os-biascfg-post-ld`
  - 因此下一步主线已经不再是
    `matmul-args -> tiled_matmul_nn_stride_auto()` 之间的外层缩圈，
    而是：
    1. 继续把 `biascfg-pre-ld -> post-ld` 之间的软件/ISA状态钻透
    2. 对照 baremetal runtime-style case 与 Linux/runtime 上下文差异
    3. 解释为什么 baremetal 能穿过，而 Linux/F2 会在这里形成 functional deadlock

## 当前执行主线

### A. 当前卡死问题主线

目标：

- 继续定位 Linux/F2 guest runtime functional deadlock
- 不把数值问题、未来扩展问题与当前卡死问题混在一起

固定顺序：

1. 先静态审计 `pipeline-runtime` 与 baremetal/Gemmini 正例的 runtime-specific diff
2. 再做最快的 baremetal targeted repro
3. 只有在新增证据已经足够缩圈后，才 replay Linux/F2

当前重点：

- `pointwise-inner -> tiled_matmul_nn_stride_auto -> OS bias/config_ld` 这一段
- ReRoCC scope acquire / fence / release 的实际时序
- 当前 xlate/range/PTBR/cache 状态与 baremetal runtime-style case 的差异

输出要求：

- 继续保持深层 marker
- 对关键 helper 维持 `pre-call / post-call` 成对断点
- 卡死前必须先抓 live `uartlog/heartbeat` 再 terminate run farm

### B. 面向未来负载的并发优化主线

目标：

- 为未来 `64` core / `6` action / `6` hart 的动态调度目标准备软件设计边界
- 明确哪些能力当前没有，哪些需要软件重构，哪些需要硬件加资源

固定顺序：

1. 先把当前 ReRoCC 约束写死：
   - `cfg` 容量 != `opcode` 路由容量
   - 单 hart 当前只有 `1` 条 Gemmini lane
   - 当前 `cfg` 散列只支持 `8` 组无冲突 stage-context
2. 软件近期方案：
   - 若要真正并行多个 stage，继续优先使用多 hart / 多 stage worker
   - 不把“单 hart 管整个 action”误当成当前已支持的高并发方案
3. 软件中期方案：
   - 把 `cfg` 分配改成显式 allocator，而不是 `stage_id` 取模
   - 让 Gemmini 发射路径支持多个 custom opcode，而不是固定 `custom3`
4. 硬件长期方案：
   - 若最终坚持单 hart 高并发驱动多 stage，需要增加 `rropc` / route lane 资源
   - 单纯增加 Gemmini manager/core 数量，不会自动解决单 hart 路由竞争

### 0. 先做 baremetal root-cause loop，不再优先消耗 Linux/F2 replay

- 目标：
  - 复原 Linux/F2 中首个真实 `bias mvin(D -> acc)` 的 issue 边界
  - 尽快判断问题是：
    - 单条 `k_MVIN` 本身
    - shared-spad alias/xlate + `D -> acc`
    - 还是必须叠加 pointwise/OS 更大上下文
- 固定复原参数：
  - 外层 pointwise：
    - `I=256 J=64 K=256 fallback=OS`
  - 首个 local tile：
    - `I=32 J=8 K=32`
    - `pad_I=0 pad_J=0 pad_K=0`
    - `stride_A=256 stride_B=256 stride_D=256 stride_C=256`
    - `act=RELU`
    - `repeating_bias=1`
    - `D_stride=0`
  - 首个 bias issue：
    - `sp=0x80000000`
    - `cols=16`
    - `rows=8`
    - `rs2=0x8001080000000`
- 实施位置：
  - [rerocc_lc_resadd_explicit_interleaved.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/bareMetalC/learn-gemmini/rerocc_lc_resadd_explicit_interleaved.c)
- 本轮优先新增的 baremetal case：
  - `contiguous` 首个 bias `mvin` 最小复现
  - `interleaved` 首个 bias `mvin` 最小复现
- 输出要求：
  - 默认关闭 `stdio` 缓冲
  - 打印 alias region 页映射
  - 打印相关 xlate PTE
  - 打印首条 issue 的 `rs1/rs2/sp/cols/rows`
  - 打印首段 bias 数据窗口，便于后续在 baremetal 继续复原
- 只有在 baremetal 仍不足以解释现象时，才回到 Linux/F2 做确认 replay。

### 0b. baremetal stall-only 已通过后，主线切到 runtime-specific delta 缩圈

最新状态已经变成：

- baremetal/F2 stall-only replay 已通过：
  - contiguous 首个 bias `mvin`：PASS
  - interleaved 首个 bias `mvin`：PASS
- 因此后续主线不再是“继续证明首个 bias `mvin` 会不会卡”
- 后续主线改为“解释为什么 Linux/pipeline-runtime 在更完整的 orchestration 下会卡，而 baremetal 不会卡”

接下来的排查顺序固定为：

1. 对照 baremetal case 与 pipeline-runtime 在首个 pointwise issue 前的状态准备差异
2. 核对 runtime 构造的 segment 私有 VA 区间、tensor/buffer 布局、shared scratchpad xlate base
3. 核对 runtime 安装到各 Gemmini core 的页表内容、覆盖范围、生命周期
4. 核对 runtime-only buffer 类型
   - single buffer
   - double buffer
   - ring buffer
   - shared buffer
5. 只在需要确认某条 runtime 差异是否足以解释 Linux stall 时，才回到 Linux/F2 replay

### 0c. 最新 runtime-specific delta 下一步固定动作

基于 `2026-03-25--13-31-38` 这轮 replay，下一步顺序固定为：

1. 在 `pointwise-inner matmul-args` 后立刻加短 marker
   - `matmul-call-enter`
   - `matmul-call-return`
2. 把长的 `matmul-args` 行拆成更短的 2-3 行
   - 避免再次因为长行/缓冲导致边界模糊
3. 保持 `alloc-pages / exec-bind` 逐页日志不动
   - 这些日志已经证明当前页分配与绑定信息可以在 guest 中稳定输出
4. 下一轮 replay 只回答一个问题：
   - stall 是在 `tiled_matmul_nn_stride_auto()` 调用前
   - 还是已经进入 Gemmini helper 但第一条 helper marker 未刷出
5. 若 boundary 仍稳定停在 call 前后之间，再转入更窄的软件 diff：
   - `alloc_pages_from_order()` 页序变化
   - `spm_pte` / xlate 安装顺序
   - `stage_tensor_exec_addr()` 生成的地址与旧 replay 对比

## 下一轮直接执行队列

### 1. 先做 Linux packaging 预检查，不先动 runtime 语义

- 进入：
  `/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests-coupleddma/workload`
- 执行：
  `HOST_INIT_CHECK_ONLY=1 bash host-init.sh`
- 核查重点：
  - overlay 中将要打包的 `rerocc_pipeline_runtime-linux` 是否来自当前 head
  - `bertmini` 的 YAML / bin / cache 是否都来自 canonical artifact 目录
  - guest wrapper 参数是否仍然匹配新的 pre-orchestrated contract
- 退出条件：
  - staging 所需文件完整
  - 不存在旧 overlay 残留或路径漂移

### 2. 再做一次完整 staging 刷新

- 执行：
  `bash host-init.sh`
- 核查重点：
  - Linux binary 重新编译成功
  - overlay 中 runtime binary 仍保留预期进度字符串
  - `pipeline_mapping.*.{ours2,gemini2,tangram2}.yaml` 都被同步进 overlay
- 退出条件：
  - host packaging 自洽
  - guest 侧拿到的是当前 contract 下的新 artifacts，而不是旧缓存

### 3. FireMarshal build/install 只做镜像与 overlay 闭环

- 通过：
  [firemarshal-tmux-run.sh](/home/ubuntu/chipyard/scripts/firemarshal-tmux-run.sh)
  执行
- workload 固定为：
  [rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime.json](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests-coupleddma/workload/rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime.json)
- 固定顺序：
  - `marshal build`
  - `marshal install`
- 退出条件：
  - image / rootfs 中的 pipeline-runtime 产物来自当前 head
  - 没有旧镜像残留继续污染 guest

### 4. FireSim F2 replay 只验证 guest bring-up 与最终行为

- 通过：
  [firesim-tmux-run.sh](/home/ubuntu/chipyard/scripts/firesim-tmux-run.sh)
  执行
- 固定顺序：
  - `launchrunfarm`
  - `infrasetup`
  - `runworkload`
  - `terminaterunfarm`
- 固定策略：
  - 先用 on-demand `f2.6xlarge`
  - 不因 Linux 早期静默就提前判死
  - 不信 manager exit code，优先看 `uartlog / heartbeat.csv / tmux pane log`
- 退出条件：
  - 若 PASS，拿到 `BERTMINI_PIPELINE_RUNTIME_PASS`
  - 若 FAIL，先把问题收敛到 guest-only setup 还是 guest runtime semantics

### 5. 若 F2 失败，排查顺序固定

- 第一优先级：
  - overlay / staged artifact 版本不一致
  - guest wrapper / CLI 参数不匹配
  - guest HugeTLB / pagemap / DMA pinning
- 第二优先级：
  - Linux userspace DMA helper / ReRoCC helper 调用顺序
- 只有第三优先级才回看：
  - `prt_runtime.c`
  - `prt_schedule_action.c`
  - `prt_page_table.c`
  - `prt_gemmini_adapter.c`

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

### 3. 当前 runtime 主合同已在 host 路径闭环

- pipeline YAML 已成为编排期真相：
  - `segmentSpmPageSpan`
  - `bufferBinding*List`
  - stage `execBaseVPage`
  - stage `localSpm*List`
- runtime 现在按 action 级 contract 执行：
  - 独立 alias VA window
  - 连续 vpage interval
  - all-bank physical page 分配
  - page table / xlate 装载
- 因此下一阶段不再优先重开 host runtime 的 page placement 假设。

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

### Phase 1. 固化 regression gates

- 保留当前 baremetal workload 作为 shared-spad / resadd / pointwise 的回归门：
  [rerocc-lc-baremetal-coupleddma-explicit-interleaved-small.json](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/rerocc-baremetal-tests-coupleddma/workload/rerocc-lc-baremetal-coupleddma-explicit-interleaved-small.json)
- 后续只要改动下列任一语义，就必须回归 baremetal：
  - shared-spad xlate
  - explicit `mvin/mvin2/mvout`
  - resadd path
  - pointwise fallback path
- 保留 host closure 作为 Linux/F2 前的第一道回归门。

### Phase 2. 先做 Linux packaging gate

- 先执行：
  `HOST_INIT_CHECK_ONLY=1 bash host-init.sh`
- 再执行完整：
  `bash host-init.sh`
- 目标：
  - 确认 overlay 中的 binary 与 runtime artifacts 与当前 canonical 产物一致
  - 确认 guest wrapper 仍与新 contract 匹配
- 2026-03-25 进展：
  - check-only 已 PASS
  - 在 `sims/firesim` manager 环境里重新拿到 RISC-V Linux 工具链后，
    完整 `host-init.sh` 已 PASS
  - 新 guest binary 已按低噪声配置重编：
    `PIPELINE_RUNTIME_PROGRESS=0`
    `PIPELINE_RUNTIME_PROGRESS_RAW=0`
  - overlay 中的 `rerocc_pipeline_runtime-linux` 已刷新为本轮重编产物
  - 2026-03-25 本轮又补了一层 marker-only 细粒度日志，并重新通过：
    - `PIPELINE_RUNTIME_GEMMINI_PHASE=1`
    - `PIPELINE_RUNTIME_ONLY_MARKER=1`
  - 新增日志重点覆盖：
    - `xlate-install`
    - `bind-plan`
    - `bind-stage`
    - `bind-topology`
    - `pointwise-inner ... matmul-args`

### Phase 3. FireMarshal build/install gate

- 用当前 fresh artifacts 重新做 FireMarshal `build/install`。
- 目标：
  - 确认 guest rootfs 中 staged 的 runtime binary / YAML / bin 文件来自当前 head
  - 不引入旧 overlay 残留
- 当前下一步：
  - 直接基于这轮 fresh overlay 进入 `marshal build -> marshal install`
  - 若 build/install 阶段发现 guest overlay 仍带旧 binary，再回头检查 FireMarshal cache
 - 2026-03-25 本轮进展：
   - `marshal build` 已 PASS
   - `marshal install` 已 PASS
   - 因此下一轮 replay 可以直接复用这版 refreshed workload；
     但按 FireSim 约束，切换前仍必须重新执行一次 `infrasetup`
   - 2026-03-25 当前补充进展：
     - `sp_tiled_matmul_os()` 入口处的 `spaddrs` 细化日志补丁已重新完成一轮 fresh guest 刷新
     - split-spaddrs 版本已经重新通过：
       - `HOST_INIT_CHECK_ONLY=1 bash host-init.sh`
       - `bash host-init.sh`
       - `marshal build`
       - `marshal install`
     - 所以下一轮 Linux/F2 replay 不需要再重做 packaging 语义改动，只需验证新的边界细分日志

### Phase 4. FireSim F2 replay gate

- 在 on-demand `f2.6xlarge` 上重放。
- 固定流程仍然是：
  `launchrunfarm -> infrasetup -> runworkload -> terminaterunfarm`
- 判定 live blocker 时：
  - 优先看 guest `uartlog`
  - 不信 manager exit code
  - 若真卡死，先回收 runfarm 再改代码
- 2026-03-25 replay 进展：
  - guest 已成功走到 `launching firemarshal workload run/command`
  - 已成功进入 bertmini method：
    `ours2`
  - refined replay 已进一步证明：
    - artifact validation 完整结束
    - runtime 已进入 `segment=0`
    - 已到：
      - `worker stage=0 subbatch=0 gemm-enter`
      - `conv-sync ... dispatch=pointwise`
      - `pointwise-chunk ... chunk-begin oc_beg=0 oc_tile=64`
  - 但在这个首个 chunk 的 `chunk-begin` 之后：
    - 额外等待超过 4 分钟
    - `heartbeat.csv` 继续从约 `9.09B` 推进到约 `11.83B`
    - 仍未出现 `chunk-drain / chunk-end / gemm-exit`
  - 因此本轮已按“compute-path stall 候选”处理并主动 terminaterunfarm
- 2026-03-25 本轮追加 replay 进展：
  - 新结果目录：
    `/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-25--04-53-51-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime`
  - boundary 已进一步推进到：
    - `pointwise-chunk ... subcall-begin`
    - `pointwise-inner ... matmul-begin I=256 J=64 K=256 fallback=OS`
  - 在 `matmul-begin` 之后：
    - `heartbeat.csv` 继续从约 `8.70B` 推进到约 `9.49B`
    - 仍未出现：
      - `pointwise-inner ... matmul-end`
      - `subcall-return`
      - `chunk-drain`
      - `chunk-end`
      - `gemm-exit`
 - 因此当前 active stall 已不再是 outer chunk wrapper，而更像是在或紧跟于
    `tiled_matmul_nn_stride_auto(...)` / 更深 Gemmini matmul phase
 - 2026-03-25 高细节 replay 进一步进展：
   - 新结果目录：
     `/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-25--08-04-25-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime`
   - 同一轮 guest 已连续越过：
     - `pointwise-inner ... matmul-begin`
     - `matmul-auto`
     - `tiled-enter`
     - `matmul-outer begin`
     - `matmul-inner begin`
     - `matmul-inner ptrs/shape/flags`
     - `tiled-pre-innercall`
     - `matmul-os-enter`
     - `matmul-os ptrs/shape/flags`
   - 最后一条稳定可见日志是：
     - `[gemmini-phase] matmul-os spaddrs A=0x0 B=0x7800 D=0x8000`
   - 在这之后仍未出现：
     - `matmul-os-after-bias`
     - `matmul-os-after-b`
     - `matmul-os-after-a`
     - `matmul-os-after-compute`
     - `pointwise-inner ... matmul-end`
   - 同期 `heartbeat.csv` 继续增长到约 `11.82B` target cycles
   - 因而当前 active boundary 已收敛到：
     - `sp_tiled_matmul_os()` 入口极早期
     - 更具体地说是在 `matmul-os spaddrs` 参数日志附近，早于 bias move-in 完成
 - 2026-03-25 下一轮 replay 目标：
   - 使用已完成 build/install 的 split-spaddrs guest
   - 重点检查：
     - `[graw] matmul-os-pre-spaddrs-ab`
     - `[gemmini-phase] matmul-os spaddrs-ab ...`
     - `[graw] matmul-os-post-spaddrs-ab`
     - `[graw] matmul-os-pre-spaddrs-dc`
     - `[gemmini-phase] matmul-os spaddrs-dc ...`
     - `[graw] matmul-os-post-spaddrs-dc`
     - `[prt-raw] matmul-os-after-bias`
   - 判定规则固定为：
     - 若没有 `post-spaddrs-ab`，则卡在 `spaddrs-ab` 日志本身
     - 若有 `post-spaddrs-ab` 但没有 `post-spaddrs-dc`，则卡在 `spaddrs-dc`
     - 若有 `post-spaddrs-dc` 但没有 `after-bias`，则转向 `D` bias move-in
     - 若已出现 `after-bias`，则继续追 `B/A` move-in 或 compute
 - 2026-03-25 run farm 状态：
   - 上一轮实例 `i-0d4390fd8b863e7a2` 已确认进入 `terminated`
 - 2026-03-25 新 replay 结果：
   - 新结果目录：
     `/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-25--08-48-52-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime`
   - split-spaddrs 版本已经证明：
     - `post-spaddrs-ab` 可见
     - `post-spaddrs-dc` 可见
     - `bias-config` 可见
     - `bias-loop` 可见
     - `pre-bias-mvin0` 可见
   - 当前最后一行停在：
     - `matmul-os bias-mvin0 bias_row=0 dram=...`
   - 因此新的 active boundary 不是 bias move-in 之后，而是：
     - 首个 `bias-mvin0` 参数日志本身
   - 下一轮补丁策略：
     - 不继续加大段普通 progress
     - 把 `bias-mvin0` 继续拆成更短的参数日志或 raw guard
     - 目标是区分：
       - 卡在 `bias_row/dram`
       - 卡在 `sp/blocks/cols/rows`
       - 还是已经真正进入首个 `gemmini_extended_mvin(...)`
 - 2026-03-25 最深 replay 结果：
   - 新结果目录：
     `/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-25--10-46-01-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime`
   - 新增最深 token：
     - `bm0c0`
     - `bm0ce`
     - `bias-mvin0-call-rs1`
     - `bias-mvin0-call-rs2`
     - `bm0ca`
   - 仍未出现：
     - `bm0cb`
     - `bm0c1`
     - `after-bias`
   - 因而当前断点已经收敛到：
     - 首个 bias `mvin` 的真实 `.insn r CUSTOM_0 ... k_MVIN` 发射点
   - 这一步已经足够深，不再优先继续消耗 Linux/F2 启动时间去切更细
   - 下一主线切换为：
     - baremetal 复原首个 bias `mvin(D -> acc)` issue
     - 用更快回归实验确认是否为这条指令本身卡住

### Phase 5. 若 guest/F2 失败，再回看 guest-only contract

- 优先检查：
  - guest userspace HugeTLB / pagemap / DMA pinning
  - guest wrapper 与 CLI 参数
  - overlay 中 artifact 版本不一致
- 只有 guest 证据直接指向 runtime 语义问题时，才重新打开 runtime 内部地址空间合同分析。
- 当前下一步：
  - 继续保留当前这一层高细节 Gemmini phase/raw marker
  - 不再整体压掉前段日志，但仍避免恢复大噪声普通 progress
  - Linux/F2 当前已经把断点压到真实 `k_MVIN` issue 点
  - 因此下一步不再优先继续加 Linux 启动轮次
  - 直接用这轮 Linux 现场参数去复原 baremetal：
    - `D` dram addr / stride
    - `acc` spad addr
    - `cols/rows`
    - repeating-bias / low_D / act flags
  - baremetal 目标是判断：
    - 首个 bias `mvin` 单独 issue 是否会卡
    - 是否必须连同某些 prior config/state 一起才会卡
  - baremetal 若主要目标是 stall triage，而不是数值校验：
    - 默认使用 `REROCC_STALL_DIAG_ONLY=1`
    - 不再先前置 pointwise/diag full golden
    - 优先直达 `bias_first_mvin_*` 相关 case
  - 一旦拿到足够边界证据，立即：
    - 抓取 `uartlog`
    - 抓取 `heartbeat.csv`
    - `terminaterunfarm`

### Phase 5.1 Runtime-Specific Diff Landing

- 本轮已完成：
  - 把 `pipeline-runtime` shared-spad allocator 修正为固定顺序 `all-bank interleave`
  - 不再使用每层 `local_page_idx` 轮转 bank 起点的旧顺序
  - 为 action 资源分配增加 `alloc-pages` marker
  - 为 stage exec-view 绑定增加 `exec-bind` marker
  - 默认构建与 `PIPELINE_RUNTIME_ONLY_MARKER=1` 构建都已本地通过
- 下一轮最小必要动作：
  - 重新把 runtime 带 marker 的版本打进 Linux workload
  - 跑 Linux/F2 replay
  - 首先看：
    - `alloc-pages`
    - `exec-bind`
    - `pointwise-inner ... matmul-args`
    - `bias-mvin0-call-rs1/rs2`
  - 若仍停在首个 bias `mvin`
    - 直接把该轮 `alloc-pages/exec-bind` 和 YAML stage-0 布局对照
    - 再决定是否继续下钻 PTBR / guest HugeTLB / DMA pinning

### Phase 6. 补全 HybridMapper -> runtime 元数据 contract

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

### Phase 7. 若 runtime 仍需手写 explicit path，必须遵守 baremetal 已验证的语义

- `mvin2` 到 accumulator 不是“独立 buffer load”。
- 若想得到“纯覆盖写入”的效果，必须先初始化目标 acc 行。
- `gemmini_fence()` 不是 ReRoCC manager 可见的 completion barrier。
- 需要 manager-visible completion 时，使用 `rr_fence(cfg_id)`。
- 显式依赖链若存在，必须类似：
  `A mvin -> rr_fence -> B mvin2 -> rr_fence -> mvout`
- 能回到 standard WS path 的地方，优先回到 standard WS path。

### Phase 8. 完整 closure

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
