# Pipeline Runtime Integration Status

日期：2026-03-20

## 当前目标

在不修改 model 定义文件、不破坏 HybridMapper 原有输出的前提下，完成面向 Gemmini `pipeline-runtime` 的离线导出与执行闭环。当前第一阶段目标仍然是：

- 只支持 `stage == 1 layer`
- 用同一份 model 定义文件同时服务 HybridMapper / Mudnac 参考语义 / pipeline-runtime
- 在 `bertmini` 上跑通：
  - `cpu` 后端产出 golden
  - `fpga` 后端在 FireSim F2 上对齐 golden

## 已冻结约束

- 不实现 MudnacSim 里假的“多层 stage”语义
- `pipeline-runtime` 只读新接口，不读旧接口
- layer mapping 中的 SPM 地址从 `0` 开始，只表示 local zero-based 视图
- runtime 在执行时负责 rebasing、页表绑定和 accelerator 动态分配
- 生成的 mapping 里 `physicalAccIds` 目前为空；主语义仍然是 `vAccIdxList + accUtil`
- `coupled DMA` 的 DRAM `src/dst/completion` 都必须使用 TL 可见物理地址
- FPGA 相关验证必须走标准流程：
  - `marshal build -> marshal install -> launchrunfarm -> infrasetup -> runworkload -> terminaterunfarm`
- 不手工修改 `sims/firesim/deploy/workloads/*`
- F2 只使用 `f2.6xlarge`

## 2026-03-20 最新状态更新

- 2026-03-20 14:00 UTC 左右的新一轮 dedicated bertmini rerun，暴露出一个比 `bertmini` 更早的 rootfs/init 卡点：
  - result dir:
    [2026-03-20--13-51-25-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime](/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-20--13-51-25-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime)
  - live capture:
    [live-capture](/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-20--13-51-25-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime/live-capture)
  - 这轮一开始容易被误判成“内核早期停住”，后来通过把 `uartlog` 里的 `\r` 展开成 `\n` 复查，已经确认：
    - guest 能继续走到
      `Mounting /dev/iceblk as root device`
      `running /etc/init.d/S01syslogd`
      `running /etc/init.d/S02klogd`
      `running /etc/init.d/S02sysctl`
      `running /etc/init.d/S10mdev`
      `Starting mdev: OK`
    - 之后 `heartbeat.csv` 继续增长，但 `uartlog` 长时间不再更新
    - 和上一轮能跑到 `S40network -> S99run -> [bertmini]` 的镜像相比，这一轮新的稳定停点已经前移为：
      `S10mdev` 返回后、`S40network` 之前
  - 镜像内脚本内容也已经核对：
    - [S10mdev](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests-coupleddma/workload/overlay/root/etc/init.d/S10mdev)
      对应的默认 Buildroot 逻辑会在打印 `Starting mdev: OK` 之后继续做
      `/sys` modalias coldplug + `modprobe -abq`
    - 这个阶段正好符合“日志静默、但 boot 继续占用 target cycle”的症状
  - 当前已落地 workaround：
    - 在
      [rerocc-linux-tests-coupleddma/workload/overlay/root/etc/init.d/S10mdev](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests-coupleddma/workload/overlay/root/etc/init.d/S10mdev)
      与
      [rerocc-linux-tests/workload/overlay/root/etc/init.d/S10mdev](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests/workload/overlay/root/etc/init.d/S10mdev)
      新增 overlay 覆盖脚本
    - 保留 `mdev -df` 守护进程启动
    - 默认跳过那段对当前 FireSim workload 非关键、但会卡住 boot 的 coldplug/modprobe 扫描
  - 对应 workload 已重新 build/install：
    - build log:
      [rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-build-2026-03-20--14-10-33-7RNKMLZ1A9J0WQIL.log](/home/ubuntu/chipyard/software/firemarshal/logs/rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-build-2026-03-20--14-10-33-7RNKMLZ1A9J0WQIL.log)
    - install log:
      [rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-install-2026-03-20--14-10-57-DS65MS1898KMBTG9.log](/home/ubuntu/chipyard/software/firemarshal/logs/rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-install-2026-03-20--14-10-57-DS65MS1898KMBTG9.log)
  - 因此当前最新 bertmini FPGA 调试路径应更新为：
    - 先验证新的 `S10mdev` overlay 是否恢复 `S40network -> S99run -> [bertmini]`
    - 只有 boot 恢复后，才继续回到此前已经定位到的 `oc-split` / `conv-sync` 执行路径
- 最新一轮针对 bertmini F2 机时过高的问题，已新增独立日志开关并验证生效：
  - [prt_progress.h](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/include/prt_progress.h)
    新增 `PRT_ENABLE_PROGRESS_RAW_LOG`
  - [pipeline-runtime/Makefile](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/Makefile)
    与
    [rerocc-linux-tests/Makefile](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests/Makefile)
    默认保持 `PRT_ENABLE_PROGRESS_LOG=1`、`PRT_ENABLE_PROGRESS_RAW_LOG=0`
  - 这意味着：
    - 仍保留 `segment=...` / `worker ...` / `dma-wait ...` 这类结构化进度日志
    - 默认关闭每笔 DMA 的 `[prt-raw] dma pre-acquire/...` 超高频原始输出
- 对应镜像已重新 build/install：
  - build log:
    [rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-build-2026-03-20--12-05-54-L566IIYHUDUA285E.log](/home/ubuntu/chipyard/software/firemarshal/logs/rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-build-2026-03-20--12-05-54-L566IIYHUDUA285E.log)
  - install log:
    [rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-install-2026-03-20--12-06-19-TWE2TR1RAHPBMFJY.log](/home/ubuntu/chipyard/software/firemarshal/logs/rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-install-2026-03-20--12-06-19-TWE2TR1RAHPBMFJY.log)
- 关闭 raw DMA 日志后的 dedicated bertmini F2 重跑，已经给出新的、更干净的定位结论：
  - runworkload log:
    [2026-03-20--12-12-43-runworkload-W93EAQU16CHOFVOD.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-20--12-12-43-runworkload-W93EAQU16CHOFVOD.log)
  - result dir:
    [2026-03-20--12-12-43-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime](/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-20--12-12-43-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime)
  - live capture:
    [live-capture](/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-20--12-12-43-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime/live-capture)
  - 新证据已经明确说明：
    - `[prt-raw]` 已完全消失
    - `segment=0` 已完整通过，并进入 `segment=1`
    - 新的 hang 不再发生在首个 segment / 首轮 DMA
    - 最新稳定停点是：
      `segment=1 begin ...`
      `worker stage=0 subbatch=0 begin/compute-done/done`
      `worker stage=0 subbatch=1 begin`
      之后 `uartlog` 停止更新，但 `heartbeat.csv` 继续增长
  - 因而当前最新主 blocker 已再前移为：
    - `segment=1` 的 split-OC compute 路径在 `subbatch=1 begin` 后、`compute-done` 前 hang
- 为了继续收敛这个新停点，已在
  [prt_gemmini_adapter.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_gemmini_adapter.c)
  新增更窄的 split-OC 诊断日志：
  - `oc-split ... begin/launch/done/repack-begin/repack-end`
  - `conv-sync ... acquire-begin/acquire-end/tiled-conv-begin/tiled-conv-end/fence-begin/fence-end`
  - 下一轮 rerun 的目的不再是判断“有没有 raw DMA 噪声”，而是直接区分：
    - 卡在 manager acquire
    - 卡在 `tiled_conv_auto`
    - 卡在 `rr_fence_scope`
    - 还是卡在最终 repack
- 一个新的 workload 入口 bug 已确认并修复：
  - 小回归镜像本应使用
    [host-init-pipeline-noexec.sh](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests-coupleddma/workload/host-init-pipeline-noexec.sh)
    关闭 `.pipeline_runtime_only`
  - 但 overlay 目录被复用时，旧的
    [`.pipeline_runtime_only`](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests-coupleddma/workload/overlay/root/rerocc-linux-tests-coupleddma)
    会残留，导致 small-regression 镜像错误跳转到 bertmini runtime
  - 现已在
    [host-init.sh](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests-coupleddma/workload/host-init.sh)
    的 `stage_coupleddma_overlay()` 里先 `rm -f .pipeline_runtime_only`
- 修复后，small-regression image 已重新 build/install：
  - build log:
    [rerocc-lc-linux-coupleddma-regression-small-pipelinefiles-build-2026-03-20--03-42-28-D7W7NPYBCLNEMHJ5.log](/home/ubuntu/chipyard/software/firemarshal/logs/rerocc-lc-linux-coupleddma-regression-small-pipelinefiles-build-2026-03-20--03-42-28-D7W7NPYBCLNEMHJ5.log)
  - install log:
    [rerocc-lc-linux-coupleddma-regression-small-pipelinefiles-install-2026-03-20--03-42-58-N30FKHHIYUO9PM0G.log](/home/ubuntu/chipyard/software/firemarshal/logs/rerocc-lc-linux-coupleddma-regression-small-pipelinefiles-install-2026-03-20--03-42-58-N30FKHHIYUO9PM0G.log)
- 修复后的小回归 FPGA 重跑已经拿到真实 DMA verdict：
  - runworkload log:
    [2026-03-20--03-48-28-runworkload-KHO2Y8NS8WNJO98U.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-20--03-48-28-runworkload-KHO2Y8NS8WNJO98U.log)
  - result dir:
    [2026-03-20--03-48-28-rerocc-lc-linux-coupleddma-regression-small-pipelinefiles-f2-rerocc-linux-regression-small-pipelinefiles](/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-20--03-48-28-rerocc-lc-linux-coupleddma-regression-small-pipelinefiles-f2-rerocc-linux-regression-small-pipelinefiles)
  - guest 已按预期执行：
    - `GEMMINI_ALL_PASS`
    - `DMA_ALL_PASS`
    - coverage 中新增
      `CASE_RESULT dma_dram_to_shared_misaligned_fullpage PASS`
  - 这说明：
    - 先前针对 Linux host<->SPM misaligned full-page DMA 的 bounce-buffer workaround 已在 FPGA 上得到正向验证
    - 旧的“第一笔 DMA 因 misaligned 1B copy 卡在 set_src”判断不再是最新主结论
- small-regression 仍有一个独立失败：
  - nonblocking 最终输出：
    - `SCENARIO_RESULT name=conv_dma_parallel_nonblocking pass=0`
    - `NONBLOCKING_SUMMARY s1=0 s2=1 s3=1 s4=1`
    - `ALL_TESTS_FAIL matrix_ret=0 coverage_ret=0 nonblocking_ret=1`
  - 这个失败与本轮 misaligned full-page DMA 关键验证结论分离；当前更像是 Linux nonblocking 场景的单独问题
- 由于 small-regression 已给出真实 DMA verdict，随后已继续重跑 dedicated bertmini pipeline-runtime：
  - infrasetup log:
    [2026-03-20--04-11-37-infrasetup-DPTC5AVV03XOGILR.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-20--04-11-37-infrasetup-DPTC5AVV03XOGILR.log)
  - runworkload log:
    [2026-03-20--04-13-35-runworkload-16O7E4UXGU82BQRM.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-20--04-13-35-runworkload-16O7E4UXGU82BQRM.log)
  - result dir:
    [2026-03-20--04-13-35-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime](/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-20--04-13-35-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime)
- 最新 bertmini FPGA 结论已经前移：
  - 旧症状不再成立：
    - 不再卡在 `before-set-src`
    - 不再卡在第一笔 DMA submit
  - 当前已明确看到：
    - `init load-model-bin end`
    - `init load-input-blob end`
    - `segment=0 begin`
    - `worker stage=0 ready`
    - 连续多次
      `dma post-src`
      `dma-wait fence-enter`
      `dma-wait fence-done`
    - 其中已经出现 `full_byte_mode_hint=1`
  - 新的 blocker 变成：
    - guest `rerocc_pipeline` 在
      `worker stage=0 subbatch=0 begin op=1 acc=0 dma=2 tiles=2`
      之后发生 `Segmentation fault`
    - kernel 证据：
      - `unhandled signal 11 code 0x2`
      - `badaddr: 0000003fb21ba400`
      - `epc : 0000003fb323d424`
    - wrapper 证据：
      - `BERTMINI_PIPELINE_RUNTIME_FAIL`
- 因而当前最新主 blocker 已从：
  - “CoupledDMA 第一次 `set_src` / readySrc 路径 hang”
  前移为：
  - “pipeline-runtime 在 FPGA guest 上通过多次 DMA wait 后出现用户态内存破坏/非法指针访问”
- 最新 bertmini run farm 已回收：
  - instance `i-0ac5afcb3d07d950a`
  - terminate log:
    [2026-03-20--04-38-16-terminaterunfarm-S8FN3IUKR4V3HNNI.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-20--04-38-16-terminaterunfarm-S8FN3IUKR4V3HNNI.log)
  - 当前 AWS 状态：`shutting-down`
- `subbatch` 语义已再次澄清并固定：
  - 这里统一按 MudnacSim 语义理解，也就是
    “同一时间一个 stage 并发推理的 batch 数”
  - 当前 `bertmini` 这条 `pipeline-runtime` / HybridMapper 路径，目标语义就是 `subbatch=1`
- 对 `subbatch=1` 的当前证据已经确认：
  - [prt_yaml_loader.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_yaml_loader.c)
    默认把每个 segment 的 `subbatch_size` 设成 `1`
  - 只有 YAML 显式包含 `subBatchSize` 时，runtime 才会覆盖该默认值
  - 当前 `bertmini` 的三份
    [pipeline_mapping.*.yaml](/home/ubuntu/chipyard/conference/HybridMapper/output/pipeline_runtime/bertmini)
    都没有写出 `subBatchSize`
  - 因此当前实际配置仍然是 `subbatch_size=1`
- 需要避免再次误读的点也固定下来：
  - guest wrapper 传的是 `--batch 16`，所以 runtime 会计算
    `target_subbatch = ceil(16 / 1) = 16`
  - 日志里的
    `worker stage=0 subbatch=2`
    是第 3 个执行轮次的 progress index，
    不是“这个 stage 同时在跑 2 个 batch”
  - 当前最新 blocker 仍应继续按
    “单样本 stage 并发、跨轮次重复执行后的 guest-side segfault”
    去定位，而不是回退成“subbatch 配错”
- 新一轮 bertmini FPGA rerun 已重新进入 FireSim 标准流程：
  - launchrunfarm 已成功：
    - instance `i-0e6f981e581bbd433`
    - private IP `192.168.1.10`
    - launch log:
      [2026-03-20--10-13-54-launchrunfarm-EMQ9N09B23F1JE47.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-20--10-13-54-launchrunfarm-EMQ9N09B23F1JE47.log)
  - 当前正在执行新一轮 `infrasetup`
  - 若本轮仍失败，后续 patch 与分析都继续以
    `subbatch=1`
    为固定前提

## 2026-03-20 对同类 DMA 风险的复查结论

- 已对 `pipeline-runtime` 当前所有活跃 DMA 入口做静态复查，重点检查是否还存在
  “Linux guest host buffer 与 SPM 端点 `mod64` 不一致、且 `bytes >= 64`”
  的同类风险。
