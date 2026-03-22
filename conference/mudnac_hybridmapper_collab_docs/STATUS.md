# Pipeline Runtime Integration Status

日期：2026-03-22

## 当前目标

- 在不修改 model 定义文件、不破坏 HybridMapper 原有行为的前提下，跑通 `bertmini` 的 `pipeline-runtime` FireSim F2 闭环。

## 固定约束

- 保持当前 single-layer-stage 合同，不实现 fake multi-layer-stage 语义。
- `pipeline-runtime` 只消费新接口；layer mapping 继续使用 local zero-based SPM 地址，由 runtime 在执行期 rebase。
- 当前目标固定为 `subbatch_size=1`。
- FireSim 必须严格走：
  - `marshal build`
  - `marshal install`
  - `launchrunfarm`
  - `infrasetup`
  - `runworkload`
  - `terminaterunfarm`
- 不手工编辑 `sims/firesim/deploy/workloads/*`。
- F2 只使用 `f2.6xlarge`。
- `bertmini` 调试路径禁止 CPU fallback。
  - 不允许再把 pointwise / resadd / 其他 runtime 路径切到 CPU 规避问题。
  - 已经存在的 CPU fallback 只能作为历史定位线索，后续必须逐项根修复并移除。
- Linux 启动阶段，只要没有明确报错，就一直等待。
  - 不能仅凭 `uartlog` 静默或 `heartbeat.csv` 增长缓慢就判定 boot 失败。
  - 只有出现 `Simulator deadlock detected`、`*** FAILED ***`、kernel panic / Oops、userspace 明确 FAIL，才判定失败。
- Gemmini ISA / 软件语义优先参考：
  - [README.md](/home/ubuntu/chipyard/generators/gemmini/README.md)
  - [gemmini-rocc-tests/include](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/include)
  - [gemmini-rocc-tests/imagenet](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/imagenet)

## 已确认事实

- host 闭环是通的：
  - `METHODS=ours2 BATCH=1 SKIP_EXPORT=1 SKIP_BUILD=1 bash generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/scripts/run_bertmini_host_closure.sh`
  - 结果：`BERTMINI_HOST_CLOSURE_PASS`
- `subbatch_size=1` 是当前真实配置：
  - 默认值来自 [prt_yaml_loader.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_yaml_loader.c)
  - 当前 `bertmini` pipeline mapping YAML 未显式覆盖 `subBatchSize`
- hugepage 配置当前为：
  - `HUGETLB_PAGES=1`
  - `Hugepagesize=2048 KiB`
  - 总保留量 `2 MiB`
  - 入口脚本在 [run_rerocc_pipeline_runtime_bertmini.sh](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests-coupleddma/workload/overlay/root/rerocc-linux-tests/run_rerocc_pipeline_runtime_bertmini.sh)
- 旧的 host-backed misaligned full-page DMA blocker 已不再是主问题：
  - Linux small regression 已通过 `dma_dram_to_shared_misaligned_fullpage`
  - 活跃的 Linux `dram <-> spm` 路径已收敛到 [prt_dma.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_dma.c) 的 bounce-buffer helper
- 大 YAML 解析风险已增加派生 cache 缓解，且该路径已在 FPGA 上验证通过：
  - 运行时加载逻辑在 [prt_gemmini_artifacts.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_gemmini_artifacts.c)
  - staging 生成逻辑在 [host-init.sh](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests-coupleddma/workload/host-init.sh)
  - 生成脚本在 [generate_gemmini_mapping_cache.py](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/scripts/generate_gemmini_mapping_cache.py)

## 疑似硬件问题与当前处理

### 1. canonical 1x1 pointwise fallback 的 WS 路径会挂

- 旧 live UART 已证明 canonical pointwise fallback 会进入 `gemmini_loop_ws`，并停在 `loop-ws run begin` 之后。
- 最强 RTL 嫌疑点仍在 [LoopMatmul.scala](/home/ubuntu/chipyard/generators/gemmini/src/main/scala/gemmini/LoopMatmul.scala)：
  - `val ldb_ahead = io.ldb_completed || io.ld_kb > k || (io.ld_ka === k && io.ld_j > j)`
  - 该条件和 `lda_ahead` 不对称，疑似应为 `io.ld_kb === k`
- 当前软件处理：
  - 在 [prt_gemmini_adapter.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_gemmini_adapter.c) 中，canonical 1x1 pointwise fallback 若原本请求 `WS`，现在强制改走 `OS`
  - `conv_call_for_manager_nb()` 已与 sync 路径对齐，不再直接绕过该保护
- 当前应观察的日志关键字：
  - `conv-nb ...`
  - `pointwise-matmul-fallback ... requested_type=WS fallback_type=OS reason=avoid-loop-ws`

### 2. resadd 的 `gemmini_loop_ws(... is_resadd=1)` 也疑似有硬件问题

