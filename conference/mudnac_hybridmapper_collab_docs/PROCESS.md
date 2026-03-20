# Pipeline Runtime Integration Process

日期：2026-03-20

## 固定流程

- 不手工编辑 `sims/firesim/deploy/workloads/*`
- workload 变体必须先改 FireMarshal 源配置，再 `marshal install`
- FireSim 只按这个顺序执行：
  - `marshal build`
  - `marshal install`
  - `launchrunfarm`
  - `infrasetup`
  - `runworkload`
  - `terminaterunfarm`
- workload、rootfs、binary、AGFI 任意一项变化后，都必须重新 `infrasetup`
- 长时间 FireSim manager 任务用：
  - `scripts/firesim-tmux-run.sh`
- FireSim 非默认 runtime 必须显式传：
  - [config_runtime_f2_rerocc_lc_linux_bertmini_pipeline_runtime.yaml](/home/ubuntu/chipyard/sims/firesim/deploy/config_runtime_f2_rerocc_lc_linux_bertmini_pipeline_runtime.yaml)
- 不要假设当前有可复用 run farm；重跑前先查询，失败 run 要及时回收
- run farm 机时昂贵，固定执行规则是：
  - 任一时刻最多只保留当前调试必需的一台 active F2 run farm
  - 在启动新一轮 `launchrunfarm` 前，先确认旧实例已经进入 `shutting-down` / `terminated`
  - 一旦本轮 run 明确失败、明确通过，或确认短期内不再复用，就立刻执行 `terminaterunfarm`
  - 不以 manager 命令返回码为准，必须再用 `aws ec2 describe-instances` 复核实例状态

## 进入正确环境

- FireMarshal 相关命令前：
  - `source /home/ubuntu/chipyard/env.sh`
- FireSim manager 相关命令前：
  - `cd /home/ubuntu/chipyard/sims/firesim`
  - `source sourceme-manager.sh`

## 专用入口

- FireMarshal workload source:
  [rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime.json](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests-coupleddma/workload/rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime.json)
- Guest wrapper:
  [run_rerocc_pipeline_runtime_bertmini.sh](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests-coupleddma/workload/overlay/root/rerocc-linux-tests/run_rerocc_pipeline_runtime_bertmini.sh)
- FireMarshal host-init:
  [host-init.sh](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests-coupleddma/workload/host-init.sh)

## 当前调试规则

- dedicated `bertmini` workload 现在默认强制：
  - `PIPELINE_RUNTIME_PROGRESS=1`
- `host-init` 会校验最终 RISC-V binary 里必须含有：
  - `[prt-early] enter main`
- 若修改了 `pipeline-runtime/src/*.c`，下一轮 FPGA 验证前必须完整重走：
  - `marshal build`
  - `marshal install`
  - `infrasetup`
  - `runworkload`

## 2026-03-20 新增通用经验：先区分 host-backed 还是纯 SPM DMA

- 这次 bertmini 第一笔 DMA hang 的直接教训不是“只要做了 `virt_to_phys` 就够了”。
- 对 Linux guest 下的 coupled-DMA，`virt_to_phys` 与按 host page 切块只是必要条件，
  不是充分条件；还必须检查每个 DMA chunk 的：
  - `bytes`
  - `src_mod64`
  - `dst_mod64`
- 经验规则固定为：
  - 只要 `bytes >= 64` 且 `src_mod64 != dst_mod64`，就把它当成需要额外证明的风险形态
  - 对 host<->SPM 路径，不要再直接 `prt_dma_submit()`，必须统一走
    [prt_dma.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_dma.c)
    里的 `prt_dma_copy_dram_to_spm_pages()` /
    `prt_dma_copy_spm_pages_to_dram()`
  - 如有必要，使用 bounce buffer 把 host 侧端点重排到与 SPM 端点相同的 `mod64`