- 当前确认已经被统一保护的活跃 host-backed 路径包括：
  - [prt_runtime.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_runtime.c)
    中 `stage_prepare_exec_views()` 对 fixed tensor 的 `dram -> spm` 预取
  - [prt_scheduler.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_scheduler.c)
    中 `prt_process_c1()` 的 `dram -> spm`
  - [prt_runtime.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_runtime.c)
    中 `copy_tensor_pages_to_model_aliases()` 的 `spm -> model blob alias`
  - [prt_scheduler.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_scheduler.c)
    中 `prt_process_c2()` 的 `spm -> dram`
- 这些活跃路径在 Linux/RISC-V 上都会统一落到
  [prt_dma.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_dma.c)
  的 `prt_dma_copy_dram_to_spm_pages()` /
  `prt_dma_copy_spm_pages_to_dram()` helper，
  因而都会经过当前已经验证过的 bounce-buffer 保护。
- 复查后确认：当前实际运行链路里，已经没有第二条“活跃的 host<->SPM 直提 DMA”
  会绕过这层保护；也就是说，和这次 bertmini 第一笔 DMA hang 同类的问题，
  目前不再存在于已启用路径中。
- 同时确认两个“不是同类问题”的剩余路径：
  - [prt_scheduler.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_scheduler.c)
    里的 overlap `prt_dma_submit()` 直提路径只对 `to_ring=1` 开放，也就是
    contiguous page-list 的 `SPM -> SPM` copy；其 `src/dst` 基址按 page 对齐，
    不属于这次 host-backed misaligned full-page 问题。
  - [prt_dma.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_dma.c)
    的 `prt_dma_copy_spm_va()` 仍允许未来提交任意 offset 的 `SPM -> SPM` 请求；
    但当前仓库内没有 call site，所以它是 latent risk，不是当前活跃 blocker。
- 因而当前固定结论应更新为：
  - “第一笔 DMA hang 已由 host<->SPM bounce-buffer workaround 修复”
  - “当前 bertmini blocker 是 DMA 成功推进后的 guest-side segfault”
  - “若未来重新启用任意-offset 的 `SPM -> SPM` 大块 copy，仍需单独验证
    `bytes >= 64 && src_mod64 != dst_mod64` 的形态，而不能直接套用当前 host bounce 结论”

## 已完成

- HybridMapper 已新增独立 exporter：
  - [create-pipeline-runtime-artifacts.py](/home/ubuntu/chipyard/conference/HybridMapper/scripts/create-pipeline-runtime-artifacts.py)
- `pipeline-runtime` 已切到新接口：
  - 支持 `--backend cpu|fpga`
  - 支持 `--layer-mapping-yaml`
  - 支持 `--golden-out`
- `pipeline-runtime` 已对齐当前 entire-model 必需语义：
  - single-layer stage
  - zero-based local SPM mapping
  - fixed tensor 独立 weight page 视图
  - fixed tensor lazy fetch
- `coupled DMA` 地址语义修复已落地：
  - completion flag submit 前先 `virt_to_phys`
  - Linux/RISC-V `dram <-> spm` 路径按 host page 切块并逐块 `virt_to_phys`
  - `C2` 的 DRAM export overlap 先关闭
- 首个 DMA 停点的窄日志已补上：
  - `src/dst/done` 的 `mod64`
  - `initial_wide_hint`
  - `full_byte_mode_hint`
  - `submit-fence begin/done`
  - `dma-wait fence-enter/done`
- 新一轮 ReRoCC 映射窄日志已补上：
  - `rr-acquire armed ... raw_cfg/cfg_acq/cfg_mgr/opc_map`
  - `before-set-dst ... rr_cpu/rr_cfg/rr_mgr/rr_opc/rr_opc_map/rr_cfg_raw`
  - `before-set-src ... rr_cpu/rr_cfg/rr_mgr/rr_opc/rr_opc_map/rr_cfg_raw`
- 约束已固定：
  - 后续只要遇到 Gemmini / DMA 调用接口问题，优先参考 Linux 下三个回归测试的写法
  - 具体优先参考：
    - [rerocc_dma_matrix_linux_coupleddma.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests-coupleddma/rerocc_dma_matrix_linux_coupleddma.c)
    - [rerocc_lc_gemmini_matrix_linux_coupleddma.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests-coupleddma/rerocc_lc_gemmini_matrix_linux_coupleddma.c)
    - [rerocc_lc_coverage_linux_coupleddma.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests-coupleddma/rerocc_lc_coverage_linux_coupleddma.c)
- 已加入一个最小实验性软件改动：
  - `dma_blocking_submit()` 在 `set_dst/set_src` 前显式执行 `fence rw, rw`
  - 目的只是验证 software ordering 是否影响当前 FPGA 停点
- `artifacts validate` 已优化为：
  - layer mapping 一次加载
  - 一次解析
  - 驻留内存供所有 stage 复用
- HugeTLB/PTBR 路径已落地：
  - anonymous `MAP_HUGETLB`
  - `hugetlbfs`
  - 失败后再回退到普通页连续性探测
- dedicated `bertmini` FireMarshal workload 已固定：
  - source workload:
    [rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime.json](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests-coupleddma/workload/rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime.json)
  - runtime config:
    [config_runtime_f2_rerocc_lc_linux_bertmini_pipeline_runtime.yaml](/home/ubuntu/chipyard/sims/firesim/deploy/config_runtime_f2_rerocc_lc_linux_bertmini_pipeline_runtime.yaml)
- 当前 build/install 链已补保护：
  - dedicated workload 默认使用 `PIPELINE_RUNTIME_PROGRESS=1`
  - `host-init` 会检查最终 RISC-V binary 中必须包含 `[prt-early] enter main`

## 本地最新已验证结果

- host 版 `pipeline-runtime` 已 `clean all` 通过
- 最新 smoke 命令：
  - `METHODS=ours2 BATCH=1 SKIP_EXPORT=1 SKIP_BUILD=1 bash generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/scripts/run_bertmini_host_closure.sh`
- 最新结果：
  - `golden compare passed`
  - `BERTMINI_HOST_CLOSURE_PASS`
- 本轮新增最小验证：
  - host build:
    `make -C generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime clean all PIPELINE_RUNTIME_PROGRESS=1`
  - RISC-V Linux build:
    `make -C generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests -f generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests/Makefile abs_top_srcdir=/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests CC_LINUX=/home/ubuntu/chipyard/.conda-env/riscv-tools/bin/riscv64-unknown-linux-gnu-gcc PIPELINE_RUNTIME_PROGRESS=1 rerocc_pipeline_runtime-linux`
  - host closure:
    `METHODS=ours2 BATCH=1 SKIP_EXPORT=1 SKIP_BUILD=1 bash generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/scripts/run_bertmini_host_closure.sh`
  - 结果：
    - host build 通过
    - RISC-V Linux 交叉编译通过
    - host closure 通过
    - 随后已完成新一轮 FPGA 重跑，并拿到了新增窄日志的 FPGA 证据

## 当前重跑前置阻塞

- 在准备把新增窄日志重新打进 workload image 时，最新一次 FireMarshal build 卡在 image overlay 阶段，而不是卡在 workload 编译
- 对应 log:
  [rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-build-2026-03-19--06-37-21-QKLVY2C67Q1YRO0X.log](/home/ubuntu/chipyard/software/firemarshal/logs/rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-build-2026-03-19--06-37-21-QKLVY2C67Q1YRO0X.log)
- 失败点是：
  - `guestmount --pid-file ... -a ...img -m /dev/sda .../disk-mount`
  - `libguestfs: error: /usr/bin/supermin exited with error status 1`
- `libguestfs-test-tool` 已进一步把根因收敛到：
  - `supermin: exception: Sys_error("/var/tmp/supermin...tmpdir: Permission denied")`
  - `libguestfs` 需要写 `/var/tmp/.guestfs-*` / `/var/tmp/supermin*.tmpdir`
- 这说明当前失败首先是执行环境对 `libguestfs/supermin` 临时目录的写权限问题
- 因而此刻还不能把这次 `marshal build` 失败当成：
  - `pipeline-runtime` 二进制回归
  - FireMarshal workload 配置错误
  - guest image overlay 内容错误
- 在这个阻塞排除前，DMA 停点分析仍以 2026-03-19 05:15 UTC 那次 FPGA 结果为基准

## 本轮已固定的新经验

- 2026-03-19 已确认：
  - 上述 `libguestfs/guestmount/supermin` 报错主要是 Codex 沙箱执行路径引入的假阻塞
  - 不是当前 workload、binary、overlay payload 本身坏了
- 同一份 workload 在非沙箱真实环境中，按用户要求先执行：
  - `cd /home/ubuntu/chipyard/sims/firesim`
  - `source sourceme-manager.sh`
  后再跑：
  - `./marshal build ...rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime.json`
  - `./marshal install ...rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime.json`
  均已成功
- 因此后续必须默认：
  - FireMarshal build/install
  - FireSim manager 相关长命令
  直接走非沙箱真实环境，不再先在沙箱里做这类会制造假故障的验证

## 2026-03-19 当前最新 live FPGA 结果

- 非沙箱真实环境下，本轮已经成功完成：
  - `marshal build`
  - `marshal install`
  - `launchrunfarm`
  - `infrasetup`
  - 新一轮 `runworkload` 已启动
- 当前 run host:
  - instance: `i-0887427d22c518b31`
  - private ip: `192.168.1.21`
- live 结果目录：
  [2026-03-19--07-23-32-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime](/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-19--07-23-32-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime)
- live `heartbeat.csv` 仍在持续增长，说明 FPGA 仿真还在推进
- 但到 heartbeat 超过 360 秒时，live `uartlog` 仍未进入：
  - `S99run`
  - `[bertmini]`
  - `[prt-early]`
  - `[prt-progress]`
- 当前最后一条稳定可见 live UART 行变成了：
  - `[    0.000000] software IO TLB: mapped [mem 0x00000000fbfff000-0x00000000fffff000] (64MB)`
- 这和 2026-03-19 05:15 UTC 那轮已知基线不同：
  - 旧基线已进入 `S99run` / `bertmini` wrapper / `pipeline-runtime`
  - 本轮目前还停留在 Linux 早期 boot 阶段
- 因此在本轮 run 真正越过 `S99run` 之前，当前最早 blocker 已暂时前移到：
  - guest Linux boot 早期阶段
  - 还不能直接把本轮现场继续解释成第一次 DMA submit 停点

## 2026-03-19 更新后的最新结论

- 上一节里的 “Linux boot 早期阶段可能是 blocker” 只是过早判断
- 用户判断正确：
  - Linux 启动时间确实很长
  - 本轮 fresh rerun 最终还是越过了 `S99run`
  - 并重新进入了 `bertmini` wrapper 与 `pipeline-runtime`
- 本轮真正有价值的新增 FPGA 证据是：
  - 新增日志已经确认出现：
    - `submit-fence begin`
    - `submit-fence done`
    - `set-dst done`
    - `before-set-src`
  - 但仍然没有出现：
    - `set-src done`
    - `dma-wait fence-enter`
    - `dma-wait fence-done`
- 当前 fresh rerun 的最后一条稳定可见 live 日志已前移到：
  - `[prt-progress] dma-submit token=1 before-set-src src=0x104ad6410`
- 同时 `heartbeat.csv` 继续推进到 1492 秒量级
- 说明：
  - 本轮不再是卡在 `hw_dma_submit_fence()`
  - 也不再是卡在 `hw_dma_set_dst(...)` 之后立刻失联
  - 当前更像是在 `hw_dma_set_src(...)` 指令本身、或它触发的紧邻硬件交互处停住
- 本轮 run farm 已按用户要求回收：
  - instance `i-0887427d22c518b31`
  - `terminaterunfarm --forceterminate` 后已进入 `shutting-down`

## 最新 FPGA 已确认事实

- FireSim 基础设施当前不是主 blocker
- 2026-03-19 这轮 dedicated `bertmini` 镜像已经确认是最新 build/install：
  - FireMarshal build log:
    [rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-build-2026-03-19--05-05-44-YOBJ7U7YQUN3WPLT.log](/home/ubuntu/chipyard/software/firemarshal/logs/rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-build-2026-03-19--05-05-44-YOBJ7U7YQUN3WPLT.log)
  - FireMarshal install log:
    [rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-install-2026-03-19--05-06-17-AWJC9LIQJLEF9NOU.log](/home/ubuntu/chipyard/software/firemarshal/logs/rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-install-2026-03-19--05-06-17-AWJC9LIQJLEF9NOU.log)
- 最新 FPGA run 已明确走到：
  - Linux boot
  - `S99run`
  - guest wrapper 的 `[bertmini] hugetlb ...`
  - `[prt-early] enter main`
  - `[prt-early] runtime_init done`
  - `artifacts validate end`
  - `init load-model-bin end`
  - `init load-input-blob end`
  - `segment=0 begin`
  - `worker stage=0 ready`
  - 第一个 `dma-submit`
- 本轮最新 live `uartlog` 证据：
  - [runworkload log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-19--05-15-08-runworkload-7AU009ZW4NTKKT8N.log)
  - result dir:
    [2026-03-19--05-15-08-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime](/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-19--05-15-08-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime)
- 到目前为止，没有再看到：
  - `Segmentation fault`
  - `unhandled signal`
  - `BERTMINI_PIPELINE_RUNTIME_FAIL`

## 2026-03-19 最新静态检查更新

- 针对当前最新停点
  - 可见：`[prt-raw] dma post-dst`
  - 不可见：`[prt-raw] dma post-src`
  的静态检查，重点已从 `GemminiCoupledDMA` 本体单独转向 `ReRoCC client/manager -> DMA` 之间的前端 ready/ack 路径
- 新的关键静态结论：
  - [GemminiCoupledDMA.scala](/home/ubuntu/chipyard/generators/gemmini/src/main/scala/gemmini/GemminiCoupledDMA.scala) 中
    - `readyDest = isDest`
    - `readySrc = isSrc && copyReqQ.io.enq.ready`
  - 也就是说，DMA 本体对 `DEST_INFO` 永远 ready，而 `SRC_INFO` 只受 `copyReqQ` 是否可入队影响
  - 但当前软件路径是单笔 `submit_and_wait` 顺序提交，前一笔会在 `hw_dma_fence()` 返回后才继续下一笔，所以“单纯因为 CoupledDMA queue 长期满导致这第一条 `set_src` 卡死”的解释目前偏弱
- 对照
  - [Client.scala](/home/ubuntu/chipyard/generators/rerocc/src/main/scala/client/Client.scala)
  - [Manager.scala](/home/ubuntu/chipyard/generators/rerocc/src/main/scala/manager/Manager.scala)
  后，更像真实根因的是：
  - `set_dst` 从软件返回，只证明 ReRoCC client 已接受该指令，不证明 manager 已经给出 `sInstAck`，也不证明 DMA 本体已经真正接收
  - 紧随其后的 `set_src` 可能卡在：
    - `InstructionSender` 输入队列尚未腾空
    - cfg credit / ack 尚未回补
    - manager 侧 `sInstAck` / response 仲裁未放行
    - 或更窄地，只有 `SRC_INFO` 这条命令在 manager->DMA 交接处被压住