- 2026-03-21 最新纯 Gemmini run 已把 pointwise 旧风险点全部穿过去，并稳定推进到：
  - `segment=5 init ...`
  - `segment=5 begin stages=1 sinks=1 subbatch_size=1 target_batch=16 target_subbatch=16`
  - `worker stage=0 ready entries=2 exports=1 isolate_pairs=0 shared_pairs=0 acc=0 dma=2 tiles=2`
  - `worker stage=0 subbatch=0 begin op=2 acc=0 dma=2 tiles=2`
- 此后：
  - `uartlog` 尾部长期固定在这条 `op=2` begin
  - `heartbeat.csv` 持续增长
  - 没有 `fallback_type=CPU`
  - 没有 `dispatch=cpu-fallback`
  - 没有 Linux panic / Oops / `*** FAILED ***`
- 这说明当前最新 blocker 已从 pointwise 转移到原始 resadd Gemmini 路径本身。
- 具体代码链路是：
  - [prt_gemmini_adapter.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_gemmini_adapter.c)
    `resadd_issue_no_fence()`
  - [gemmini.h](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/include/gemmini.h)
    `sp_tiled_resadd()`
  - `gemmini_loop_ws(... is_resadd=1)`
- 旧历史文档 [2026Q1_history.md](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/docs/archive/2026Q1_history.md)
  也已经单独怀疑过 `gemmini_flush -> tiled_resadd_auto -> gemmini_fence` 这一段。
- 历史上试过的大尺寸 / spatial split resadd CPU fallback，已被用户明确禁止继续作为 bertmini workaround。
- 当前要求：
  - 逐一定位 resadd 在 Gemmini 上的根因
  - 不允许再用 CPU fallback 绕开
  - 修复必须保持在 Gemmini 上

### 3. pointwise 旧卡点已被 hot-log thinning + zero-repack 直接路径显著绕开

- 当前 pointwise 相关软件处理已经更新为：
  - [prt_progress.h](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/include/prt_progress.h)
    新增 hot-path log tier，默认关闭
  - [prt_dma.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_dma.c)
    和 [prt_runtime.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_runtime.c)
    将高频进度日志下沉到 hot tier
  - [prt_gemmini_adapter.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_gemmini_adapter.c)
    中 canonical pointwise fallback 改为 direct-strided zero-repack Gemmini 路径
  - canonical pointwise 的 split-OC 也已切成 direct-strided Gemmini 子视图，不再默认 `pack/repack`
- 这些改动的实测结果是：
  - 已稳定推进通过 `segment=0`
  - 已稳定推进通过 `segment=1`
  - 已稳定推进通过旧高风险点 `segment=2 / subbatch=14`
  - 已稳定推进通过 `segment=3` 的 `stage=0 + stage=1` 双阶段执行
  - 已稳定推进通过 `segment=4`
- 当前 run 在进入 `segment=5 / op=2` 之前没有观察到 CPU fallback 标记。

### 4. 当前最新卡点：`segment=5 / stage=0 / subbatch=0 / op=2 (RESADD)`

- 最新 run 是：
  - `launchrunfarm`: `bertmini-launch7`
  - `infrasetup`: `bertmini-infrasetup7`
  - `runworkload`: `bertmini-runworkload7`
- 活跃实例曾是：
  - instance `i-0eb51ac0c790295bd`
  - private IP `192.168.1.197`
  - public IP `44.246.224.16`
- 该 run 在明确卡住后已按纪律回收：
  - `terminaterunfarm` session: `bertmini-terminate7`
  - manager log:
    [2026-03-21--16-58-40-terminaterunfarm-51BZWQ9K7NT06BDA.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-21--16-58-40-terminaterunfarm-51BZWQ9K7NT06BDA.log)
  - AWS 状态已进入 `shutting-down`
- 当前判断优先级：
  - 这不是 Linux boot 问题
  - 也不是 pointwise 的旧 UART backpressure 问题
  - 第一怀疑点已经收敛到原始 resadd `gemmini_loop_ws(... is_resadd=1)` 路径
- 当前本地工作树中，已经有一个“仅 Gemmini、不切 CPU”的草案修复：
  - 文件：
    [prt_gemmini_adapter.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_gemmini_adapter.c)
  - 内容：
    - 在 RISC-V 上把 resadd 的运行类型硬钉为 Gemmini
    - 尝试把 `resadd_issue_no_fence()` 从 `sp_tiled_resadd()/gemmini_loop_ws` 改为显式 `mvin/mvin2/mvout`
  - 状态：
    - 只是本地草案
    - 还没有重新 build / install / rerun
    - 不能视为已验证修复

## 当前仍存在的重排点

- 这里的“重排”只记算子为了喂 Gemmini 而做的 `pack/repack`。
- 不把 Linux `dram <-> spm` misaligned DMA 的 bounce buffer 混进来；那条是 DMA 正确性保护，不是算子布局优化。

### 1. 当前 bertmini live 路径上最重要的重排点

- canonical pointwise fallback 的 chunked `OC` 路径已经改成 direct-strided zero-repack：
  - 不再默认分配 `w_pack / o_pack`
  - 直接对原始 `weights + oc_beg` / `output + oc_beg` 做 Gemmini stride-aware 调度
  - 这条路径已经在 `run7` 中把旧 pointwise 风险点穿过去