- 当前复查后的固定分类是：
  - 活跃的 host-backed DMA 路径已经全部收敛到上述 helper，当前没有第二条绕过保护的活跃入口
  - scheduler 中允许 overlap 直提的路径只剩 contiguous page-list 的 `SPM -> SPM`
  - `prt_dma_copy_spm_va()` 虽然还能表达任意 offset 的 `SPM -> SPM` copy，但当前没有 call site，
    只能当 latent risk 记账，不能当活跃 bug
- 以后检查 pipeline-runtime 新 DMA 路径时，先按这个顺序做静态审核：
  1. 这条路径是不是 host-backed，也就是地址最终来自 `malloc/fread/model_blob/input_blob/DRAM alias`
  2. 它是否最终落到 `prt_dma_copy_dram_to_spm_pages()` /
     `prt_dma_copy_spm_pages_to_dram()`
  3. 如果没有，是否可能出现 `bytes >= 64 && src_mod64 != dst_mod64`
  4. 如果可能，就先补 workaround 或限制条件，再谈 FPGA 验证
- 回归测试的通用经验也固定下来：
  - 不要只测页对齐 `mmap` buffer
  - 必须保留一个故意让 `src/dst` 页内 offset 不相等的 full-page case
  - 这次对应的最小守门用例是
    [rerocc_lc_coverage_linux_coupleddma.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests-coupleddma/rerocc_lc_coverage_linux_coupleddma.c)
    中的 `dma_dram_to_shared_misaligned_fullpage`
- 调试判读规则也补一条：
  - 一旦 FPGA 已经明确出现重复的 `dma post-src` 与 `dma-wait fence-done`
  - 就不要继续把同一轮现场解释成“还是第一笔 DMA 没修好”
  - blocker 应该前移到 DMA 之后的执行阶段

## 2026-03-20 新增通用经验：高频原始日志必须与结构化进度日志解耦

- 这次 dedicated bertmini F2 重跑已经证明：
  - 把每笔 DMA 都打印成 `[prt-raw] ...` 会严重放大 UART 体积和 run farm 机时
  - 即使功能路径是对的，也可能让一次本可继续定位的 run 变成数十分钟到数小时的低效消耗
- 因此当前规则固定为：
  - `PRT_ENABLE_PROGRESS_LOG=1` 用来保留 `segment=...` / `worker ...` / `dma-wait ...` 这类结构化边界日志
  - 原始逐笔硬件日志必须放到单独开关 `PRT_ENABLE_PROGRESS_RAW_LOG`
  - 默认调试配置应保持：
    - 结构化日志开
    - raw 日志关
  - 只有在已经把停点收敛到很窄的 critical path，且结构化日志仍不足时，才临时打开 raw 日志
- 新的判读规则也固定下来：
  - 如果 `heartbeat.csv` 持续增长，但 `uartlog` 的 mtime 和行数长时间不再变化
  - 就把停点定在“最后一条结构化日志之后”的那段代码里
  - 不要再把这种现象优先解释成：
    - FireSim manager 没回传日志
    - run host SSH 问题
    - 单纯 boot 慢
- 这次关闭 raw 日志后的实际案例是：
  - `segment=0` 已完整通过
  - `segment=1` 打到
    `worker stage=0 subbatch=1 begin`
    后 `uartlog` 静默
  - 同时 `heartbeat.csv` 继续推进
  - 因而 blocker 应被定性为：
    - `segment=1` split-OC compute 路径里、`begin -> compute-done` 之间的真实 hang
  - 后续 instrumentation 应直接补在：
    - manager acquire
    - `tiled_conv_auto`
    - fence
    - repack
    这些边界，而不是重新打开整条 raw DMA flood

## FireMarshal / libguestfs 注意事项

- `marshal build` / `marshal install` 最后会走 `guestmount` / `libguestfs` / `supermin`
- 2026-03-19 已确认：
  - 在当前 Codex 沙箱里跑这类命令，容易出现与真实环境无关的假故障
  - 直接在非沙箱真实环境里、并先执行
    `cd /home/ubuntu/chipyard/sims/firesim && source sourceme-manager.sh`
    后再跑完整 `marshal build` / `marshal install`，本 workload 已成功通过