- 与已通过 FPGA 的 Linux coupleddma 回归对照后，仍应保持这个约束：
  - 优先复用它们的调用顺序
  - 保持 `rr_set_opc -> set_dst -> set_src` 区域尽量干净
  - 只有为了缩窄停点时，才加极窄探针
- 因此本轮新增的最小验证探针是：
  - 在 [prt_dma.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_dma.c) 的 `post-dst` 与 `set_src` 之间加入只读 monitor 探针
  - 新增 raw 标记：
    - `[prt-raw] dma pre-mon`
    - `[prt-raw] dma post-mon`
  - 中间执行 `FUNCT_READ_MONITOR(MON_VALID)`
- 这组新探针的判读规则：
  - 若能看到 `post-mon` 但仍看不到 `post-src`：
    - 更支持“只有 `SRC_INFO` / `readySrc` 路径卡住”
  - 若停在 `post-dst` 之后、`post-mon` 之前：
    - 更支持“ReRoCC 前端或 response/ack 路径已经先卡住，`set_src` 还没成为第一责任点”

## 2026-03-19 最新真实环境重跑进度（monitor probe 版本）

- 已完成本轮最小诊断改动的本地验证：
  - host build 通过
  - RISC-V Linux 交叉编译通过
  - host closure 通过，结果仍是 `BERTMINI_HOST_CLOSURE_PASS`
- 已在真实环境重新完成：
  - FireMarshal build:
    [rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-build-2026-03-19--16-59-09-GDTWENPF0C7E9BTS.log](/home/ubuntu/chipyard/software/firemarshal/logs/rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-build-2026-03-19--16-59-09-GDTWENPF0C7E9BTS.log)
  - FireMarshal install:
    [rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-install-2026-03-19--17-02-37-RUA0WG35Y310UDZA.log](/home/ubuntu/chipyard/software/firemarshal/logs/rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-install-2026-03-19--17-02-37-RUA0WG35Y310UDZA.log)
  - launchrunfarm:
    [2026-03-19--17-03-05-launchrunfarm-JUAXXK2SV8B3P6M4.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-19--17-03-05-launchrunfarm-JUAXXK2SV8B3P6M4.log)
  - infrasetup:
    [2026-03-19--17-03-33-infrasetup-Y25803CDSP6PYJDL.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-19--17-03-33-infrasetup-Y25803CDSP6PYJDL.log)
  - 新 runworkload:
    [2026-03-19--17-09-48-runworkload-0XQX5G0CG7TDUVB8.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-19--17-09-48-runworkload-0XQX5G0CG7TDUVB8.log)
- 当前 run host:
  - instance: `i-0d236b83fe1bfeea8`
  - private ip: `192.168.1.75`
- 当前结果目录：
  [2026-03-19--17-09-48-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime](/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-19--17-09-48-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime)
- 到写入此节时，manager 仍未回拷本地 `uartlog`，因此 live 证据以 run host 上的
  `/home/ubuntu/sim_slot_0/uartlog` 和 `/home/ubuntu/sim_slot_0/heartbeat.csv` 为准
- 当前 live 状态：
  - `uartlog` 行数：`114`
  - 最后一条稳定可见 UART：
    - `[    0.000000] software IO TLB: mapped [mem 0x00000000fbfff000-0x00000000fffff000] (64MB)`
  - `heartbeat.csv` 仍持续推进到大约 `4269143560, 477`
- 当前判断：
  - 这轮 run 还没有重新进入 `S99run` / `[bertmini]` / `[prt-early]`
  - 但也还没有出现：
    - `Simulator deadlock detected at target cycle 0`
    - `heartbeat` 掉回 `0`
    - 显式 Linux panic / crash
  - 因此截至目前不能把这轮现场判成“已经卡死”
  - 现阶段仍按“Linux 启动很慢，继续等待更可靠”处理

## 当前 blocker

当前 blocker 已经从“`main()` 极早期崩溃”收敛到了“第一个 DMA submit 附近停住”。

### 1. 已解决的旧 blocker

- 旧镜像误用了 `PIPELINE_RUNTIME_PROGRESS=0`
- 这件事已经被最新 build/install 修正
- FPGA 现已稳定看到：
  - `[prt-early] ...`
  - `[prt-progress] ...`
  - 第一段运行期 worker / DMA 日志

### 2. 当前新 blocker

- 最新 fresh FPGA rerun 上最后一条稳定可见的进度日志是：
  - `[prt-progress] dma-submit token=1 before-set-src src=0x104ad6410`
- 对应 live run host 上：
  - `uartlog` 在这条后面连续数分钟没有新增
  - `heartbeat.csv` 仍持续递增
- 这说明：
  - 当前不是 Linux boot / wrapper / early main 路径问题
  - 当前也不是 “完全没有 progress log” 的镜像问题
  - 当前更像是第一次 DMA 提交过程中，在 `hw_dma_set_src(...)` 本身或其紧邻硬件交互处卡住

### 3. 与已通过 FPGA 的 Linux coupleddma 自测对照后的新增判断

- 已对照这些 known-good Linux/coupleddma 用例：
  - [rerocc_dma_matrix_linux_coupleddma.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests-coupleddma/rerocc_dma_matrix_linux_coupleddma.c)
  - [rerocc_lc_gemmini_matrix_linux_coupleddma.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests-coupleddma/rerocc_lc_gemmini_matrix_linux_coupleddma.c)
  - [rerocc_lc_coverage_linux_coupleddma.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests-coupleddma/rerocc_lc_coverage_linux_coupleddma.c)
  - [rerocc_lc_nonblocking_linux_coupleddma.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests-coupleddma/rerocc_lc_nonblocking_linux_coupleddma.c)
- 这些通过用例在 DMA 临界区的共同模式是：
  - `rr_acquire_cfg_with_retry(...)`
  - `rr_set_opc(2, cfg_id)`
  - 几乎立刻 `set_dst`
  - 紧接着 `set_src`
  - 然后只在 userspace 自旋等 done flag，再 `rr_fence/rr_release`
- 它们在 `rr_set_opc -> set_dst/set_src` 之间基本不做：
  - `printf/fflush`
  - `getcpu()` 之类 syscall
  - 其他明显会把线程带进 kernel 的调试动作
- 当前 pipeline-runtime 与其不同的地方是：
  - stage-0 上 DMA 用的是 `cfg=0`，Gemmini 用的是 `cfg=1`
  - known-good 自测通常反过来固定成 `GEMMINI_CFG_ID=0`、`DMA_CFG_ID=1`
  - 并且 [prt_dma.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_dma.c) 在 `before-set-dst` / `before-set-src` 日志里会执行：
    - `PRT_PROGRESS_LOG(...)`，其实现包含 `fprintf(stdout, ...)` 和 `fflush(stdout)`
    - `dma_debug_current_cpu()`，其内部调用 `syscall(SYS_getcpu, ...)`
- 同时硬件静态阅读表明：
  - [GemminiCoupledDMA.scala](/home/ubuntu/chipyard/generators/gemmini/src/main/scala/gemmini/GemminiCoupledDMA.scala) 的 `readySrc` 只依赖 `copyReqQ.io.enq.ready`
  - 它不直接看 DMA busy、shared-spad PTW 状态、或目的地址是否已编程
  - 因而如果 CPU 指令真卡在 `hw_dma_set_src(...)`，更像是 stalled 在 ReRoCC client/manager 入口 ready，而不是 CoupledDMA 内部 copy 状态机主动拒收 `SRC_INFO`
- 所以当前新增优先怀疑是：
  - `rr_set_opc` 之后、`set_src` 之前的 syscall/stdio flush 触发了 ReRoCC client 的 `ptbr/status` 更新门控
  - 或者 `cfg0` / `custom2 -> cfg` 这条映射相比 known-good 用例更脆弱
- 下一轮最小验证应优先做：
  - 保留日志内容，但避免在 `set_dst/set_src` 临界区内执行 `getcpu()` 这类 syscall
  - 尽量把 `rr_set_opc -> set_dst -> set_src` 做成和 known-good Linux 自测一样“干净”的序列
  - 再观察 freshest stop point 是否仍是 `before-set-src`

### 4. 2026-03-19 最新 live rerun 的新增证据

- 最新 live rerun：
  - instance `i-03f2c024eac1a8850`
  - runworkload log:
    [2026-03-19--09-20-38-runworkload-YL7VLR5DK8NCVZLR.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-19--09-20-38-runworkload-YL7VLR5DK8NCVZLR.log)
  - terminaterunfarm log:
    [2026-03-19--09-48-58-terminaterunfarm-ACMKWELXW85L9C0O.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-19--09-48-58-terminaterunfarm-ACMKWELXW85L9C0O.log)
- 这轮 run 在回收前已经重新走到：
  - `init load-model-bin end`
  - `init load-input-blob end`
  - `segment=0 begin`
  - `worker stage=0 ready`
  - `rr-acquire armed`
  - `before-set-dst`
  - `submit-fence begin/done`
  - `set-dst done`
- 但与上一轮不同的是：
  - UART 末尾没有完整出现 `before-set-src`
  - 最后字节稳定停在：
    - `...[prt-prog`
  - 也就是下一条 `[prt-progress] ...` 只写出前缀就停住了
- 同时 live `heartbeat.csv` 继续从 `1530s` 量级推进到 `1609s`
- 这说明本轮最新重跑的更直接现象已经变成：
  - 不只是 `set-src done` 缺失
  - 连 `before-set-src` 这条日志本身都没有完整刷到 UART
  - 当前新增 instrumentation 已经足够侵入，以至于 critical path 上的 `stdout/fflush` 本身成为高优先嫌疑
- 因而当前要分开看两层结论：
  - 上一轮较轻日志 rerun 的“语义 freshest stop point”仍然是 `before-set-src`
  - 这一轮较重日志 rerun 的“物理 UART freshest stop point”已经前移到 `set-dst done` 之后的半行 `[prt-prog`
- 用户要求的处理顺序已执行：
  - 先确认这轮 live run 真实卡住
  - 再执行 `terminaterunfarm --forceterminate`
  - EC2 状态已进入 `shutting-down`

## 最新静态检查结论

### 1. 当前这轮实际走的不是 progress-thread DMA，而是 blocking-fence DMA

- [main.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/main.c) 默认把 `dma_backend` 设为 `poll_progress_thread`
- 但 [prt_runtime.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_runtime.c) 在 `spm_xlate_enable` 打开时，会强制把：
  - `sync_mode -> BLOCKING_DEBUG`
  - `dma_backend -> BLOCKING_FENCE`
  - `gemmini_mode -> BLOCKING_FENCE`
- 所以当前 FPGA 卡住时，更应优先怀疑：
  - `dma_blocking_submit()`
  - `hw_dma_fence()`
  - coupled-DMA 硬件内部 copy/fence 路径
- 之前“progress thread 轮询 `hw_done_flag`”这条线，仍然是代码味道，但不是当前这次 stall 的主线

### 2. “local shared scratchpad 上 1-byte TL 访问天然非法”这个怀疑已降级

- [SharedScratchpad.scala](/home/ubuntu/chipyard/generators/gemmini/src/main/scala/gemmini/SharedScratchpad.scala) 的 local bank 通过：
  - `TLRAM(beatBytes = local_bank_beat_bytes)`
  - `TLFragmenter(local_bank_beat_bytes, dma_maxbytes)`
  接到 coupled-DMA
- [SRAM.scala](/home/ubuntu/chipyard/generators/rocket-chip/src/main/scala/tilelink/SRAM.scala) 里的 `TLRAM` 明确声明支持：
  - `supportsGet = TransferSizes(1, beatBytes)`
  - `supportsPutFull = TransferSizes(1, beatBytes)`
  - `supportsPutPartial = TransferSizes(1, beatBytes)`
- [Fragmenter.scala](/home/ubuntu/chipyard/generators/rocket-chip/src/main/scala/tilelink/Fragmenter.scala) 只是不允许“把请求再碎到 sub-beat”；它明确允许：
  - `orig <= min`
  - 也就是本来就是 1B 的请求可以原样通过
- 所以当前更合理的判断是：
  - 1B 路径不是 TL 协议层面“必然非法”
  - 真正值得怀疑的是 coupled-DMA 自己的 1B copy 实现是否有 bug / 长期未覆盖

### 3. 当前 workload 确实会触发 misaligned host->SPM DMA，把 coupled-DMA 推进 1-byte copy 路径

- [prt_types.h](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/include/prt_types.h) 把 `PRT_PAGE_SIZE_BYTES` 固定为 `1024`
- [prt_dma.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_dma.c) 的 Linux `dram->spm` 路径：
  - 只按 host page 边界切 chunk
  - 不做任何 64B 对齐修正
  - 也不保证 `src_pa % 64 == dst_pa % 64`
- 这与 live FPGA 上看到的首个 DMA 请求一致：
  - `src=...410`
  - `dst=...400`
  - `bytes=1024`
- [GemminiCoupledDMA.scala](/home/ubuntu/chipyard/generators/gemmini/src/main/scala/gemmini/GemminiCoupledDMA.scala) 中，只有 `src/dst` 同时 beat 对齐时才走宽拷贝；否则整个请求退化成：
  - `nextXferBytes = 1`
  - `nextLgSize = 0`
- 由于当前首个请求 `src offset = 0x10`、`dst offset = 0x00`，offset 差值恒定，静态上很像会让整段 `1024B` 都走 1-byte `Get/Put`

### 4. 已知 coupled-DMA Linux 自测与当前 workload 覆盖面不一样

- [rerocc_dma_matrix_linux_coupleddma.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests-coupleddma/rerocc_dma_matrix_linux_coupleddma.c) 的 known-good 自测：
  - `src/dst/done` 都用 `mmap`
  - 天然页对齐
  - 每个 chunk 同时受 `src` 和 `dst` 页边界约束
  - `src`/`dst` 往往保持相同页内 offset
- pipeline-runtime 当前不是这种路径：
  - model/input blob 有 `malloc` 路径
  - chunking 只看 host 源页边界
  - 因而更容易把 coupled-DMA 推到 misaligned byte-copy 模式

### 5. 软件侧仍有一个真实差异：submit 前缺少显式 `fence rw, rw`

- [prt_dma.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_dma.c) 在 `tok->hw_done_flag = 0` 后直接发：
  - `hw_dma_set_dst(...)`
  - `hw_dma_set_src(...)`
- 但 known-good Linux DMA test 会在 `*done = 0` 之后显式做：
  - `fence rw, rw`
- 这个差异仍然是合理嫌疑，但当前更像 secondary suspect：
  - 它解释得了“可见性/排序风险”
  - 但不如 “misaligned 1024B 请求把硬件推进 1-byte 路径” 那样直接贴合当前首个 stall 形态
- 当前已将这条差异作为最小 probe 落地到软件里：
  - `dma_blocking_submit()` 现在会先执行 `fence rw, rw`
  - 但在拿到新 FPGA 结果前，不能把它当成已经验证的 fix

### 6. 新增静态判断：stage worker 已绑核，ReRoCC CSR 迁核跑偏不是主嫌疑

