# Pipeline Runtime Integration Process

日期：2026-03-24

## 目的

本文件只记录当前稳定有效的工程流程，不记录具体一轮轮调试结论。
具体状态看：

- [HANDOFF.md](/home/ubuntu/chipyard/conference/mudnac_hybridmapper_collab_docs/HANDOFF.md)
- [STATUS.md](/home/ubuntu/chipyard/conference/mudnac_hybridmapper_collab_docs/STATUS.md)
- [PLAN.md](/home/ubuntu/chipyard/conference/mudnac_hybridmapper_collab_docs/PLAN.md)
- [LESSONS_LEARNED.md](/home/ubuntu/chipyard/conference/mudnac_hybridmapper_collab_docs/LESSONS_LEARNED.md)

## 固定流程

- 对当前 workload，固定执行顺序是：
  1. `marshal build`
  2. `marshal install`
  3. `launchrunfarm`
  4. `infrasetup`
  5. `runworkload`
  6. `terminaterunfarm`

- 不手工编辑：
  `sims/firesim/deploy/workloads/*`

- 长时间 FireMarshal / FireSim 任务统一通过：
  - [firemarshal-tmux-run.sh](/home/ubuntu/chipyard/scripts/firemarshal-tmux-run.sh)
  - [firesim-tmux-run.sh](/home/ubuntu/chipyard/scripts/firesim-tmux-run.sh)

- 任何以下变化之后，都重新 `infrasetup`：
  - workload
  - binary
  - rootfs
  - overlay
  - AGFI
  - 中断过的 run 之后重新接续

## 环境要求

- FireMarshal 相关命令前：
  - `source /home/ubuntu/chipyard/env.sh`

- FireSim manager 相关命令前，必须先：
  - `cd /home/ubuntu/chipyard/sims/firesim`
  - `source sourceme-manager.sh --skip-ssh-setup`

- `sourceme-manager.sh` 依赖当前目录，不能在 repo root 直接 source。

## Run Farm 纪律

- 任一时刻最多只保留当前调试必需的 run farm。
- 启动新 run 之前，先确认旧实例已经 `shutting-down` 或 `terminated`。
- 停机顺序固定为：
  1. `firesim terminaterunfarm --forceterminate`
  2. `aws ec2 describe-instances` 复核
  3. 如 manager 返回成功但实例仍在，再手工 `aws ec2 terminate-instances`

- 不信任 manager 返回码本身。
- AWS 实例状态才是最终依据。

## Linux Boot 判读规则

- Linux 启动阶段，只要没有明确报错，就继续等待。
- 不要因为以下现象提前 kill：
  - `uartlog` 长时间无新行
  - `heartbeat.csv` 继续增长但很慢
  - guest 只停留在早期 kernel 输出附近

- 只有以下信号才判定明确失败：
  - `Simulator deadlock detected`
  - `*** FAILED ***`
  - kernel panic / Oops
  - userspace 明确 FAIL
  - `heartbeat.csv` 长时间完全不增长且伴随明确错误

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
  3. 确认 `runworkload` 是否真正进入 guest
  4. 再看 guest 结构化日志 / 最终 PASS-FAIL 标记

## 调试硬约束

- 不改 RTL / 不改硬件。
- 不恢复 `host_addr` 特判。
- 不把多 manager 路径降级成 single manager。
- 不破坏 shared-spad 的 `all-bank / 1KB interleaved` 分配策略。
- `bertmini` 调试路径禁止 CPU fallback。

## Gemmini 语义核对顺序

- 优先看：
  - [README.md](/home/ubuntu/chipyard/generators/gemmini/README.md)
  - [gemmini-rocc-tests/include](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/include)
  - [gemmini-rocc-tests/imagenet](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/imagenet)

- 不要只凭 pipeline-runtime 自己的实现去反推 ISA 语义。

- 一旦问题涉及以下任一内容，必须先回看：
  - accumulator 地址位语义
  - `mvin/mvin2/mvin3`
  - `mvout`
  - `rr_fence(cfg_id)` 与 `gemmini_fence()`
  - shared-spad xlate cfg/range/flush/reset

## FireSim 资源使用规则

- 只要还没拿到决定性结果，就继续跑，不要因为“静默”误判。
- 一旦已经拿到决定性结果：
  - 先停 `runworkload`
  - 再 `terminaterunfarm`
  - 不让 F2 空转

## `spot` 规则

- 当前对 `f2.6xlarge spot` 的结论是：
  AWS 容量不足，持续 `insufficient capacity`
- 在没有新容量信号前，默认先用 on-demand 完成 correctness closure。