- 因此后续规则固定为：
  - FireMarshal `marshal build/install`
  - FireSim manager `launchrunfarm/infrasetup/runworkload/terminaterunfarm`
  这几类命令优先直接在非沙箱真实环境执行
- 不要再把沙箱里的 `libguestfs` / socket / `/var/tmp` / `/boot/vmlinuz-*` 报错当成 workload 本身故障
- 如果日志里出现：
  - `libguestfs: error: /usr/bin/supermin exited with error status 1`
  - `supermin: exception: Sys_error("/var/tmp/supermin...tmpdir: Permission denied")`
- 优先判定为当前执行环境不允许 `libguestfs` 往 `/var/tmp/.guestfs-*`、`/var/tmp/supermin*.tmpdir` 写临时文件
- 这类报错先不要误判成：
  - workload 二进制没编进去
  - image overlay 内容损坏
  - guest 侧 `pipeline-runtime` 逻辑回归
- 遇到这类报错时，先用最小命令验证：
  - `LIBGUESTFS_DEBUG=1 LIBGUESTFS_TRACE=1 libguestfs-test-tool`
- 只有在 `libguestfs-test-tool` 正常后，才继续 `marshal build` / `marshal install`

## 日志判定顺序

### 1. 先判定是不是用对了镜像

- 看 FireMarshal build log 里 runtime 编译参数是否真的是 `PIPELINE_RUNTIME_PROGRESS=1`
- 如果 guest 里连 `[prt-early] enter main` 都没有，优先怀疑：
  - build/install 没更新进 image
  - 跑错 workload
  - `runworkload` 使用了错误的 runtime config

### 2. 再看 early startup

下一轮 FPGA 重跑时，先只盯这些日志：

- `[prt-early] enter main`
- `[prt-early] live stdio ready`
- `[prt-early] defaults ready`
- `[prt-early] arg parse done`
- `[prt-early] calling runtime_init`

判定方法：

- 若只出现第一条或第二条就崩：
  - 优先查 `main()` 极早期 / `setvbuf()` / libc startup
- 若能到 `calling runtime_init` 才崩：
  - 再查 `prt_runtime_init()`
- 若已经进入 `[prt-progress]`：
  - 再按原来的 init / artifacts / worker 日志继续往后看

补充：如果 live `uartlog` 连 `S99run` / `[bertmini]` 都还没出现，就不要继续按 DMA submit 路线解释。
先把停点记成：

- Linux boot 早期阶段
- 以 live `uartlog` 最后一条内核打印为准
- 同时记录 `heartbeat.csv` 是否仍在增长

2026-03-19 当前最新一轮 run 的最新 live 停点就是：

- `[    0.000000] software IO TLB: mapped [mem 0x00000000fbfff000-0x00000000fffff000] (64MB)`
- 且 `heartbeat.csv` 仍持续增长

补充修正：

- 2026-03-19 这轮 fresh rerun 后来已经证明：
  - Linux boot 的确很慢
  - 不能因为早期几十到几百秒还没到 `S99run` 就直接判定成新 blocker
- 2026-03-19 10:47 UTC 这轮 active rerun 又进一步证明：
  - live `uartlog` 可以在
    `S10mdev -> S40network -> S99run`
    之间出现很长静默
  - 在这段静默期间，`heartbeat.csv` 仍会持续增长
  - 因此如果最后一条 live 行只是：
    `Starting mdev: OK`
    且没有明确报错，不要急着判 boot 死锁，也不要急着终止 run farm
- 所以如果没有明显报错，优先耐心等到：
  - `S99run`
  - `[bertmini]`
  - `[prt-early]`
  - `[prt-progress]`
  真正出现后再决定是否回到 DMA 路线