- [prt_runtime.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_runtime.c) 的 `stage_bind_current_thread()` 会把每个 stage worker 绑到固定 CPU
- 因此：
  - “同一 stage worker 在 `set_dst` 和 `set_src` 之间迁到别的 hart，导致 `RROPC2` 读到另一套 CSR 状态” 这条怀疑优先级下降
  - 但仍要直接验证：
    - `rr_set_opc()` 之后 `custom2 -> cfg` 映射是否保持不变
    - 对应 `RRCFG<cfg>` 的 acquire bit / manager id 是否仍然匹配
- 所以下一轮最有价值的新证据将是：
  - `rr-acquire armed ...`
  - `before-set-dst ... rr_opc_map / rr_cfg_raw ...`
  - `before-set-src ... rr_opc_map / rr_cfg_raw ...`
- 如果这些值在 `set_dst -> set_src` 之间保持一致，而 UART 仍停在 `before-set-src`：
  - 就更应把主怀疑集中到 Coupled-DMA 的 `SRC_INFO` 接受路径，而不是 ReRoCC CSR 跑偏

## 为下一轮 FPGA 重跑准备好的调试信号

[main.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/main.c) 现在会输出这些最早级别日志：

- `[prt-early] enter main`
- `[prt-early] live stdio ready`
- `[prt-early] defaults ready`
- `[prt-early] arg parse done`
- `[prt-early] calling runtime_init`
- `[prt-early] runtime_init done`

这些日志直接用 `write(2, ...)` 输出，不依赖 stdio buffer。

## 下一步

下一步不是扩语义，而是围绕第一次 DMA submit 做最小闭环调试：

1. 先用最小验证排除 `libguestfs` 的 `/var/tmp` 写权限阻塞，再完成新的 `marshal build` / `marshal install`
2. 回收本轮 run farm，避免空转占机
3. 以当前停点为基准检查：
   - [prt_dma.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_dma.c)
4. 重点看：
   - `blocking_fence` 提交/等待路径
   - coupled-DMA 的 misaligned `1B Get/Put` copy 路径
5. 若需要新增日志，优先围绕：
   - `hw_dma_set_src(...)` 是否真正发出
   - `hw_dma_fence()` 是否在等待 coupled-DMA busy 清零
   - `src/dst/done` 的 `mod64` 与 `full_byte_mode_hint`
6. 不要重新发散回 model / exporter 语义

下一轮最重要的观测点已经更新为：

- FPGA 上是否能越过：
  - `dma-submit token=1 set-dst done ...`
- 如果能越过，下一条应当先出现：
  - `dma-submit token=1 before-set-src ...`
- 如果确认其实已经越过 submit，而是卡在 wait/fence：
  - 优先回头看 coupled-DMA 的 byte-copy 状态机

## 2026-03-19 10:06 UTC 状态同步

- 已按最新要求把“Gemmini / DMA 接口调用问题优先参考 Linux 下三个回归测试写法”同步到：
  - [STATUS.md](/home/ubuntu/chipyard/conference/mudnac_hybridmapper_collab_docs/STATUS.md)
  - [PROCESS.md](/home/ubuntu/chipyard/conference/mudnac_hybridmapper_collab_docs/PROCESS.md)
  - [NEXT_SESSION_PROMPT.md](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/NEXT_SESSION_PROMPT.md)
- 当前用于最小验证的软件改动仍然只有一处：
  - [prt_dma.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_dma.c)
  - 目的：去掉 `set_dst -> set_src` 之间的 syscall-backed 日志干扰，使调用序列更贴近 Linux regression 的 `rr_set_opc -> set_dst -> set_src`
- 这份最小改动对应的真实非沙箱 FireMarshal 已成功完成：
  - build log:
    [rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-build-2026-03-19--10-01-33-YUTYI7W478QTEE8H.log](/home/ubuntu/chipyard/software/firemarshal/logs/rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-build-2026-03-19--10-01-33-YUTYI7W478QTEE8H.log)
  - install log:
    [rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-install-2026-03-19--10-02-07-7WDK7FZEQTM8XDGC.log](/home/ubuntu/chipyard/software/firemarshal/logs/rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-install-2026-03-19--10-02-07-7WDK7FZEQTM8XDGC.log)
- 新一轮 fresh FPGA rerun 也已重新拉起：
  - launchrunfarm log:
    [2026-03-19--10-02-35-launchrunfarm-9IW62XMWRT778YPF.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-19--10-02-35-launchrunfarm-9IW62XMWRT778YPF.log)
  - runfarm instance:
    `i-08c422a15b62a2fc1`
  - 当前 run host private ip:
    `192.168.1.113`
- 当前真实进度不是 `runworkload`，而是：
  - `infrasetup` 正在进行中
  - live log:
    [2026-03-19--10-04-01-infrasetup-9TQBMJ41MDDFTI5D.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-19--10-04-01-infrasetup-9TQBMJ41MDDFTI5D.log)
  - 当前已看到：
    - 远端 host 正在安装 AWS FPGA SDK
    - 清 slot / 重新准备 FPGA 基础设施
  - 暂未看到失败
- 这一轮新的验证目标保持不变：
  - 如果最小改动有效，live UART 停点应当从之前的 `before-set-src` / 半截 `[prt-prog` 继续向后推进
  - 如果仍然停在 `set_src` 邻域，则“critical-path logging intrusion” 可以降级，后续主怀疑继续集中到真实 DMA / coupled-DMA 接口交互

## 2026-03-19 10:14 UTC runworkload live 状态

- 当前 fresh rerun 的真实 `runworkload` 已启动：
  - tmux session:
    `rerocc-prt-runworkload-20260319`
  - pane log:
    [rerocc-prt-runworkload-20260319.pane.log](/home/ubuntu/chipyard/tmp/firesim-aws-f2/tmux/rerocc-prt-runworkload-20260319.pane.log)
  - runworkload log:
    [2026-03-19--10-07-25-runworkload-OTBWWO21YVBP45C8.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-19--10-07-25-runworkload-OTBWWO21YVBP45C8.log)
  - results dir:
    [2026-03-19--10-07-25-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime](/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-19--10-07-25-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime)
- manager 当前仍显示：
  - `1/1 instances are still running`
  - `1/1 simulations are still running`
- live run host 现场：
  - `heartbeat.csv` 已推进到 `3390179376, 380`
  - 但 `uartlog` 当前仍只有 `114` 行
  - 最后一条仍停在：
    - `[    0.000000] software IO TLB: mapped [mem 0x00000000fbfff000-0x00000000fffff000] (64MB)`
- 这个现象目前只记录为：
  - “fresh rerun 还没有越过 Linux 早期 boot 输出空窗”
  - 不是最终卡死结论
- 原因：
  - 用户已明确提醒：Linux 启动时间很长，之前就出现过“看似早停、随后继续往前走”的情况
  - 所以在它真正进入 `S99run` 或长时间保持 `heartbeat` 推进但 `uartlog` 永不变化之前，不把这轮现场重新解释成 DMA submit 停点

## 2026-03-19 10:18 UTC live 纠偏

- 以上 10:14 UTC 的“仍处于 Linux 早期 boot 空窗”判断依然成立，但需要补充一条更重要的 live 事实：
  - 它不是卡死
  - 用户关于 “Linux 启动时间就是很长” 的提醒是对的
- 新抓到的 live 证据：
  - `heartbeat.csv` 已推进到 `6011882029, 660`
  - `uartlog` 已从 `software IO TLB` 继续向前推进到：
    - `Run /init as init process`
    - `Mounting /dev/iceblk as root device`
    - `running /etc/init.d/S01syslogd`
    - `running /etc/init.d/S02klogd`
- 结论更新为：
  - 本轮 fresh rerun 目前仍然是正常慢启动
  - 不能把 `heartbeat` 300 到 400 秒量级而 UART 仍停在早期 boot 的现象直接当成 stuck
  - 真正有价值的下一观察点还是：
    - `S99run`
    - `[bertmini]`
    - `[prt-early]`
    - `[prt-progress]`

## 2026-03-19 10:27 UTC 已重新进入 workload

- 新的 live 证据已经确认本轮 fresh rerun 越过了：
  - `running /etc/init.d/S99run`
  - `launching firemarshal workload run/command`
  - `[bertmini] hugetlb before ...`
  - `[bertmini] hugetlb after ...`
  - `[bertmini] method=ours2 cores=2 gemmini=2 dma=2`
  - `[prt-early] enter main`
  - `[prt-early] runtime_init done`
  - `[prt-early] calling runtime_run`
- 也确认重新进入了 `pipeline-runtime` 的初始化主线：
  - `init page-table end`
  - `init dma-backend end`
  - `init gemmini-backend end`
  - `init trace-calibrate end`
  - `runtime begin`
  - `init load-model-yaml ...`
  - `init load-pipeline-yaml ...`
  - `artifacts mapping load end ... elapsed_ms=547`
- 到这一步为止，可以明确更新判断：
  - boot 不是这轮 fresh rerun 的 blocker
  - 当前 run 重新回到了和前几轮同类的 `pipeline-runtime` 观测窗口
  - 接下来最关键的新观察点重新变回：
    - `segment=0 begin`
    - `submit-fence done`
    - `set-dst done`
    - `set-src done`
    - 或新的更靠后停点

## 2026-03-19 10:35 UTC 当前最新 stuck 证据

- 这轮 fresh rerun 最终已经确认重新打到了第一次 DMA submit 的关键窗口：
  - `segment=0 begin`
  - `worker stage=0 ready ... dma=2`
  - `dma-submit token=1 ... begin`
  - `rr-acquire armed ... cfg=0 raw_cfg=0x102 ...`
  - `dma-submit token=1 acquired cfg=0 manager=2 opcode=2`
  - `dma-submit token=1 before-set-dst ...`
- 然后 live UART 稳定停在半截：
  - `[prt-progress] dma-submit token=`
- 同时 live `heartbeat.csv` 继续从 `1451` 推进到 `1637`
- 并且此时 `uartlog` 行数稳定停在：
  - `827`
- 因此这轮已经满足“真 stuck”判据：
  - 不是 Linux 慢启动
  - 不是 runworkload manager 退出假象
  - 而是 guest 已进入第一次 DMA submit 邻域后，UART 停点稳定不前，而 FPGA 心跳继续推进
- 本轮 run farm 已按约定立即回收：
  - instance:
    `i-08c422a15b62a2fc1`
  - `terminaterunfarm` log:
    [2026-03-19--10-35-03-terminaterunfarm-U00Q39SKFBTD7NWY.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-19--10-35-03-terminaterunfarm-U00Q39SKFBTD7NWY.log)
  - EC2 状态已确认进入：
    `shutting-down`

## 关于“半截日志是不是没 flush” 的最新判断

- 当前判断不是 “stdout 没及时 flush”
- 依据：
  - [prt_progress.h](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/include/prt_progress.h)
    里的 `PRT_PROGRESS_LOG` 每次都会：
    - `fprintf(stdout, ... "\n")`
    - `fflush(stdout)`
  - [main.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/main.c)
    在最早阶段就把：
    - `stdout`
    - `stderr`
    设成 `_IONBF`
- 所以当前半截 UART 更像：
  - 在输出下一条日志时被截断
  - 或程序正好在那条日志附近卡住
  - 而不是整行已经留在用户态 stdio buffer 里没 flush
- 结合当前 [prt_dma.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_dma.c) 的日志顺序：
  - `before-set-dst`
  - `submit-fence begin`
  - `submit-fence done`
  - `hw_dma_set_dst(...)`
  - `hw_dma_set_src(...)`
  - `set-src done`
- 因此这轮 half-line 最接近的解释是：
  - 稳定停点已经逼近 `submit-fence begin / hw_dma_submit_fence()` 之前后这一小段
  - 但仅凭半截 UART，仍不能把语义停点精确断言到 `hw_dma_submit_fence()` 已经执行完

## 2026-03-19 10:39 UTC 下一针最小改动与新 rerun

- 基于上面的新证据，当前新的最小判断是：
  - `hw_dma_submit_fence()` 本身只是普通 `fence rw, rw`
  - 它不应成为会让 guest 长时间卡死的真实硬件点
  - 因而当前半截 UART 更像是 critical-path stdout 干扰仍未完全去干净
- 因此已继续做下一针更窄的最小改动：
  - [prt_dma.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_dma.c)
  - 移除了：
    - `submit-fence begin`
    - `submit-fence done`
  - 现在 `before-set-dst -> submit_fence -> set_dst -> set_src` 整段不再插入 stdout 日志
  - 仍保留：
    - `before-set-dst`
    - `set-src done`
    - `dma-wait fence-enter`
    - `dma-wait fence-done`
- 本地验证已通过：
  - host build:
    `make -C generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime clean all PIPELINE_RUNTIME_PROGRESS=1`
  - Linux cross-build:
    `make -C generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests ... rerocc_pipeline_runtime-linux`
- 真实非沙箱 FireMarshal 也已完成新的 build/install：
  - build log:
    [rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-build-2026-03-19--10-38-01-PZD4X6RXYOS3FF4V.log](/home/ubuntu/chipyard/software/firemarshal/logs/rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-build-2026-03-19--10-38-01-PZD4X6RXYOS3FF4V.log)
  - install log:
    [rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-install-2026-03-19--10-38-51-HNBZN5EY4ELBYC8I.log](/home/ubuntu/chipyard/software/firemarshal/logs/rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-install-2026-03-19--10-38-51-HNBZN5EY4ELBYC8I.log)
- 下一轮 fresh rerun 已重新发起：
  - launchrunfarm log:
    [2026-03-19--10-39-04-launchrunfarm-RYK5ASO49YMAINF4.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-19--10-39-04-launchrunfarm-RYK5ASO49YMAINF4.log)
  - 当前阻塞不是设计问题，而是 AWS 容量：
    - `Tried all subnets, but there was insufficient capacity to launch your instances`
  - manager 正在继续自动重试

## 2026-03-19 11:05 UTC 对 `S10mdev` 长静默的更正

- 当前 active rerun 是：
  - result dir:
    [2026-03-19--10-47-21-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime](/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-19--10-47-21-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime)
  - runworkload log:
    [2026-03-19--10-47-21-runworkload-9QEA9KX75CTOVMAM.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-19--10-47-21-runworkload-9QEA9KX75CTOVMAM.log)
  - instance:
    `i-0dbfa64b1edfad8a5`
  - private ip:
    `192.168.1.123`
- 这轮 run 一度在 live UART 上长时间停在：
  - `running /etc/init.d/S10mdev`
  - `Starting mdev: OK`
- 当时 `heartbeat.csv` 已从大约 `817` 推进到 `925`，容易被误判成 boot hang
- 但随后同一份 live `uartlog` 还是继续前进到了：
  - `running /etc/init.d/S40network`
  - `Starting network: OK`
  - `running /etc/init.d/S99run`
  - `launching firemarshal workload run/command`
- 同时 `heartbeat.csv` 已继续推进到大约 `1093`
- 因此这一轮已经明确修正一个判据：
  - 仅凭 `S10mdev` 之后长时间没有新 UART 行、且 `heartbeat.csv` 继续增长，还不能直接判成“Linux boot 阶段真死锁”
  - 这一路径至少已经证明可能只是超长静默 boot
