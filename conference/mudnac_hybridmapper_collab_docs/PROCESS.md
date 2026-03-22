# Pipeline Runtime Integration Process

日期：2026-03-22

## 固定流程

- 对这个 workload，固定执行顺序是：
  - `marshal build`
  - `marshal install`
  - `launchrunfarm`
  - `infrasetup`
  - `runworkload`
  - `terminaterunfarm`
- 不手工编辑 `sims/firesim/deploy/workloads/*`。
- 长时间 FireSim manager 任务统一用 [firesim-tmux-run.sh](/home/ubuntu/chipyard/scripts/firesim-tmux-run.sh)。
- 任何 workload / overlay / rootfs / binary / AGFI 变化之后，都重新 `infrasetup`。
- 对当前 `bertmini` 调试，默认按“完整重走”处理，不走省略步骤的捷径。

## CPU Fallback 硬约束

- `bertmini` 调试路径禁止 CPU fallback。
- 不允许把 pointwise、resadd 或其他 runtime 算子切到 CPU 作为通过手段。
- 如果当前代码里已经有这类 CPU fallback：
  - 把它们视为历史调试残留
  - 后续逐项做 Gemmini 根因修复
  - 修好后移除这些 CPU 路径，再重新 build/install 验证

## 进入正确环境

- FireMarshal 相关命令前：
  - `source /home/ubuntu/chipyard/env.sh`
- FireSim manager 相关命令前，必须先：
  - `cd /home/ubuntu/chipyard/sims/firesim`
  - `source sourceme-manager.sh --skip-ssh-setup`
- `sourceme-manager.sh` 依赖当前目录，不能在 repo root 直接 source。

## Run Farm 纪律

- 任一时刻最多只保留一台当前调试必需的 F2 run farm。
- 2026-03-22 已复核当前 AWS 账户：
  - 没有未终止的 `fsimcluster` 实例
  - 没有未终止的 `f2.*` 实例
- 启动新一轮 `launchrunfarm` 前，先确认旧实例已经 `shutting-down` / `terminated`。
- 停机顺序优先是：
  - `firesim terminaterunfarm --forceterminate`
  - 再用 `aws ec2 describe-instances` 复核
  - 如 manager 返回成功但实例仍在，才手工 `aws ec2 terminate-instances`
- 不信任 manager 返回码本身，AWS 实例状态才是最终依据。

## Linux Boot 判读硬约束

- Linux 启动阶段，只要没有明确报错，就一直等待。
- 不要因为以下现象提前 kill：
  - `uartlog` 长时间无新行
  - `heartbeat.csv` 继续增长但很慢
  - guest 只停留在早期 kernel 输出附近
- 只有以下信号才判定 boot 明确失败：
  - `Simulator deadlock detected`
  - `*** FAILED ***`
  - kernel panic / Oops
  - userspace 明确 FAIL
  - `heartbeat.csv` 长时间完全不增长且伴随明确错误
- 在没有上述证据前，不再修改 Linux boot/init 相关代码。

## 监控顺序

- 第一优先级看：
  - `sims/firesim/deploy/logs/`
  - `tmp/firesim-aws-f2/tmux/*.pane.log`
- run host 已经起来后，source of truth 是远端：
  - `/home/ubuntu/sim_slot_0/uartlog`
  - `/home/ubuntu/sim_slot_0/heartbeat.csv`
- 结果目录在：
  - `sims/firesim/deploy/results-workload/`
- 当前常用核查顺序：
  1. 确认 `launchrunfarm` 是否真的拿到实例
  2. 确认 `infrasetup` 是否完成
  3. 确认 `runworkload` 是否进入 `S99run`
  4. 再看 `[bertmini]`、`[prt-progress]`、Gemmini 相关结构化日志

## 当前日志策略

- 默认保留结构化进度日志，不开 raw flood。
- 当前有用的关键字包括：
  - `segment=...`
  - `worker stage=...`
  - `gemm-issue-conv ...`
  - `conv-sync ...`
  - `conv-nb ...`
  - `pointwise-matmul-fallback ...`
  - `pointwise-matmul-fallback-chunked ...`
  - `resadd-fallback ...`
  - `resadd-fallback-map ...`
  - `resadd-fallback-writeback ...`
- 若 `heartbeat.csv` 继续增长，但边界日志长时间停在最后一条结构化日志之后，就把停点收敛到那条日志之后的代码段，不要退回去重新怀疑 Linux boot。
- 如果 `heartbeat.csv` 持续增长、`uartlog` mtime 冻结，而且尾部停在半条 `PRT_PROGRESS_LOG` 生成的日志上：
  - 先怀疑 hot path UART / console backpressure
  - 不要直接把半行日志等价成“Gemmini 一定已经发完并卡在计算里”
  - 先降掉最内层重复日志，再复跑验证