- 但 2026-03-20 又新增了一条更具体的 early-userspace 判读规则：
  - 如果已经明确看到
    `S01syslogd -> S02klogd -> S02sysctl -> S10mdev -> Starting mdev: OK`
  - 且之后连续多轮都没有
    `S40network`
    `S99run`
    `[bertmini]`
  - 同时 `heartbeat.csv` 继续增长、`uartlog` mtime 不再更新
  - 就不要继续把它记成“只是 boot 慢”
  - 当前应优先怀疑：
    - Buildroot 默认 `S10mdev` 在 daemon 启动后继续执行的
      `/sys` modalias coldplug + `modprobe -abq` 扫描
  - 当前固定 workaround 是：
    - 在 workload overlay 中覆盖
      [S10mdev](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests-coupleddma/workload/overlay/root/etc/init.d/S10mdev)
    - 保留 `mdev -df`
    - 跳过 coldplug/modprobe 扫描
  - 所以以后若再次看到“停在 `Starting mdev: OK`”的形态，排查顺序应改成：
    1. 先确认本轮镜像是否包含 overlay 版 `S10mdev`
    2. 若没有，先重做 `marshal build/install`
    3. 若有，再继续看 `S40network` 是否出现

### 3. 若进入 `[prt-progress]`

按这个顺序看：

- `init begin`
- `init page-table begin/end`
- `init dma-backend begin/end`
- `init gemmini-backend begin/end`
- `init spm-xlate begin/end`
- `runtime begin`
- `artifacts mapping load/parse`
- `artifacts validate stage-hit/stage-scan`
- `init load-model-bin begin/end`
- `init load-input-blob begin/end`
- `segment=0 begin`
- `worker stage=0 ready`
- 第一条 `dma-submit`

如果走到 worker，再看：

- `worker stage=... waiting ...`
- `dma-submit ...`
- `rr-acquire ...`

### 4. 当前已知的新停点

- 2026-03-19 最新 FPGA live run 已确认越过 early startup，并进入首个 DMA submit
- 当前最后一条稳定可见日志是：
  - `dma-submit token=1 before-set-src ...`
- 而下一条预期日志本应是：
  - `dma-submit token=1 set-src done ...`
- 如果再次复现这个模式：
  - 优先查 [prt_dma.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_dma.c) 中 `hw_dma_set_src(...)` 附近
  - 同时结合 live `heartbeat.csv` 判断是 guest 卡住还是 simulation 停止
- 2026-03-19 11:14 UTC 的更新进一步说明：
  - 在删掉 `submit-fence begin/done` 之后，最新 active rerun 的稳定停点又前移到了
    半截 `before-set-dst ... initial_wide_hi`
  - 且当时 `uartlog` 行数固定、`heartbeat.csv` 仍继续增长
- 所以当前流程上要补一条更具体的判定：
  - 如果 live `uartlog` 稳定截断在 `before-set-dst` 这条 progress log 本身
  - 优先怀疑：
    - `before-set-dst` 的超宽 `PRT_PROGRESS_LOG(...)`
    - 紧邻的 `dma_debug_current_cpu()` / `getcpu`
    仍在扰动 critical path
  - 这时先做“继续瘦 critical-path 日志”的最小实验，再去扩大对硬件 ready/握手的推断
- 2026-03-19 12:10 UTC 的更新又把这个判断再往前推了一步：
  - 在删掉 `before-set-dst` 和 `getcpu` 之后，最新 fresh rerun 的最后一条完整日志变成了
    `rr-acquire armed ...`
  - 紧接着 live UART 只剩半截：
    `[prt-progress] dma`
  - 按当前代码顺序推断，这一半截最可能对应下一条：
    `dma-submit token=... acquired ...`
  - 所以如果再次复现这种模式，下一针应先继续去掉
    `acquired` 这条 progress log，
    而不是先扩大对 `set_dst/set_src` 硬件行为的解释范围

## 当前静态判断优先级