- 因此 pointwise chunked zero-repack 不再是“待做项”，而是“已落地、待继续扩到其他 split 路径”的现状。

### 2. 通用 Gemmini split conv 里仍保留的重排点

- `conv oc-split` 默认路径仍在做 `pack/repack`：
  - 会为每个 tile 分配 `w_pack / b_pack / o_pack`
  - 结束后再把 `o_pack` 写回大输出张量
  - 位置：
    [prt_gemmini_adapter.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_gemmini_adapter.c)
- `conv spatial-split` 仍在做输入裁剪和输出回填：
  - `in_pack`：把 tile 对应的输入窗口和 halo 拷到连续小 buffer
  - `out_pack`：Gemmini 输出 tile 后再 scatter 回原始输出
  - 位置：
    [prt_gemmini_adapter.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_gemmini_adapter.c)
- 这两类不一定是当前 bertmini 停点，但都属于后续要消掉的历史重排路径。

### 3. 当前 resadd 问题不属于 `pack/repack`

- `segment=5` 当前最新 blocker 是 resadd 原始 Gemmini primitive 本身。
- 这条问题属于：
  - Gemmini `loop_ws is_resadd` 正确性 / 前进性问题
- 不属于：
  - pointwise `pack/repack`
  - DMA misalignment bounce-buffer
- 所以后续处理顺序不要混淆：
  - resadd stall 要先做 Gemmini 根修复
  - 然后再继续做剩余 split conv 去重排

### 4. 当前优化优先级

- P0：
  - 先修通 `segment=5` 的 resadd Gemmini 路径
- P1：
  - 再修通通用 `conv oc-split` 的 direct-strided 路径，替代默认 `pack/repack`
- P2：
  - 最后处理 `conv spatial-split`
  - 这条最难，因为不只是 stride，还涉及 tile 输入窗口裁剪和 halo 语义

## 当前代码与镜像状态

- 主要工作文件：
  - [prt_progress.h](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/include/prt_progress.h)
  - [prt_dma.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_dma.c)
  - [prt_runtime.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_runtime.c)
  - [prt_gemmini_adapter.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_gemmini_adapter.c)
  - [prt_gemmini_artifacts.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_gemmini_artifacts.c)
- 最新已完成的镜像构建：
  - build log:
    [rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-build-2026-03-21--16-10-19-T6AGVCRBO6ZV0BYN.log](/home/ubuntu/chipyard/software/firemarshal/logs/rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-build-2026-03-21--16-10-19-T6AGVCRBO6ZV0BYN.log)
  - install log:
    [rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-install-2026-03-21--16-10-51-3BYKVKSQ2T0MK7O4.log](/home/ubuntu/chipyard/software/firemarshal/logs/rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-install-2026-03-21--16-10-51-3BYKVKSQ2T0MK7O4.log)
- 当前 run farm 状态：
  - 最近一次完整 run：
    - `bertmini-launch7` exit `0`
    - `bertmini-infrasetup7` exit `0`
    - `bertmini-runworkload7` 在 `segment=5 / op=2` 明确卡住
    - manager log:
      [2026-03-21--16-16-52-runworkload-02WF3678Z5WY42M7.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-21--16-16-52-runworkload-02WF3678Z5WY42M7.log)
  - 回收状态：
    - `bertmini-terminate7` exit `0`
    - 最后一台实例 `i-0eb51ac0c790295bd` 已被回收
  - 2026-03-22 复核结果：
    - AWS 上没有未终止的 `fsimcluster` 实例
    - AWS 上没有未终止的 `f2.*` 实例
  - 当前没有需要保留的活跃 run farm

## 下一轮验证目标

- 先决定是否保留 / 调整当前本地的 resadd Gemmini-only 草案修复。
- 重新 build / install 有效镜像。
- 从 `launchrunfarm` 开始重新跑完整 FireSim 流程。
- 核对 `segment=5 / stage=0 / subbatch=0 / op=2` 是否不再卡在原始 resadd 路径。
- 若该点通过，再继续确认后续 segment 能自然收尾。
- 最终通过标志仍是：
  - `BERTMINI_PIPELINE_RUNTIME_PASS`

## 关键参考

- 状态文档：
  - [STATUS.md](/home/ubuntu/chipyard/conference/mudnac_hybridmapper_collab_docs/STATUS.md)
- 流程文档：
  - [PROCESS.md](/home/ubuntu/chipyard/conference/mudnac_hybridmapper_collab_docs/PROCESS.md)
- workload 配置：
  - [rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime.json](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests-coupleddma/workload/rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime.json)
- FireSim runtime config：
  - [config_runtime_f2_rerocc_lc_linux_bertmini_pipeline_runtime.yaml](/home/ubuntu/chipyard/sims/firesim/deploy/config_runtime_f2_rerocc_lc_linux_bertmini_pipeline_runtime.yaml)