- 但对 2026-03-21 当前最新 run，需要使用更新后的判读：
  - 若尾部稳定停在
    `worker stage=0 subbatch=0 begin op=2 acc=0 dma=2 tiles=2`
    且 `heartbeat.csv` 持续增长，
  - 优先把停点收敛到 resadd Gemmini primitive，
    不要再退回去怀疑 pointwise UART 半行日志。

## 当前调试规则

- Gemmini 语义核对必须优先看：
  - [README.md](/home/ubuntu/chipyard/generators/gemmini/README.md)
  - [gemmini-rocc-tests/include](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/include)
  - [gemmini-rocc-tests/imagenet](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/imagenet)
- 不要只凭 pipeline-runtime 自己的实现去反推 ISA 语义。
- host-backed `dram <-> spm` 路径必须走 [prt_dma.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_dma.c) 的 bounce-buffer helper。
- 对 Gemmini workaround，不能只改 sync 路径；必须同时核对 nonblocking / single-dispatch 路径是否也命中同一保护。
- 新 workaround 必须用 live UART 反证是否真的生效，不能只看离线 shape 猜测。

## 当前已固定的软件处理

- canonical 1x1 pointwise fallback 若请求 `WS`，改为 `OS`，避免进入疑似坏掉的 WS matmul loop。
- 对 canonical pointwise 且 `out_channels > 64` 的场景，新增按输出通道切块的 chunked OS fallback。
- chunked pointwise 当前已进一步改成 direct-strided zero-repack Gemmini 路径。
- chunked pointwise 仍保留 chunk 级边界日志，但内层高频 `begin/end` 热日志已静音。
  - 目的只是避免 UART backpressure 把 guest 卡在 `fprintf/fflush`
  - 这不是 CPU fallback，也不是 Gemmini 类型语义变化
- canonical pointwise 的 split-OC 当前也已有 direct-strided Gemmini 路径。
- 历史上加过的 resadd CPU fallback 不再允许继续作为 bertmini workaround，后续要回到 Gemmini 根修复。
- 当前最新 live 证据表明：
  - pointwise 旧卡点已经被绕开
  - 新 blocker 已收敛到 resadd 原始 `gemmini_loop_ws(... is_resadd=1)` 路径
- 当前工作树里存在一个未验证的 Gemmini-only resadd 草案修复：
  - 在 RISC-V 上把 resadd 运行类型硬钉为 Gemmini
  - 尝试把 `resadd_issue_no_fence()` 从 `sp_tiled_resadd()/loop_ws`
    改成显式 `mvin/mvin2/mvout`
  - 该草案尚未重新 build/install/rerun，不能当作已验证结论

## 当前重排优化纪律

- 先区分两类问题，不要混为一谈：
  - 算子 `pack/repack`
  - DMA misalignment bounce buffer
- 只有前者属于当前“去重排”目标；后者属于 Linux coupleddma 正确性保护，不能为了追求零拷贝直接删掉。
- 当前 `zero-repack` 优先级固定为：
  1. `conv oc-split` 默认 pack/repack
  2. `conv spatial-split`
- 注意：
  - pointwise chunked zero-repack 已经落地，不再是当前 P0
  - 当前 P0 不是“去重排”，而是先修掉 resadd Gemmini stall
- `conv oc-split` 已有 direct-strided 实验入口，但当前默认保持关闭：
  - 原因是它在当前 FPGA 栈上曾卡在 `tiled_conv_stride_auto()`
  - 所以在没有新 FPGA 正例前，不要直接删掉旧的 `pack/repack` 路径
- `conv spatial-split` 的去重排优先级低于前两者：
  - 这条路径不只是 stride，还涉及 tile 输入窗口裁剪、padding、halo
  - 没有明确方案前，不要把它和 pointwise/oc-split 放在同一难度层级
- resadd 的当前问题不要放进“去重排”篮子里：
  - 它是原始 Gemmini primitive 前进性 / 正确性问题
  - 不是 pack/repack 问题

## 下一轮最小执行清单

1. `marshal build`
2. `marshal install`
3. `launchrunfarm`
4. `infrasetup`
5. `runworkload`
6. 监控 run host `uartlog` 与 `heartbeat.csv`
7. 先确认 pointwise 旧风险点继续通过：
   - `segment=2 / subbatch=14`
   - `segment=3`
   - `segment=4`
8. 然后重点确认 `segment=5 / worker stage=0 subbatch=0 begin op=2`
   之后是否还能继续前进
9. 若再次停在该 resadd begin 且 `heartbeat.csv` 继续增长，
   立即按“resadd Gemmini primitive stall”记录和回收
10. 若发现 CPU fallback 镜像或无效 workaround，立刻 `terminaterunfarm`
11. 本轮结束后立刻 `terminaterunfarm`