### 1. 先按 blocking-fence 路径理解当前运行

- 不要再默认认为当前 FPGA run 走的是 progress-thread DMA
- 只要 `spm_xlate_enable` 打开，runtime init 会把：
  - `dma_backend` 强制成 `blocking_fence`
  - `gemmini_mode` 强制成 `blocking_fence`
- 所以首查路径应该是：
  - `dma_blocking_submit()`
  - `hw_dma_set_dst() / hw_dma_set_src()`
  - `hw_dma_fence()`

### 2. shared scratchpad local path 上的 1B TL 访问不是首要怀疑点

- `TLRAM` 自身支持 `TransferSizes(1, beatBytes)`
- `TLFragmenter(min=beatBytes, ...)` 只是不支持“再向下碎成 sub-beat”
- 对于本来就是 1B 的请求，静态上可以原样通过
- 因此当前不应把主要时间花在“证明 TL 协议层面非法”上

### 3. 当前首个 DMA 请求更像是把硬件推进了未充分验证的 1B copy 路径

- pipeline-runtime 的 Linux `dram->spm` chunking 只按 host page 边界切块
- model/input blob 有 `malloc` 路径，不保证 64B 对齐
- live 首个请求已出现：
  - `src offset = 0x10`
  - `dst offset = 0x00`
  - `bytes = 1024`
- coupled-DMA 在这种情况下会静态退化为整段 1B `Get/Put`
- 这条线比“TL 不合法”更值得优先验证

### 4. known-good coupled-DMA 自测覆盖不到当前这个形态

- 现有 Linux DMA 自测主要使用页对齐 `mmap` buffer
- 并且保留 `src/dst` 相同页内 offset
- pipeline-runtime 当前 workload 则更容易落到：
  - `malloc` source
  - `src/dst` offset 不相等
  - `1024B` page copy 全程 byte mode

### 5. 软件侧额外注意点

- 当前 submit 前缺少 known-good test 里的 `fence rw, rw`
- 这是合理嫌疑，但优先级低于：
  - misaligned host->SPM 请求
  - coupled-DMA 1B copy/fence 路径

### 6. 新增静态结论：先别假设是线程迁核把 ReRoCC CSR 映射冲掉了

- [prt_runtime.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_runtime.c) 的 `stage_bind_current_thread()` 已经会把 stage worker 绑到固定 CPU
- 这说明：
  - `rr_set_opc()` 之后立刻因为 stage worker 迁核而换了一套 `RROPC*` CSR，这条怀疑优先级下降
  - 但仍不能排除：
    - 同一 hart 上 `custom2 -> cfg` 映射被别的路径改写
    - `cfg` acquire bit / manager 字段在 `set_dst -> set_src` 之间发生了意外变化
- 因此下一轮优先加的是：
  - `RROPC2` 当前映射值
  - 对应 `RRCFG<cfg>` 当前 raw 值、acquire 位、manager 位
  - 并在 `before-set-dst` / `before-set-src` 两个时刻都打印

### 7. 新增静态结论：优先对照已通过 FPGA 的 Linux coupleddma 自测

- 约束：后续只要遇到 Gemmini / DMA 调用接口问题，优先参考 Linux 下三个回归测试的写法，不要先从新抽象或 baremetal 版本推断
  - [rerocc_dma_matrix_linux_coupleddma.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests-coupleddma/rerocc_dma_matrix_linux_coupleddma.c)
  - [rerocc_lc_gemmini_matrix_linux_coupleddma.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests-coupleddma/rerocc_lc_gemmini_matrix_linux_coupleddma.c)
  - [rerocc_lc_coverage_linux_coupleddma.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests-coupleddma/rerocc_lc_coverage_linux_coupleddma.c)