- 刚才发起过一次 `terminaterunfarm`，但用户中途打断：
  - 该尝试没有把当前 run host 停掉
  - EC2 实例状态复查仍是：
    `running`
- 截至这次记录时，当前 active rerun 的最新 live 结论变成：
  - boot 已再次越过 `S99run`
  - 当前正在等待重新进入：
    - `[bertmini]`
    - `[prt-early]`
    - `segment=0 begin`
    - 第一条 `dma-submit`
  - 所以下一个真正有价值的判断点，重新回到 workload / pipeline-runtime，而不是早期 boot

## 2026-03-19 11:14 UTC 当前最新 DMA 停点已更新

- 上一节提到的 active rerun
  [2026-03-19--10-47-21-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime](/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-19--10-47-21-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime)
  最终已经重新走到：
  - `artifacts validate end`
  - `init load-model-bin end elapsed_ms=783 size=17055744`
  - `init load-input-blob end elapsed_ms=5 size=65536`
  - `segment=0 begin`
  - `worker stage=0 ready`
  - 第一条 `dma-submit`
  - `rr-acquire armed`
  - `dma-submit token=1 acquired cfg=0 manager=2 opcode=2`
- 当前这轮最新稳定可见的 live UART 末尾不再是 `before-set-src`，而是更早停在：
  - 半截 `before-set-dst`：
    `[prt-progress] dma-submit token=1 before-set-dst done_va=0x3fb05b03e8 done_pa=0x105cee3e8 done_mod64=0x28 done_pa_rc=0(ok) src_mod64=0x10 dst_mod64=0x00 initial_wide_hi`
- 同时这条半截行稳定不再增长，而：
  - `uartlog` 行数固定在 `826`
  - `heartbeat.csv` 继续从大约 `1422` 增长到 `1491`
- 因而这次已经满足新的“真 stuck”判据：
  - 不是 boot 长静默
  - 不是 artifact validate / model bin / input blob 加载阶段
  - 而是在第一次 DMA submit 关键区里，稳定停在 `before-set-dst` 这条 progress log 自身附近
- 这说明当前最新代码版本下的 freshest evidence 是：
  - 删除 `submit-fence begin/done` 之后，停点没有继续后移到 `set-src done` 或 `dma-wait`
  - 反而进一步暴露出：
    - `before-set-dst` 这条超宽 `PRT_PROGRESS_LOG(...)`
    - 以及它前面的 `dma_debug_current_cpu()` / `getcpu`
    很可能仍然过于侵入 critical path
- 本轮 run farm 已按流程回收：
  - terminaterunfarm log:
    [2026-03-19--11-14-22-terminaterunfarm-P4UDIWS6XSQZWFUK.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-19--11-14-22-terminaterunfarm-P4UDIWS6XSQZWFUK.log)
  - instance:
    `i-0dbfa64b1edfad8a5`
  - EC2 状态已确认进入：
    `shutting-down`
- 当前最新最小改动建议也随之更新为：
  - 先不要继续在 `set_dst/set_src` 临界区加宽日志
  - 下一针应优先去掉或极限瘦身：
    - `before-set-dst` 的长格式 progress log
    - `dma_debug_current_cpu()` 这类 critical-path syscall
  - 然后再重新 build/install/infrasetup/runworkload，看停点是否重新回到完整 `before-set-dst`、或继续后移到 `set_src`/`dma-wait`

## 2026-03-19 12:10 UTC 去掉 `before-set-dst`/`getcpu` 后的最新结果

- 已按上一节建议做最小改动：
  - [prt_dma.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_dma.c)
  - 删除了：
    - `before-set-dst` 这条超宽 `PRT_PROGRESS_LOG(...)`
    - `dma_debug_current_cpu()` / `getcpu`
- 本地验证通过后，已完成新的真实环境重打镜像：
  - build log:
    [rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-build-2026-03-19--11-20-51-IC5ZJ4BGWIZMAZZI.log](/home/ubuntu/chipyard/software/firemarshal/logs/rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-build-2026-03-19--11-20-51-IC5ZJ4BGWIZMAZZI.log)
  - install log:
    [rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-install-2026-03-19--11-23-53-PXKBPSHRTGK83IFG.log](/home/ubuntu/chipyard/software/firemarshal/logs/rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-install-2026-03-19--11-23-53-PXKBPSHRTGK83IFG.log)
- 随后新一轮标准流程已完成到 `runworkload`：
  - launchrunfarm log:
    [2026-03-19--11-24-39-launchrunfarm-WF3327MAJSHUBJVD.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-19--11-24-39-launchrunfarm-WF3327MAJSHUBJVD.log)
  - infrasetup log:
    [2026-03-19--11-30-26-infrasetup-YUM92NI7TCVRT8AN.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-19--11-30-26-infrasetup-YUM92NI7TCVRT8AN.log)
  - runworkload log:
    [2026-03-19--11-44-42-runworkload-0DVPOL3ZR8J230V2.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-19--11-44-42-runworkload-0DVPOL3ZR8J230V2.log)
  - result dir:
    [2026-03-19--11-44-42-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime](/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-19--11-44-42-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime)
- 本轮 run host：
  - instance:
    `i-0ba7379b03ec31bb3`
  - private ip:
    `192.168.1.172`
- 这轮 fresh rerun 再次确认已经走到：
  - `artifacts validate end`
  - `init load-model-bin end`
  - `init load-input-blob end`
  - `segment=0 begin`
  - `worker stage=0 ready`
  - 第一条 `dma-submit`
  - `rr-acquire armed ...`
- 但最新稳定可见 live UART 末尾进一步前移成：
  - 半截：
    `[prt-progress] dma`
- 同时：
  - `uartlog` 行数固定在 `825`
  - `heartbeat.csv` 继续从大约 `1288` 增长到 `1489`
- 结合当前代码顺序，这里的解释需要明确标注为“按源代码顺序推断”：
  - `rr-acquire armed ...` 之后，下一条本应是
    `dma-submit token=1 acquired cfg=0 manager=2 opcode=2`
  - 因而这轮 freshest stop point 很可能已经前移到：
    - `acquired` 这条 progress log 自身
    - 而不是之前的 `before-set-dst`
- 这说明：
  - 去掉 `before-set-dst` / `getcpu` 还不够
  - 目前任何位于 `rr-acquire` 之后、`set_dst/set_src` 之前的 `PRT_PROGRESS_LOG(...)`
    仍然可能过于侵入 critical path
- 本轮 run farm 已按约定回收：
  - terminaterunfarm log:
    [2026-03-19--12-10-05-terminaterunfarm-9PK9B1KG9WRVKZ4W.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-19--12-10-05-terminaterunfarm-9PK9B1KG9WRVKZ4W.log)
  - EC2 状态已确认进入：
    `shutting-down`
- 当前新的最小改动建议更新为：
  - 下一针继续去掉或极限瘦身
    `dma-submit token=... acquired ...`
    这条 log
  - 尽量让
    `rr-acquire -> submit_fence -> set_dst -> set_src`
    中间不再插入 userspace `fprintf/fflush`
  - 然后再重跑，观察 freshest stop point 是否终于跨过 `acquired`

## 2026-03-19 12:15 UTC 已落地但尚未重新打镜像的下一针

- 上一节建议的最小改动已直接落地到：
  [prt_dma.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_dma.c)
- 本次新增移除的是：
  - `dma-submit token=... acquired cfg=... manager=... opcode=...`
    这条 progress log
- 这样当前 `rr-acquire` 之后、`set_dst/set_src` 之前又少了一条 userspace `fprintf/fflush`
- 对应本地最小验证已通过：
  - host build:
    `make -C generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime clean all PIPELINE_RUNTIME_PROGRESS=1`
  - RISC-V Linux build:
    `make -C generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests ... rerocc_pipeline_runtime-linux`
- 截至这次记录时：
  - 这针改动已经在源码中
  - 但还没有再次执行新的 `marshal build/install` 和 FPGA rerun
- 所以下一轮最直接的工作就是：
  - 用真实环境重新 `marshal build`
  - `marshal install`
  - `launchrunfarm -> infrasetup -> runworkload`
  - 看 freshest stop point 是否越过 `acquired`

## 2026-03-19 12:35 UTC 新实验进行中 + 新静态判断

- 以上“去掉 `acquired` progress log”这针之后，真实环境重打镜像已经完成：
  - build log:
    [rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-build-2026-03-19--12-20-45-J3KU3HJBZPB16L8R.log](/home/ubuntu/chipyard/software/firemarshal/logs/rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-build-2026-03-19--12-20-45-J3KU3HJBZPB16L8R.log)
  - install log:
    [rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-install-2026-03-19--12-25-26-WR0IQS9CPUBPPOIF.log](/home/ubuntu/chipyard/software/firemarshal/logs/rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-install-2026-03-19--12-25-26-WR0IQS9CPUBPPOIF.log)
- 新一轮标准流程已经进入：
  - launchrunfarm log:
    [2026-03-19--12-25-38-launchrunfarm-9TU1MK144BT1HRGI.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-19--12-25-38-launchrunfarm-9TU1MK144BT1HRGI.log)
  - infrasetup log:
    [2026-03-19--12-26-14-infrasetup-CJL2QF0LRKN050MV.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-19--12-26-14-infrasetup-CJL2QF0LRKN050MV.log)
  - tmux pane:
    [rerocc-prt-infrasetup-20260319e.pane.log](/home/ubuntu/chipyard/tmp/firesim-aws-f2/tmux/rerocc-prt-infrasetup-20260319e.pane.log)
- 当前 run host：
  - instance:
    `i-0f287e0d6f70d49ec`
  - private ip:
    `192.168.1.45`
- 目前 `infrasetup` 不是假死：
  - manager log 已明确显示远端正在执行
    `git clone https://github.com/firesim/aws-fpga-firesim-f2.git aws-fpga`
  - clone 接收进度已持续推进到 `90%+`
  - 所以当前状态仍然是“等待远端 SDK clone/部署完成”，还没进入新的 `runworkload`

- 本轮并行静态检查新增两个更窄的判断：

- 判断 1：
  - `GemminiCoupledDMA` 的软件接口本质上是：
    - `funct=2` 只把 `dst_addr/completion_addr` 写到内部寄存器
    - `funct=1` 再把 `src_addr/len` 入队
    - 之后硬件状态机独立搬运并在结束时写 completion flag
  - 这意味着单从硬件接口形态看，
    `set_dst -> set_src`
    中间“有一点时间间隔”本身不自动等价于硬件协议错误
  - 所以目前更需要优先怀疑的是：
    - runtime 在这段临界路径里插入的 userspace 行为
    - 尤其是 `PRT_PROGRESS_LOG(...)`
    - 而不是先假设硬件要求两条指令必须零间隔相邻

- 判断 2：
  - `PRT_PROGRESS_LOG(...)` 当前实现仍是：
    - `fprintf(stdout, ...)`
    - `fflush(stdout)`
  - 并且在 `prt_dma.c` 里已经能确认至少存在一类高风险调用：
    - DMA progress thread 持有 `rt->dma_pending_lock`
    - 同时也持有 `tok->lock`
    - 期间仍可能进入 `dma_log_pending_state(...)`
    - 再走到 `PRT_PROGRESS_LOG(...)`
  - 这说明“stdout 路径阻塞”不仅会拖慢打印，
    还可能被放大成真正的线程/锁级停顿
  - 因而当前根因方向继续收敛为：
    - 不是简单的“没 flush”
    - 而是 `stdout + fflush + 多线程 + 持锁区日志` 这组组合过于侵入

- 下一步保持不变：
  - 等当前 `infrasetup` 完成
  - 立刻启动这轮新的 `runworkload`
  - 核心观测点仍是：
    在移除 `acquired` log 后，freshest stop point 是否终于越过之前的半截
    `[prt-progress] dma`

## 2026-03-19 12:50 UTC 当前 live run 状态

- 上一节里的 `infrasetup` 已确认成功结束：
  - `rerocc-prt-infrasetup-20260319e.exitcode = 0`
- 随后已按真实环境流程启动新的 `runworkload`：
  - tmux session:
    `rerocc-prt-runworkload-20260319e`
  - manager log:
    [2026-03-19--12-39-50-runworkload-ZHSWWPICUGO8HY5O.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-19--12-39-50-runworkload-ZHSWWPICUGO8HY5O.log)
  - result dir:
    [2026-03-19--12-39-50-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime](/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-19--12-39-50-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime)
- 当前 run host 仍是：
  - instance:
    `i-0f287e0d6f70d49ec`
  - private ip:
    `192.168.1.45`
- 截至当前记录：
  - manager 端持续显示 simulation running
  - remote `heartbeat.csv` 已持续增长到大约 `400s`
  - `uartlog` 尚未出现：
    - `S99run`
    - `[bertmini]`
    - `[prt-early]`
    - `[prt-progress]`
- 但这不是新的卡死结论；按 live UART 最新内容，只能记录为：
  - guest Linux 仍停留在早期 boot 输出窗口
  - 最新稳定可见末尾是：
    `[    0.000000] software IO TLB: mapped [mem 0x00000000fbfff000-0x00000000fffff000] (64MB)`
  - 这正是用户此前特别提醒过“不能过早误判卡死”的位置
- 因此当前最准确的说法是：
  - 新实验已经成功启动并在持续推进
  - 但还没越过 Linux boot 到 `bertmini` / `pipeline-runtime`
  - 现在应继续耐心等待，不要因为长静默窗口提前终止 run farm

## 2026-03-19 13:15 UTC 当前 run 已确认真卡死并已回收

- 在继续等待后，这轮 run 最终不是停在 Linux boot：
  - live `uartlog` 已明确越过 boot，重新进入：
    - `segment=0 flush-spm-xlate end`
    - `segment=0 begin`
    - `worker stage=0 ready`
    - `dma-submit token=1 ... begin`
  - 但最后稳定可见的末尾变成了半截：
    - `[prt-progress] rr-acquire armed stage=0 manager=2 opcode`
- 关键判定证据是：
  - `uartlog` 连续两次检查都保持：
    - `824` 行不变
  - 同一时间段内 `heartbeat.csv` 从大约：
    - `1834s`
    - 增长到
    - `1903s`
  - 这满足用户此前要求的“UART 已稳定，但 heartbeat 继续前进”的真卡死判据
- 这轮最新 stop point 相比上一轮又更具体了一点：
  - 上一轮在移除 `acquired` log 之前，最新现场是：
    - full `rr-acquire armed ...`
    - 后面跟着半截 `[prt-progress] dma`
  - 这一轮在移除 `acquired` log 之后，半截直接前移到了：
    - `rr-acquire armed ...` 这条 log 自身
- 这强化了当前主判断：
  - 不是“没有 flush”
  - 也不再是 Linux boot 早期问题
  - 而是 `rr-acquire` 后这类 critical path progress log 本身依旧过于侵入
  - 特别是半截行再次说明执行很可能停在 log write 自身或其极近邻，而不是停在一条已经完整写完的后续语句