- 直接可参考的 known-good Linux/coupleddma 用例包括：
  - [rerocc_dma_matrix_linux_coupleddma.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests-coupleddma/rerocc_dma_matrix_linux_coupleddma.c)
  - [rerocc_lc_gemmini_matrix_linux_coupleddma.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests-coupleddma/rerocc_lc_gemmini_matrix_linux_coupleddma.c)
  - [rerocc_lc_coverage_linux_coupleddma.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests-coupleddma/rerocc_lc_coverage_linux_coupleddma.c)
  - [rerocc_lc_nonblocking_linux_coupleddma.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests-coupleddma/rerocc_lc_nonblocking_linux_coupleddma.c)
- 它们在 DMA 提交临界区里的共同模式非常稳定：
  - `rr_acquire_cfg_with_retry`
  - `rr_set_opc(2, cfg_id)`
  - 立刻 `set_dst`
  - 立刻 `set_src`
  - 只在 userspace 自旋等待 done flag
  - 然后 `rr_fence` / `rr_release`
- 这些通过用例在 `rr_set_opc -> set_dst/set_src` 之间几乎不会做：
  - `printf/fflush`
  - `getcpu()`
  - 其他会把线程带进 kernel 的 syscall 型调试
- 当前 pipeline-runtime 的关键差分是：
  - [prt_dma.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_dma.c) 在 `before-set-dst` / `before-set-src` 日志里会执行 `PRT_PROGRESS_LOG(...)`
  - [prt_progress.h](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/include/prt_progress.h) 的实现包含 `fprintf(stdout, ...)` 与 `fflush(stdout)`
  - 同时 `dma_debug_current_cpu()` 通过 `syscall(SYS_getcpu, ...)` 读当前核号
  - known-good Linux 用例普遍还会 `mlockall(MCL_CURRENT | MCL_FUTURE)`，并对 DMA buffer 做 `mlock`
  - pipeline-runtime 当前 `model_bin/input_blob` 则是 `malloc + fread` 路线，没有同等级的锁页保护
- 结合硬件静态阅读：
  - [GemminiCoupledDMA.scala](/home/ubuntu/chipyard/generators/gemmini/src/main/scala/gemmini/GemminiCoupledDMA.scala) 中 `readySrc` 只依赖 `copyReqQ.io.enq.ready`
  - 它并不直接看 DMA busy、PTW、或目的地址配置状态
- 所以如果现场仍停在 `before-set-src`，下一轮最小验证应优先做：
  - 让 `rr_set_opc -> set_dst -> set_src` 更贴近 known-good Linux 自测
  - 先避免临界区内的 syscall 型 debug
  - 同时注意 `virt_to_phys -> set_src` 之间目前比 known-good 用例多了一段没有锁页保护的窗口
  - 再观察 freshest stop point 是否还留在 `before-set-src`
- 2026-03-19 11:14 UTC 的最新现场已经把这个判断再往前推了一步：
  - freshest stop point 现在甚至可能留在 `before-set-dst` 这条日志本身
  - 因而下一针更应该先把：
    - `before-set-dst` 长日志
    - `getcpu`
    从 critical path 上移开或极限瘦身
- 2026-03-19 12:10 UTC 的最新现场进一步说明：
  - 去掉 `before-set-dst` / `getcpu` 之后，停点又前移到了
    `acquired` 这条 log 本身附近
  - 所以“继续收缩 `rr-acquire` 之后的 userspace progress log”
    仍是当前优先级最高的最小实验

### 8. 新增 live 证据：重日志版本现在会在 `set-dst done` 之后卡在半行 progress 输出

- 最新 live rerun 已经重新确认：
  - `rr-acquire armed`
  - `before-set-dst`
  - `submit-fence begin/done`
  - `set-dst done`
- 但 UART 末尾随后稳定停在：
  - `...[prt-prog`
- 而不是完整的：
  - `before-set-src ...`
- 同时 `heartbeat.csv` 仍持续增长
- 这条证据非常重要，因为它意味着：
  - 当前最新这版 extra logging 已经可能改变现场
  - 问题不再只是“`hw_dma_set_src()` 之后没回来”
  - 更可能已经前移成“critical path 上的 progress log / `fflush(stdout)` / 相关 syscall 本身把 guest 卡住”