- 按既定流程，本轮 run farm 已先回收：
  - terminaterunfarm log:
    [2026-03-19--13-15-29-terminaterunfarm-DR6TGLF9XXYYPRQS.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-19--13-15-29-terminaterunfarm-DR6TGLF9XXYYPRQS.log)
  - instance:
    `i-0f287e0d6f70d49ec`
  - 当前 EC2 状态已确认进入：
    `shutting-down`
- 因而此刻最小下一针已经进一步收敛成：
  - 不再优先盯 `dma-submit ... acquired ...`，因为它已经删掉了
  - 下一目标应是继续减小或绕开：
    - `prt_rr_acquire_scope()` 里的 `rr-acquire armed ...`
      这条 progress log
  - 保持
    `rr_acquire -> rr_set_opc -> set_dst -> set_src`
    周围没有 `fprintf/fflush`

## 2026-03-19 13:35 UTC 新最小实验已完成，现象已改变

- 本轮最小代码变化是：
  - 从
    [prt_rerocc.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_rerocc.c)
    删除了 `rr_set_opc(...)` 之后那条
    `rr-acquire armed ...`
    progress log
- 该源码状态在真实环境中已重新完成：
  - FireMarshal build:
    [rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-build-2026-03-19--13-21-57-NVD171NHBMBLGJCG.log](/home/ubuntu/chipyard/software/firemarshal/logs/rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-build-2026-03-19--13-21-57-NVD171NHBMBLGJCG.log)
  - FireMarshal install:
    [rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-install-2026-03-19--13-22-18-CFMRC8TJZD1LH6KN.log](/home/ubuntu/chipyard/software/firemarshal/logs/rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-install-2026-03-19--13-22-18-CFMRC8TJZD1LH6KN.log)
  - launchrunfarm:
    [2026-03-19--13-22-45-launchrunfarm-324FHKQLM414K16C.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-19--13-22-45-launchrunfarm-324FHKQLM414K16C.log)
  - infrasetup:
    [2026-03-19--13-23-46-infrasetup-B0BNCLFZ7OACL35T.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-19--13-23-46-infrasetup-B0BNCLFZ7OACL35T.log)
    且 tmux exit code 已确认是 `0`
- 新 runworkload：
  - log:
    [2026-03-19--13-28-38-runworkload-LXPXJP14YYNQCLV9.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-19--13-28-38-runworkload-LXPXJP14YYNQCLV9.log)
  - result dir:
    [2026-03-19--13-28-38-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime](/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-19--13-28-38-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime)
  - run host:
    - instance `i-0e0ef189cc43eac9e`
    - private ip `192.168.1.138`
- 这次没有再进入之前那个用户态 DMA 停点：
  - `uartlog` 总共只有 `114` 行
  - 最后只到 Linux 早期 boot：
    - `Initmem setup node 0 ...`
  - 随后 simulator 直接打印：
    - `Simulator deadlock detected at target cycle 0. Terminating.`
    - `*** FAILED *** (code = 1) after 0 cycles`
- 但这个 `target cycle 0` 不能按字面理解成“完全没跑”：
  - 同一轮 live `heartbeat.csv` 先连续推进到：
    - `1421084536, 163`
  - 然后才变成：
    - `0, 171`
    - `0, 176`
  - 所以更像是：
    - `clock.tcycle()` 在 run 末期被重置成 `0`
    - heartbeat bridge 随后按 deadlock 规则报错
  - 而不是 target 从一开始就 `0 cycle`
- 这说明本轮最新现象已经和上一轮不同：
  - 上一轮是 Linux boot 后进入 `segment=0 begin` / 第一条 DMA 附近，再在半截 progress log 处真卡死
  - 这一轮是在 Linux 更早阶段直接进入 simulator-level deadlock / clock reset 症状
- 因而当前判断要更新成两层：
  - 判断 1：
    - 删除 `rr-acquire armed ...` 这条 log 确实改变了故障形态
    - 但这轮没有到达之前的用户态 stop point，所以它还不能直接证明老的 DMA critical-path 假设已经消失
  - 判断 2：
    - 现在新增了一个独立需要追的方向：
      FireSim/bridge 视角的 `tcycle -> 0 -> deadlock`
    - 这个症状比单纯 “没 flush” 更底层，至少说明这轮失败不只是 `rr_acquire -> set_src` 附近的一条 guest 日志被卡住
- 本轮 run farm 已按流程回收：
  - terminaterunfarm:
    [2026-03-19--13-34-44-terminaterunfarm-G6GSWWFLULX9ARGN.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-19--13-34-44-terminaterunfarm-G6GSWWFLULX9ARGN.log)
  - instance `i-0e0ef189cc43eac9e` 已确认进入：
    `shutting-down`

## 当前下一步判断

- 继续保留原先的软件侧主线：
  - `PRT_PROGRESS_LOG(stdout + fflush)` 仍然是高风险侵入项
  - Gemmini / DMA 调用接口问题仍优先参考 Linux 三个回归测试
- 但在下一轮实验前，必须同时补上新的 simulator-level 分析：
  - 为什么本轮 `heartbeat` 先增长、后 `tcycle` 归零
  - 为什么 `heartbeat.cc` 最终在 `current_cycle == 0` 时触发 deadlock
  - 这是否和当前 image / boot path / bridge 状态有关，而不只是用户态 `pipeline-runtime` 临界区

## 2026-03-19 14:15 UTC 同镜像第二次复跑已回到稳定 DMA 停点

- 为了区分“偶发平台态”与“稳定 workload 停点”，在完全不改源码、不重装镜像的前提下，又做了一次同镜像复跑：
  - launchrunfarm:
    [2026-03-19--13-41-13-launchrunfarm-IL0IY3CCYU5G9DZV.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-19--13-41-13-launchrunfarm-IL0IY3CCYU5G9DZV.log)
  - infrasetup:
    [2026-03-19--13-41-52-infrasetup-IP4PRS4XMQUCHE9X.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-19--13-41-52-infrasetup-IP4PRS4XMQUCHE9X.log)
  - runworkload:
    [2026-03-19--13-47-50-runworkload-FRKDZBN8N50QLC4W.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-19--13-47-50-runworkload-FRKDZBN8N50QLC4W.log)
  - result dir:
    [2026-03-19--13-47-50-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime](/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-19--13-47-50-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime)
  - run host:
    - instance `i-073b0c6f3ca6cd3b2`
    - private ip `192.168.1.140`
- 这次关键对比结果非常明确：
  - 上一轮那个 Linux 早期
    `Simulator deadlock detected at target cycle 0`
    没有复现
  - 同镜像第二次复跑正常越过：
    - `S99run`
    - `[bertmini]`
    - `[prt-early] runtime_init done`
    - `artifacts validate end`
    - `init load-model-bin end`
    - `init load-input-blob end`
    - `segment=0 begin`
    - `worker stage=0 ready`
- 它随后重新稳定卡回了第一条 `dma-submit` progress log 本身：
  - 当前 live UART 最后稳定半行是：
    - `[prt-progress] dma-submit token=1 stage=0 tensor=0 bytes=1024 src=0x104afe410 dst=0x40008400 src_acc=2 dst_acc=2 src_mod64=0x10 dst_mod64=0x00 initial_wide_hint=0 full_`
  - 对应 `uartlog` 行数在两次检查间都保持：
    - `823`
  - 同时 `heartbeat.csv` 从大约：
    - `1437s`
    - 增长到
    - `1633s`
- 这次比上一轮更有力，因为现在可以把几个方向重新排序：
  - 判断 1：
    - 13:28 那轮 Linux 早期 `tcycle -> 0 -> deadlock` 目前应降级为偶发平台态/桥侧症状
    - 它不是这份 workload image 的稳定主 blocker
  - 判断 2：
    - 当前稳定主 blocker 重新回到第一条 `dma-submit` 的长 progress log
    - 而且这条 log 在
      [prt_dma.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_dma.c)
      里位于：
      - `dma_blocking_submit()` 最开头
      - `prt_rr_acquire_scope(...)` 之前
      - `hw_dma_submit_fence()/set_dst/set_src` 之前
    - 所以这次稳定现场更强地支持：
      - 先卡在 `PRT_PROGRESS_LOG` 自身或其 `stdout/fflush` 路径
      - 而不是先卡在 `rr_acquire` / `set_dst` / `set_src`
- 因而当前更准确的优先级应调整为：
  - 第一优先：
    - 继续最小化或改写第一条 `dma-submit ... begin` progress log
    - 尤其避免这条超长 `fprintf(stdout)+fflush(stdout)` 出现在 DMA submit 前
  - 第二优先：
    - 保留对 Linux regression 调用序列的参考约束
  - 第三优先：
    - 把 `tcycle -> 0` 当作偶发平台态继续记录，但不再把它当主线 blocker
- 本轮 run farm 已按流程回收：
  - terminaterunfarm:
    [2026-03-19--14-15-32-terminaterunfarm-1QYMFSFUN07NUEOA.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-19--14-15-32-terminaterunfarm-1QYMFSFUN07NUEOA.log)
  - instance `i-073b0c6f3ca6cd3b2` 当前已确认进入：
    `shutting-down`

## 2026-03-19 14:20 UTC 最新源码状态已前进一步

- 基于上面第二次复跑的稳定证据，已经对
  [prt_dma.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_dma.c)
  再做了一个最小代码改动：
  - 删除了 `dma_blocking_submit()` 开头那条超长
    `dma-submit ... initial_wide_hint=... full_byte_mode_hint=... begin`
    progress log
- 这一步的目的不是改 DMA 调用语义，而是只验证：
  - 去掉这条发生在
    `prt_rr_acquire_scope()` /
    `hw_dma_submit_fence()` /
    `set_dst` /
    `set_src`
    之前的长 `stdout+fflush` 日志后，stable stop point 是否还能停在同一位置
- 本地最小验证已通过：
  - host build:
    `make -C generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime clean all PIPELINE_RUNTIME_PROGRESS=1`
  - RISC-V Linux build:
    `make -C generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests -f generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests/Makefile abs_top_srcdir=/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests CC_LINUX=/home/ubuntu/chipyard/.conda-env/riscv-tools/bin/riscv64-unknown-linux-gnu-gcc PIPELINE_RUNTIME_PROGRESS=1 rerocc_pipeline_runtime-linux`
  - 结果：
    - host build 通过
    - RISC-V Linux 交叉编译通过
- 但注意：
  - 这个最新源码状态还没有重新 `marshal build/install`
  - 所以上一节 FPGA 证据仍然对应“删了 `rr-acquire armed ...`，但还没删第一条 `dma-submit ... begin`”那一版镜像

## 2026-03-19 15:10 UTC 最新收敛状态

- 上一节之后，已经在真实非沙箱环境中重新完成过一轮：
  - `marshal build`
  - `marshal install`
  - `launchrunfarm`
  - `infrasetup`
  - `runworkload`
  - `terminaterunfarm`
- 对应最新一轮完整 real-environment 结果：
  - build log:
    [rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-build-2026-03-19--14-27-13-19Z81FH5EL3S3N0G.log](/home/ubuntu/chipyard/software/firemarshal/logs/rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-build-2026-03-19--14-27-13-19Z81FH5EL3S3N0G.log)
  - install log:
    [rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-install-2026-03-19--14-27-46-5PYW6NEFRNC0O4IT.log](/home/ubuntu/chipyard/software/firemarshal/logs/rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-install-2026-03-19--14-27-46-5PYW6NEFRNC0O4IT.log)
  - launchrunfarm:
    [2026-03-19--14-28-05-launchrunfarm-J3RLG9O7Q8MEB45D.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-19--14-28-05-launchrunfarm-J3RLG9O7Q8MEB45D.log)
  - infrasetup:
    [2026-03-19--14-28-30-infrasetup-EDLWABEDWLAR9JHQ.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-19--14-28-30-infrasetup-EDLWABEDWLAR9JHQ.log)
  - runworkload:
    [2026-03-19--14-35-31-runworkload-DR6N3HI83I2HRX1R.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-19--14-35-31-runworkload-DR6N3HI83I2HRX1R.log)
  - result dir:
    [2026-03-19--14-35-31-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime](/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-19--14-35-31-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime)
  - terminaterunfarm:
    [2026-03-19--15-02-23-terminaterunfarm-USLLUV2327W1KXAS.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-19--15-02-23-terminaterunfarm-USLLUV2327W1KXAS.log)
- 这轮最关键的新证据是：
  - 之前那次 Linux 早期
    `Simulator deadlock detected at target cycle 0`
    没有复现
  - 删掉第一条超长 `dma-submit ... begin` log 后，guest 明显向前推进到了：
    - `S99run`
    - `[bertmini]`
    - `[prt-early] runtime_init done`
    - `artifacts validate end`
    - `init load-model-bin end`
    - `init load-input-blob end`
    - `init ready`
    - `segment=0 begin`
    - `worker stage=0 ready`
- 随后真实稳定停点也前移了：
  - `uartlog` 行数稳定在 `823`
  - `heartbeat.csv` 继续从大约 `1356s` 推进到 `1560s`
  - 当前最后一条稳定 UART 行是：
    - `worker stage=0 ready ... dma=2 tiles=2`
- 因而当前判断已更新为：
  - “第一条超长 `dma-submit ... begin` log 自身是稳定 blocker” 这件事已经得到强支持
  - 但它不是全部根因，因为删掉它后程序确实继续前进，只是停点前移到了第一次 DMA 提交之前更靠后的区域
  - 当前更像是停在：
    - `worker stage=0 ready`
    之后
    - `dma_blocking_submit()`
    之内
    - 且在下一个可见 log 之前
- 结合当前代码顺序，最有价值的下一个 narrowing 是：
  - `prt_rr_acquire_scope(...)` 之前
  - `prt_rr_acquire_scope(...)` 内部
  - acquire 成功后、DMA 编程前
  - `submit_fence/set_dst/set_src` 里
- 因此已经再做一轮最小源码更新：
  - [prt_progress.h](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/include/prt_progress.h)
    新增 `PRT_PROGRESS_RAW_LINE(...)`
    直接用 `write(STDERR_FILENO, ...)` 打很短的 raw marker
  - [prt_dma.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_dma.c)
    新增：
    - `[prt-raw] dma pre-acquire`
    - `[prt-raw] dma acquire-ok`
    - `[prt-raw] dma pre-program`
  - 同时删掉了 `set_dst -> set_src` 中间的 CSR debug read，并缩短后续日志字段，尽量贴近 Linux coupleddma 回归测试的干净调用序列
- 这轮最新最小改动的本地验证现已通过：
  - host build:
    `make -C generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime clean all PIPELINE_RUNTIME_PROGRESS=1`
  - RISC-V Linux build:
    `make -C generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests -f generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests/Makefile abs_top_srcdir=/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests CC_LINUX=/home/ubuntu/chipyard/.conda-env/riscv-tools/bin/riscv64-unknown-linux-gnu-gcc PIPELINE_RUNTIME_PROGRESS=1 rerocc_pipeline_runtime-linux`
  - 结果：
    - host build 通过
    - RISC-V Linux 交叉编译通过
- 但注意当前还没有完成的是：
  - 把这版 raw-marker 源码重新 `marshal build/install`
  - 再做新的
    `launchrunfarm -> infrasetup -> runworkload`
- 所以下一步应直接执行：
  - 在真实非沙箱环境里：
    - `source /home/ubuntu/chipyard/env.sh`
    - `cd /home/ubuntu/chipyard/sims/firesim`
    - `source sourceme-manager.sh`
  - 然后：
    - `marshal build`
    - `marshal install`
    - 新一轮 FireSim FPGA 重跑

## 2026-03-19 15:55 UTC 最新 real-environment FPGA 结果

- 上一节提到的 raw-marker 源码版本，现已在真实非沙箱环境中完整跑过一轮：
  - build log:
    [rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-build-2026-03-19--15-09-48-13N9052K6C0HBD89.log](/home/ubuntu/chipyard/software/firemarshal/logs/rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-build-2026-03-19--15-09-48-13N9052K6C0HBD89.log)
  - install log:
    [rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-install-2026-03-19--15-10-20-TM7GIZPOADVIYWV9.log](/home/ubuntu/chipyard/software/firemarshal/logs/rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-install-2026-03-19--15-10-20-TM7GIZPOADVIYWV9.log)
  - launchrunfarm:
    [2026-03-19--15-13-38-launchrunfarm-FV8FKGPEQGL344T6.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-19--15-13-38-launchrunfarm-FV8FKGPEQGL344T6.log)
  - infrasetup:
    [2026-03-19--15-14-03-infrasetup-RCD8RTC3UME6FFWB.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-19--15-14-03-infrasetup-RCD8RTC3UME6FFWB.log)
  - runworkload:
    [2026-03-19--15-24-42-runworkload-7N0BWOO04AG25KVE.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-19--15-24-42-runworkload-7N0BWOO04AG25KVE.log)
  - result dir:
    [2026-03-19--15-24-42-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime](/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-19--15-24-42-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime)
  - terminaterunfarm:
    [2026-03-19--15-52-24-terminaterunfarm-WAHTOWWR7DVQMI2U.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-19--15-52-24-terminaterunfarm-WAHTOWWR7DVQMI2U.log)
- 本轮 run host：
  - instance `i-0e331841de6639b5e`
  - private ip `192.168.1.125`
  - 已确认进入 `shutting-down`
- 这轮和之前一样，Linux boot 早期长时间沉默并不是 blocker：
  - 它正常越过了
    - `S10mdev`
    - `S40network`
    - `S99run`
    - `[bertmini]`
    - `[prt-early] runtime_init done`
    - `artifacts validate end`
    - `init load-model-bin end`
    - `init load-input-blob end`
    - `init ready`
    - `segment=0 begin`
    - `worker stage=0 ready`
- 本轮最关键的新证据是：
  - 新增 raw marker 已经真实出现在 FPGA 上：
    - `[prt-raw] dma pre-acquire`
    - `[prt-raw] dma acquire-ok`
    - `[prt-raw] dma pre-program`
  - 之后 UART 再也没有新增
- 这是目前最重要的收敛：
  - `prt_rr_acquire_scope(...)` 明确已经返回成功
  - 停点不再是 acquire 之前
  - 停点也不再只是 “`worker stage=0 ready` 后没任何 DMA 证据”
  - 当前稳定停点已经进一步前移到：
    - `dma pre-program`
    之后
    - 即 `hw_dma_submit_fence() / hw_dma_set_dst() / hw_dma_set_src()`
    这一小段临界区内
- 当前稳定现场的定量证据：
  - `uartlog` 行数两次检查都固定在：
    - `826`
  - 最后稳定 tail 为：
    - `worker stage=0 ready ... dma=2 tiles=2`
    - `[prt-raw] dma pre-acquire`
    - `[prt-raw] dma acquire-ok`
    - `[prt-raw] dma pre-program`
  - 同时 `heartbeat.csv` 继续从大约：
    - `1487s`
    - 增长到
    - `1635s`
- 因而当前最新判断应更新为：
  - 之前“可能在 `prt_rr_acquire_scope(...)` 里停住”的怀疑已经显著降级
  - 现在主怀疑更集中到：
    - `hw_dma_submit_fence()`
    - `hw_dma_set_dst(...)`
    - `hw_dma_set_src(...)`
    - 或它们触发的紧邻硬件交互
- 还需要保留一条谨慎解释：
  - 由于 `dma pre-program` 之后目前没有更窄 marker，
    现在还不能仅靠这轮日志区分：
    - 卡在 `submit_fence`
    - 卡在 `set_dst`
    - 卡在 `set_src`
  - 但至少已经可以明确排除：
    - 卡在第一条 `dma-submit ... begin` 长日志
    - 卡在 `rr-acquire` 之前
    - 卡在 `rr-acquire` 返回之前
- 下一轮最小 narrowing 应优先保持现有调用序列不变，只补最短 raw marker：
  - `pre-fence`
  - `post-fence`
  - `post-set-dst`
  - `post-set-src`
- 仍然保留用户约束：
  - 任何 Gemmini / DMA 接口调用问题，优先参考 Linux 下三个 coupleddma 回归测试写法
  - 尤其保持 `rr_set_opc -> set_dst -> set_src` 临界区尽可能干净

## 2026-03-19 16:00 UTC 下一轮更窄实验已启动

- 基于上一节最新结论，已对
  [prt_dma.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_dma.c)
  再做一轮更窄的最小改动：
  - 保留现有
    - `dma pre-acquire`
    - `dma acquire-ok`
    - `dma pre-program`
  - 新增：
    - `dma post-fence`
    - `dma post-dst`
    - `dma post-src`
  - 同时删除了 `set_src` 之后那条长 `stdout/fflush` progress log，避免它在下一轮重新成为 instrumentation 干扰
- 这轮源码的本地最小验证已通过：
  - host build:
    `make -C generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime clean all PIPELINE_RUNTIME_PROGRESS=1`
  - RISC-V Linux build:
    `make -C generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests -f generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests/Makefile abs_top_srcdir=/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests CC_LINUX=/home/ubuntu/chipyard/.conda-env/riscv-tools/bin/riscv64-unknown-linux-gnu-gcc PIPELINE_RUNTIME_PROGRESS=1 rerocc_pipeline_runtime-linux`
- 新镜像也已在真实非沙箱环境中重新：
  - build:
    [rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-build-2026-03-19--15-55-36-Y5OMV91059DLZSK5.log](/home/ubuntu/chipyard/software/firemarshal/logs/rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-build-2026-03-19--15-55-36-Y5OMV91059DLZSK5.log)
  - install:
    [rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-install-2026-03-19--15-56-24-UNUJCBBA4X2AHP5S.log](/home/ubuntu/chipyard/software/firemarshal/logs/rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-install-2026-03-19--15-56-24-UNUJCBBA4X2AHP5S.log)
- 新一轮 FireSim 也已重新拉起：
  - launchrunfarm:
    [2026-03-19--15-57-08-launchrunfarm-45KKIYBY5IVPEJ18.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-19--15-57-08-launchrunfarm-45KKIYBY5IVPEJ18.log)
  - run host:
    - instance `i-09f9a88800f991a35`
    - private ip `192.168.1.59`
- 当前正在进行：
  - infrasetup tmux session:
    `rerocc-prt-infrasetup-20260319g`
  - live log:
    [2026-03-19--15-58-23-infrasetup-K2F5KHCAAYTX2PBM.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-19--15-58-23-infrasetup-K2F5KHCAAYTX2PBM.log)
- 所以下一轮唯一目标保持不变：
  - 用最短 raw marker 区分 stall 究竟落在：
    - `submit_fence` 之后
    - `set_dst` 之后
    - 还是 `set_src` 之后

## 2026-03-19 16:45 UTC 最新更窄停点

- 上一节提到的 `post-fence / post-dst / post-src` 版本已经完成新一轮真实实验：
  - runworkload:
    [2026-03-19--16-06-42-runworkload-P6ND1HPK9IA5W0M6.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-19--16-06-42-runworkload-P6ND1HPK9IA5W0M6.log)
  - result dir:
    [2026-03-19--16-06-42-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime](/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-19--16-06-42-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime)
  - terminaterunfarm:
    [2026-03-19--16-42-09-terminaterunfarm-EGMWN4IFFP84XP2S.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-19--16-42-09-terminaterunfarm-EGMWN4IFFP84XP2S.log)
  - run host:
    - instance `i-09f9a88800f991a35`
    - private ip `192.168.1.59`
    - 已确认进入 `shutting-down`
- 这轮 Linux boot 和用户态前半段都正常推进：
  - 越过了 `S99run`
  - 重新进入 `[bertmini]`
  - 重新进入 `[prt-early]`
  - 重新进入 `segment=0 begin`
  - 到达 `worker stage=0 ready`
- 本轮新增最关键证据：
  - FPGA 上已经依次看到：
    - `[prt-raw] dma pre-acquire`
    - `[prt-raw] dma acquire-ok`
    - `[prt-raw] dma pre-program`
    - `[prt-raw] dma post-fence`
    - `[prt-raw] dma post-dst`
  - 但始终没有看到：
    - `[prt-raw] dma post-src`
- 这说明当前停点已再次收窄：
  - `hw_dma_submit_fence()` 已经返回
  - `hw_dma_set_dst(...)` 已经返回
  - 当前主停点更像是：
    - `hw_dma_set_src(...)` 指令本身
    - 或它触发的紧邻硬件交互
- 稳定性证据：
  - `uartlog` 连续两次检查都固定在：
    - `828`
  - 最后稳定 tail 为：
    - `[prt-raw] dma pre-program`
    - `[prt-raw] dma post-fence`
    - `[prt-raw] dma post-dst`
  - 同时 `heartbeat.csv` 继续从大约：
    - `1939s`
    - 增长到
    - `2038s`
- 因而当前最新排序应更新为：
  - 旧的 `stdout/fflush` 干扰嫌疑继续降级
  - `rr_acquire` 嫌疑继续降级
  - 当前主线集中到：
    - `set_src`
    - coupled-DMA `SRC_INFO` 接受路径
    - 或 ReRoCC manager/client 在 `set_src` 这一步的 ready 门控

## 2026-03-19 18:21 UTC 新一轮 no-post-mon 实验已重新启动并仍在进行

- 基于当前源码状态
  [prt_dma.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_dma.c)
  中“保留 `pre-mon` / monitor read / 去掉 `post-mon`”的最小改动，现已重新完成一轮真实环境重跑前两步：
  - launchrunfarm:
    [2026-03-19--17-46-54-launchrunfarm-ALOWOP3N81O133J8.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-19--17-46-54-launchrunfarm-ALOWOP3N81O133J8.log)
  - infrasetup:
    [2026-03-19--17-52-36-infrasetup-BF0WRZ3JCQGAAS0B.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-19--17-52-36-infrasetup-BF0WRZ3JCQGAAS0B.log)
    - tmux session:
      `rerocc-prt-infrasetup-20260319-nopostmon2`
    - manager wrapper exit code:
      `0`
- 当前 live run：
  - runworkload:
    [2026-03-19--18-10-53-runworkload-0BPLLUCO4SR97OVI.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-19--18-10-53-runworkload-0BPLLUCO4SR97OVI.log)
  - result dir:
    [2026-03-19--18-10-53-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime](/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-19--18-10-53-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime)
  - tmux session:
    `rerocc-prt-runworkload-20260319-nopostmon2`
  - run host:
    - instance `i-008177957ef9d1d40`
    - private ip `192.168.1.66`
- 当前 live 观测：
  - `runworkload` 仍在运行，manager 仍报告：
    - `1/1 simulations are still running`
  - run host 上
    `/home/ubuntu/sim_slot_0/uartlog`
    已从最初 OpenSBI/early-kernel 输出继续推进到更后面的 Linux init 阶段：
    - 最新检查行数约 `217`
    - 已看到：
      - `smp: Brought up 1 node, 2 CPUs`
      - `printk: console [ttySIF0] enabled`
      - `NET: Registered PF_INET6 protocol family`
      - `9pnet: Installing 9P2000 support`
    - 但还没有到：
      - `S99run`
      - `[bertmini]`
      - `[prt-early]`
      - `[prt-raw] dma ...`
  - `heartbeat.csv` 仍持续前进，最近已到大约：
    - `5111228308, 565`
- 当前结论先不要越界：
  - 这轮还处在“Linux 正在慢速启动”的阶段
  - 不能把当前还未到用户态的状态直接判成 stuck
  - 这轮最重要的新事实只是：
    - 新实验已真实重新启动
    - `infrasetup` 已成功完成
    - `runworkload` 已进入 guest Linux，并且仍在持续推进
- 下一步继续：
  - 盯住 run host 的
    - `/home/ubuntu/sim_slot_0/uartlog`
    - `/home/ubuntu/sim_slot_0/heartbeat.csv`
  - 等它越过
    - `S99run`
    - `[bertmini]`
    - `[prt-early]`
  - 再判断这轮 no-`post-mon` 版本是否终于越过：
    - `[prt-raw] dma post-src`
    - 或再次稳定停在 `set_src` 邻域

## 2026-03-19 18:40 UTC no-post-mon 实验已确认仍会真卡死并已回收

- 上一节记录的 live run 现已满足真 stuck 判据，并已先回收 runfarm：
  - runworkload:
    [2026-03-19--18-10-53-runworkload-0BPLLUCO4SR97OVI.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-19--18-10-53-runworkload-0BPLLUCO4SR97OVI.log)
  - terminaterunfarm:
    [2026-03-19--18-39-10-terminaterunfarm-8AEEGUDJH4MUAF02.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-19--18-39-10-terminaterunfarm-8AEEGUDJH4MUAF02.log)
  - terminaterunfarm tmux session:
    `rerocc-prt-terminaterunfarm-20260319-nopostmon2`
    - wrapper exit code:
      `0`
  - run host:
    - instance `i-008177957ef9d1d40`
    - private ip `192.168.1.66`
    - 已确认进入 `shutting-down`
- 这轮确实正常越过了 Linux boot 和用户态初始化前半段，重新到达第一条 DMA 提交附近：
  - 可见：
    - `[prt-progress] init ready ...`
    - `[prt-progress] segment=0 begin ...`
    - `[prt-progress] worker stage=0 ready ... dma=2 tiles=2`
    - `[prt-raw] dma pre-acquire`
    - `[prt-raw] dma acquire-ok`
    - `[prt-raw] dma pre-program`
    - `[prt-raw] dma post-fence`
    - `[prt-raw] dma post-dst`
    - `[prt-raw] dma pre-mon`
  - 不可见：
    - `[prt-raw] dma post-src`
- 稳定性证据：
  - 两次相隔约 90 秒的远端检查中，
    `/home/ubuntu/sim_slot_0/uartlog`
    行数都固定在：
    - `829`
  - 同时
    `/home/ubuntu/sim_slot_0/heartbeat.csv`
    从大约：
    - `14547403827, 1547`
    增长到：
    - `15727096512, 1665`
- 这一轮的解释必须写清：
  - 这不是“卡点提前到了一个更新的位置”的新证据
  - 因为当前 no-`post-mon` 版本本来就删除了
    `pre-mon` 之后、`set_src` 之前的那条 `post-mon` raw marker
  - 所以现在 UART 停在：
    - `[prt-raw] dma pre-mon`
    只能说明停点仍在这个极窄窗口里：
    - `hw_dma_read_monitor(DMA_MON_VALID)`
    - `hw_dma_set_src(...)`
    - 或它们触发的紧邻硬件交互