- 所以下一轮最小验证的顺序要调整成：
  - 先把 `set_dst/set_src` 临界区的日志侵入性降下来
  - 再观察是否重新回到完整的 `before-set-src`
  - 只有在这一步恢复后，才继续用它区分 ReRoCC 映射漂移 vs `set_src` 真正不返回

## 本轮新增 instrumentation / probe

### 1. 软件已新增更窄的 DMA 提交日志

- 位置：
  [prt_dma.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_dma.c)
- 新增内容：
  - `src_mod64`
  - `dst_mod64`
  - `done_mod64`
  - `initial_wide_hint`
  - `full_byte_mode_hint`
  - `submit-fence begin/done`
  - `dma-wait fence-enter/done`

### 2. 日志 gating 已改成 submit 局部快照

- `dma_blocking_submit()` 现在会把：
  - `token_id`
  - `stage_idx`
  - `tensor_id`
  - `src/dst/bytes`
  先存在局部变量里
- 目的：
  - 如果 `set-dst` 之后 token 内存被意外改写，日志不至于因为 `dma_should_progress_log(tok)` 再次取值失败而整段消失

### 3. 已加入最小实验性行为改动

- `dma_blocking_submit()` 在 `set_dst/set_src` 前新增：
  - `fence rw, rw`
- 当前把它视为 probe，不视为 confirmed fix
- 只有新的 FPGA 运行结果能说明这条变化是否有效

### 4. 已加入新的 ReRoCC 映射窄日志

- [prt_rerocc.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_rerocc.c) 现在会在 `rr_set_opc()` 后立即打印：
  - `raw_cfg`
  - `cfg_acq`
  - `cfg_mgr`
  - `opc_map`
- [prt_dma.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_dma.c) 现在会在：
  - `before-set-dst`
  - `before-set-src`
  额外打印：
  - `rr_cpu`
  - `rr_cfg / rr_mgr / rr_opc`
  - `rr_opc_map`
  - `rr_cfg_raw / rr_cfg_acq / rr_cfg_mgr`
- 目的：
  - 区分“`custom2 -> cfg` 映射或 acquire 状态跑偏”与“Coupled-DMA 的 `SRC_INFO` ready 本身拉低”

## 本轮最小验证结果

- host build:
  - `make -C generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime clean all PIPELINE_RUNTIME_PROGRESS=1`
  - 通过
- RISC-V Linux 交叉编译:
  - `make -C generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests -f generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests/Makefile abs_top_srcdir=/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests CC_LINUX=/home/ubuntu/chipyard/.conda-env/riscv-tools/bin/riscv64-unknown-linux-gnu-gcc PIPELINE_RUNTIME_PROGRESS=1 rerocc_pipeline_runtime-linux`
  - 通过
- host closure:
  - `METHODS=ours2 BATCH=1 SKIP_EXPORT=1 SKIP_BUILD=1 bash generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/scripts/run_bertmini_host_closure.sh`
  - 通过，结果为 `BERTMINI_HOST_CLOSURE_PASS`
- 本轮还没有做新的 FPGA run

## 成功判据

- 不能只看 `runworkload` 返回 `0`
- 最终判据仍然是 guest `uartlog`
- 第一阶段 success 定义：
  - `bertmini`
  - `cpu` golden 正常生成
  - `fpga` backend 在误差阈值内对齐

## 当前最重要的下一步

不是继续设计新语义，而是：

1. 保留本轮“最后可见行”证据
2. 回收 run farm
3. 先确保 `marshal build/install` 不再被 `libguestfs` 的 `/var/tmp` 写权限阻塞
4. 只围绕首个 `dma-submit` 的停点继续加窄日志或检查硬件交互

在越过 `dma-submit token=1 set-dst done ...` 之前，不要继续扩展实现范围。