- 但这轮依然给出一个重要结论：
  - 去掉 `post-mon` 那条 `write(STDERR_FILENO, ...)` 之后，系统仍然会真卡死
  - 因而：
    - `post-mon` 那条 syscall 不是“唯一根因”
    - “删掉最后一条 raw log 就能越过 `set_src`” 这一更乐观的分支当前未被支持
- 与上一轮 `post-mon` 版本合并解释后的当前最强结论：
  - 上一轮已经证明：
    - monitor read 成功返回过
    - 当时还能看到 `[prt-raw] dma post-mon`
    - 但仍看不到 `[prt-raw] dma post-src`
  - 这一轮又证明：
    - 即使删掉 `post-mon` 这条 syscall，本质性真卡死仍然存在
  - 所以当前主怀疑应继续集中到：
    - `hw_dma_set_src(...)`
    - coupled-DMA `SRC_INFO` 接受路径
    - 或 ReRoCC client/manager 在 `set_src` 这一步的 ready 门控

## 2026-03-19 18:50 UTC 最新静态收敛：主线应从“post-mon 日志”转向“monitor-read + set_src”组合

- 这轮之后，最需要纠正的方向是：
  - 不要再把问题表述成“`post-mon` 那条 raw `write()` 导致卡死”
  - 当前更像是：
    - `hw_dma_read_monitor(DMA_MON_VALID)`
    - 紧跟着的 `hw_dma_set_src(...)`
    这一组合路径本身有问题
- 静态原因 1：
  [GemminiCoupledDMA.scala](/home/ubuntu/chipyard/generators/gemmini/src/main/scala/gemmini/GemminiCoupledDMA.scala)
  中 `monitor read` 与 `set_src` 在硬件语义上并不等价：
  - `monitor read`
    - `readyMonitor = isMonitor && !respValid`
    - `acceptMonitor` 会执行：
      - `respValid := true`
      - `respData := monitorData`
    - 对应 [GemminiCoupledDMA.scala#L400-L448](/home/ubuntu/chipyard/generators/gemmini/src/main/scala/gemmini/GemminiCoupledDMA.scala#L400)
  - `set_src`
    - `readySrc = isSrc && copyReqQ.io.enq.ready`
    - 真正把 copy request 组包入队发生在
      [GemminiCoupledDMA.scala#L429-L434](/home/ubuntu/chipyard/generators/gemmini/src/main/scala/gemmini/GemminiCoupledDMA.scala#L429)
- 静态原因 2：
  `monitor read` 不是普通“无返回值配置命令”，它会走 ReRoCC 的
  response/writeback 路径：
  - manager 侧把 `io.resp` 包成 `sWrite` 响应：
    [Manager.scala#L162-L171](/home/ubuntu/chipyard/generators/rerocc/src/main/scala/manager/Manager.scala#L162)
  - client 侧再把 `sWrite` 变回 RoCC response：
    [Client.scala#L241-L248](/home/ubuntu/chipyard/generators/rerocc/src/main/scala/client/Client.scala#L241)
  - 因而当前 `pipeline-runtime` 在关键区里实际执行的不是
    “`set_dst -> set_src`”，而是：
    - `set_dst`
    - 一个会产生 writeback 的 `monitor read`
    - `set_src`
- 静态原因 3：
  已通过的 Linux coupleddma 回归测试并不这么写。
  目前优先参考的三类 Linux regression 基本都保持最干净的：
  - `fence -> set_dst -> set_src -> wait`
  - 中间不插 `read_monitor`
  - 也不插日志 syscall
  - 代表位置：
    - [rerocc_dma_matrix_linux_coupleddma.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests-coupleddma/rerocc_dma_matrix_linux_coupleddma.c)
    - [rerocc_lc_gemmini_matrix_linux_coupleddma.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests-coupleddma/rerocc_lc_gemmini_matrix_linux_coupleddma.c)
    - [rerocc_lc_coverage_linux_coupleddma.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests-coupleddma/rerocc_lc_coverage_linux_coupleddma.c)
- 当前偏弱、可暂时降级的解释：
  - “CoupledDMA queue 太浅，第一条 `set_src` 就自然打满”
  - 因为静态默认值看起来并不小：
    - `copy_queue_depth = 8`
    - [GemminiCoupledDMA.scala#L14](/home/ubuntu/chipyard/generators/gemmini/src/main/scala/gemmini/GemminiCoupledDMA.scala#L14)
    - `ReRoCCIBufEntriesKey = 4`
    - [Parameters.scala#L42](/home/ubuntu/chipyard/generators/rerocc/src/main/scala/manager/Parameters.scala#L42)
- 所以下一步最优先的最小实验，不再是围绕 `post-mon` 日志做文章，而是：
  - 直接把
    `hw_dma_read_monitor(DMA_MON_VALID)`
    也从 `set_dst -> set_src` 临界区移走或临时去掉
  - 让 `pipeline-runtime` 的关键调用序列尽量与 Linux coupleddma 正例一致：
    - `rr_acquire -> rr_set_opc -> fence -> set_dst -> set_src`
  - 再在真实环境中重跑一轮：
    - `marshal build`
    - `marshal install`
    - `launchrunfarm -> infrasetup -> runworkload`

## 2026-03-20 当前本地实现状态

- 当前源码已经继续向前推进，但还没有重新进入 FireMarshal image；必须明确区分“源码状态”和“已验证 image 状态”：
  - 当前源码
    [prt_dma.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_dma.c)
    已经把 `hw_dma_read_monitor(DMA_MON_VALID)` 从 `set_dst -> set_src` 临界区里完全移除
  - 但当前已构建的
    `rerocc_pipeline_runtime-linux`
    与 overlay binary 仍是 2026-03-19 17:44 UTC 左右的产物
  - 这些已构建 binary 里还能直接搜到：
    - `[prt-raw] dma pre-mon`
  - 所以截至目前，最后一份真正被 FireMarshal/FireSim 跑过的有效基线，
    仍然是 2026-03-19 18:10 UTC 那轮 no-`post-mon` run
- 针对当前最强软件嫌疑
  - `src_mod64 != dst_mod64`
  - 导致整段 DMA 长时间停留在 1B copy 模式
  现已在本地 runtime 中加了一个最小 workaround：
  - Linux `dram <-> spm` 路径新增 per-stage host-page bounce buffer
  - 当单个 DMA chunk 满足：
    - `bytes >= 64`
    - 且 `src_mod64 != dst_mod64`
    时，不再直接提交原始 host pointer
  - 而是先把 host 数据搬到一个 host-page 对齐的临时页中，
    并把临时页内偏移摆到与目标地址相同的 `mod64`
  - 目的是把当前最可疑的首笔请求形态：
    - `src offset = 0x10`
    - `dst offset = 0x00`
    - `bytes = 1024`
    从“整段永久 byte-mode”改写成可重新进入 wide-copy 的形态
- 为了让这个 workaround 有独立于 bertmini 的回归证据，现有 Linux coupleddma coverage regression
  也已补了一个新的精确复刻 case：
  - `dma_dram_to_shared_misaligned_fullpage`
  - 形态固定为：
    - DRAM source offset `0x10`
    - shared-SPM destination offset `0x00`
    - `1024B`
- 这些 2026-03-20 的本地源码改动目前还没有重新经过：
  - `rerocc_pipeline_runtime-linux` rebuild
  - `marshal build`
  - `marshal install`
  - FireSim F2 replay
- 因此下一跳必须固定为：
  1. 重新构建 Linux binary，并确认新 binary：
     - 仍包含 `[prt-raw] dma post-src`
     - 不再包含 `[prt-raw] dma pre-mon`
  2. 先跑 small Linux coupleddma regression image 做快速 gate
  3. 再把同一份源码重新打进 bertmini dedicated image，做新的 F2 重跑

## 2026-03-20 03:05 UTC 本轮实现与最新阻塞

- 本轮首先已把本地源码与 Linux binary 的 skew 收掉：
  - host `pipeline-runtime` 重新编译通过
  - `rerocc_pipeline_runtime-linux` 重新交叉编译通过
  - 新 binary 检查结果：
    - 仍包含 `[prt-raw] dma post-src`
    - 已不再包含 `[prt-raw] dma pre-mon`
  - 新增 coverage case binary 也已重新生成，并确认包含：
    - `dma_dram_to_shared_misaligned_fullpage`
- 对应本轮真实环境镜像也已重新 build/install：
  - small regression image:
    - build:
      [rerocc-lc-linux-coupleddma-regression-small-pipelinefiles-build-2026-03-20--02-30-01-E095QTWN9EB8ENBQ.log](/home/ubuntu/chipyard/software/firemarshal/logs/rerocc-lc-linux-coupleddma-regression-small-pipelinefiles-build-2026-03-20--02-30-01-E095QTWN9EB8ENBQ.log)
    - install:
      [rerocc-lc-linux-coupleddma-regression-small-pipelinefiles-install-2026-03-20--02-31-14-KIYWOOOTTC9A4CPS.log](/home/ubuntu/chipyard/software/firemarshal/logs/rerocc-lc-linux-coupleddma-regression-small-pipelinefiles-install-2026-03-20--02-31-14-KIYWOOOTTC9A4CPS.log)
  - bertmini dedicated image:
    - build:
      [rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-build-2026-03-20--02-41-58-S0XNWQHZJA1NWBGR.log](/home/ubuntu/chipyard/software/firemarshal/logs/rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-build-2026-03-20--02-41-58-S0XNWQHZJA1NWBGR.log)
    - install:
      [rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-install-2026-03-20--02-57-22-TDWAI9N5HQDTPU6D.log](/home/ubuntu/chipyard/software/firemarshal/logs/rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-install-2026-03-20--02-57-22-TDWAI9N5HQDTPU6D.log)
- small regression 第一次 FPGA gate 已经完整跑过一轮，但结论不是 DMA pass/fail，而是 image packaging bug：
  - launchrunfarm:
    [2026-03-20--02-31-56-launchrunfarm-X3UB9V1G86R0DSX8.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-20--02-31-56-launchrunfarm-X3UB9V1G86R0DSX8.log)
  - infrasetup:
    [2026-03-20--02-32-20-infrasetup-UPK8QF920C97JJAM.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-20--02-32-20-infrasetup-UPK8QF920C97JJAM.log)
  - runworkload:
    [2026-03-20--02-35-32-runworkload-2QNPQVPJ2JLDYBCY.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-20--02-35-32-runworkload-2QNPQVPJ2JLDYBCY.log)
  - result dir:
    [2026-03-20--02-35-32-rerocc-lc-linux-coupleddma-regression-small-pipelinefiles-f2-rerocc-linux-regression-small-pipelinefiles](/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-20--02-35-32-rerocc-lc-linux-coupleddma-regression-small-pipelinefiles-f2-rerocc-linux-regression-small-pipelinefiles)
  - terminaterunfarm:
    [2026-03-20--02-59-59-terminaterunfarm-X6E60VXYIM32122E.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-20--02-59-59-terminaterunfarm-X6E60VXYIM32122E.log)
  - guest 已进入：
    - `running /etc/init.d/S99run`
    - `launching firemarshal workload run/command`
    - `[rerocc-lc-linux-regression] ...`
  - 但随后直接报：
    - `/root/rerocc-linux-tests-coupleddma/rerocc_lc_matrix_linux_coupleddma_verify-linux: No such file or directory`
    - `/root/rerocc-linux-tests-coupleddma/rerocc_lc_coverage_linux_coupleddma-linux: No such file or directory`
    - `/root/rerocc-linux-tests-coupleddma/rerocc_lc_nonblocking_linux_coupleddma-linux: No such file or directory`
    - `ALL_TESTS_FAIL matrix_ret=127 coverage_ret=127 nonblocking_ret=127`
- 这轮失败把真正的软件问题缩到 workload staging：
  - `host-init.sh` 里 `build_linux_binaries()` 本来已经把 coupleddma Linux binaries 编到
    `overlay/root/rerocc-linux-tests-coupleddma/`
  - 但 `stage_coupleddma_overlay()` 又对同一个目录做了 `rm -rf`
  - 结果 `rerocc_lc_matrix_linux_coupleddma_verify-linux`、
    `rerocc_lc_coverage_linux_coupleddma-linux`、
    `rerocc_lc_nonblocking_linux_coupleddma-linux`
    在 image 打包前被自己删掉
- 该 staging bug 现已在本地修复并验证：
  - 文件：
    [host-init.sh](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests-coupleddma/workload/host-init.sh)
  - 修复方式：
    - 不再在 `stage_coupleddma_overlay()` 中删除整个 overlay 目录
    - 保留 `build_linux_binaries()` 刚生成的 coupleddma executables
    - 仅额外补充 generic binaries 与 wrapper script
  - 修复后本地 overlay 已确认包含：
    - `rerocc_lc_matrix_linux_coupleddma_verify-linux`
    - `rerocc_lc_coverage_linux_coupleddma-linux`
    - `rerocc_lc_nonblocking_linux_coupleddma-linux`
- 修复后的 small regression image 也已重新 build/install：
  - build:
    [rerocc-lc-linux-coupleddma-regression-small-pipelinefiles-build-2026-03-20--03-01-54-85CHWK0F0AHOYDW7.log](/home/ubuntu/chipyard/software/firemarshal/logs/rerocc-lc-linux-coupleddma-regression-small-pipelinefiles-build-2026-03-20--03-01-54-85CHWK0F0AHOYDW7.log)
  - install:
    [rerocc-lc-linux-coupleddma-regression-small-pipelinefiles-install-2026-03-20--03-02-41-ZLKA3NPJSAOTUCJG.log](/home/ubuntu/chipyard/software/firemarshal/logs/rerocc-lc-linux-coupleddma-regression-small-pipelinefiles-install-2026-03-20--03-02-41-ZLKA3NPJSAOTUCJG.log)
- 但第二轮 corrected small regression 还没进入 `infrasetup/runworkload`，当前新 blocker 已变成 AWS run farm launch：
  - 当前 tmux session:
    `regression-pipelinefiles-launch-r2`
  - 当前 log:
    [2026-03-20--03-02-52-launchrunfarm-U5Y12CNH5R4OMD91.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-20--03-02-52-launchrunfarm-U5Y12CNH5R4OMD91.log)
  - 目前没有任何新 F2 run instance 成功启动
  - `aws ec2 describe-instances` 当前只看到 manager 自身：
    - `i-08b9950158e875a41`
    - `c5.2xlarge`
    - `firesim-manager-wzy`
  - `launchrunfarm` 失败信息已从“纯 capacity”收窄到：
    - `VcpuLimitExceeded`
    - 并伴随 `f2.6xlarge` 在 `us-west-2a` / `us-west-2d` 不受支持
  - 因此截至 2026-03-20 03:05 UTC：
    - 源码修复完成
    - runtime / workload image rebuild 完成
    - small regression 的第一个真实 FPGA gate 已揭示并修掉 overlay bug
    - 下一步 blocker 不再是 DMA 逻辑本身，而是 F2 run-farm launch 配额/容量
