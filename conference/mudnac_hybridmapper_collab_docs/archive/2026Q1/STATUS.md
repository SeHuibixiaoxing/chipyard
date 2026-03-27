# Pipeline Runtime Integration Status

日期：2026-03-26

先读：

- `HANDOFF.md`
- `PLAN.md`
- `LESSONS_LEARNED.md`

## 当前状态

- 2026-03-26 已完成一轮不依赖 replay 的 ReRoCC/Gemmini 静态并发审计，结论已经稳定，可直接作为后续设计边界。
  - 结论一：
    - “当前可以 acquire 16 个 Gemmini core” 不等于 “一个 hart 现在能同时直接驱动 16 个 core”
  - 结论二：
    - ReRoCC client 硬件里虽然有 `16` 个 `cfg` 和 `4` 个 `opcode` 槽，
      但当前软件栈把所有 Gemmini 指令都固定编码成 `custom3`
    - 所以单 hart 当前只有 `1` 条 live Gemmini route lane
  - 结论三：
    - DMA 固定走 `custom2`
    - 因此单 hart 当前实际可长期稳定持有的是：
      - `1` 条 Gemmini lane
      - `1` 条 DMA lane
  - 结论四：
    - 当前 runtime 的 `cfg` 分配是：
      - `cfg = ((stage_id * 2) + lane) % 16`
      - `lane=0` 给 DMA
      - `lane=1` 给 Gemmini
    - 因此单 hart 当前最多只有 `8` 组互不冲突的 stage-context
  - 结论五：
    - `rr_set_opc()` 不需要等 Gemmini 远端执行结束才允许重绑
    - 已经发出去的请求保留自己的 `cfg/client_id/manager_id`
    - 但 `rr_release()` 只释放 `cfg`，不会自动清掉 opcode 映射
  - 对未来目标的直接含义：
    - 若坚持 “一个 hart 管一个 action，同时并发多个 stage/多个 Gemmini issue 流”，当前实现不够
    - 若要保留真正 stage 并发，当前更接近可行的仍是 “多 stage worker / 多 hart”

- 2026-03-26 最新 Linux/F2 replay 已把 live boundary 从此前的 `spaddrs-dc` 进一步压缩到 `gemmini_extended_config_ld()` 调用点内部，并已据此落地一轮修复。
  - replay 结果目录：
    [2026-03-26--04-31-39-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime](/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-26--04-31-39-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime)
  - 手工抓回的现场：
    [uartlog](/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-26--04-31-39-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime/live-capture-deep/uartlog)
    [heartbeat.csv](/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-26--04-31-39-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime/live-capture-deep/heartbeat.csv)
  - 这一轮首先证明：
    - 之前看到的 `software IO TLB` 停顿不等于 kernel hang
    - guest 最终仍继续越过：
      - `/init`
      - `S99run`
      - `[bertmini] ...`
      - `runtime-init`
      - `load-model-bin end`
      - `segment=0`
      - `alloc-pages / exec-bind`
      - `stage-conv-desc`
      - `pointwise-inner ... matmul-call-enter`
      - `matmul-os spaddrs-dc`
  - 新增 `gcrit` 断点后，最深边界已经明确：
    - 已出现：
      - `[gcrit] matmul-os-biascfg-enter`
      - `[gcrit] matmul-os-biascfg-pre-ld`
    - 始终未出现：
      - `[gcrit] matmul-os-biascfg-post-ld`
    - 同时 heartbeat 继续推进到约 `9.70B` target cycles
    - 因此当前最准确表述是：
      - live stall 已经收敛到
        `sp_tiled_matmul_os()` 中 bias `config_ld` 调用内部/之后
      - 不再只是“停在 bias 附近”
  - 进一步静态对照 `gemmini.h` 后确认一个高可疑 software diff：
    - 当前 OS bias 路径原来使用：
      - `gemmini_extended_config_ld(D_stride, D_scale_factor)`，默认 `id=0`
      - `prt_gemmini_issue_bias_mvin0_debug(...)`
      - `gemmini_extended_mvin(...)`
      - `D_stride` 还硬编码为 `sizeof(acc_t)`
    - 但同文件参考实现已经明确使用：
      - `gemmini_extended3_config_ld(..., low_D, 2)`
      - `gemmini_extended_mvin3(...)`
      - `sizeof_D = low_D ? sizeof(elem_t) : sizeof(acc_t)`
  - 基于这条 diff，已落地修复：
    - [gemmini.h](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/include/gemmini.h)
      - bias `config_ld` 改为 `id=2`
      - bias move-in 改为 `mvin3`
      - bias DRAM 地址和 stride 改为按 `sizeof_D/low_D` 计算
      - debug helper 同步改为 `bias-mvin3`
  - 本地验证：
    - `make -C generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime clean all`
      - PASS
    - `METHODS=ours2 SKIP_BUILD=1 SKIP_EXPORT=1 WATCHDOG_MS=300000 ./scripts/run_bertmini_host_closure.sh`
      - PASS
  - 修补后 guest/workload 已重打：
    [rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-build-2026-03-26--04-54-53-3OFL8WWC82IZ0WYV.log](/home/ubuntu/chipyard/software/firemarshal/logs/rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-build-2026-03-26--04-54-53-3OFL8WWC82IZ0WYV.log)
  - 新一轮 FireSim replay 已启动：
    - launchrunfarm：
      [2026-03-26--04-55-21-launchrunfarm-HISRUXWB4JPA7SQ5.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-26--04-55-21-launchrunfarm-HISRUXWB4JPA7SQ5.log)
    - infrasetup：
      [2026-03-26--04-56-18-infrasetup-QT6MA2FK3YXNMH26.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-26--04-56-18-infrasetup-QT6MA2FK3YXNMH26.log)
    - 当前 active runworkload：
      [2026-03-26--05-00-12-runworkload-HJJRV1N3IIPJWISK.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-26--05-00-12-runworkload-HJJRV1N3IIPJWISK.log)
    - 当前实例：
      - instance id: `i-02d23599150444ce5`
      - private ip: `192.168.1.155`

- 2026-03-26 `biasfix` Linux/F2 replay 结果已经回收并定性：`id=2 + mvin3 + sizeof_D/low_D` 这轮修补没有把卡点推过去。
  - replay 结果目录：
    [2026-03-26--05-00-12-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime](/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-26--05-00-12-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime)
  - 手工抓取现场：
    [uartlog](/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-26--05-00-12-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime/live-capture-deep/uartlog)
    [heartbeat.csv](/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-26--05-00-12-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime/live-capture-deep/heartbeat.csv)
  - 同一轮 guest 已再次完整越过：
    - `/init`
    - `S99run`
    - `runtime-init`
    - `load-model-bin end`
    - `segment=0`
    - `alloc-pages / exec-bind`
    - `stage-conv-desc`
    - `pointwise-inner ... matmul-call-enter`
    - `matmul-os spaddrs-dc`
    - `matmul-os-biascfg-enter`
    - `matmul-os-biascfg-pre-ld`
  - 同时仍未出现：
    - `matmul-os-biascfg-post-ld`
    - `bias-loop`
    - `bias-mvin3-*`
    - `pointwise-inner ... matmul-call-return`
  - 关键结论：
    - 这轮修补前后，live boundary 没有发生可观测推进
    - 45 秒窗口内 `uartlog` 尾部完全不变
    - 但 `heartbeat` 从约 `13.12B` 持续增长到约 `14.10B` target cycles
    - 因此当前 active conclusion 是：
      - stall 仍稳定钉在 `biascfg-pre-ld -> post-ld` 之间
      - 仅把 OS bias 路径改成 `id=2 + mvin3` 还不足以修复
  - 静态补充结论：
    - [README.md](/home/ubuntu/chipyard/generators/gemmini/README.md) 已明确说明 Gemmini 有三条 `mvin` 指令，`config_mvin` 的 `rs1[4:3]=2` 就是配置 `mvin3`
    - [gemmini.cc](/home/ubuntu/chipyard/generators/gemmini/software/libgemmini/gemmini.cc) 也确认：
      - `mvin3_funct -> mvin(..., 2)`
      - `config_mvin` 的 `state_id = (rs1 >> 3) & 0x3`
    - 所以“这套硬件根本不支持 `mvin3/id=2`”当前没有证据支持
  - 本轮 run farm 已回收：
    - instance: `i-02d23599150444ce5`
    - latest observed state: `shutting-down`
  - terminaterunfarm log：
    [2026-03-26--05-25-16-terminaterunfarm-T5J17PQZJ90M6F0F.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-26--05-25-16-terminaterunfarm-T5J17PQZJ90M6F0F.log)

- 2026-03-26 上述静态并发审计没有关闭当前卡死问题。
  - 当前 active blocker 仍然是 Linux/F2 guest runtime 内的 functional deadlock
  - 当前最重要的未解问题仍是：
    - pointwise OS path 为何会在 `bias config_ld` 周边停住
    - 以及该停顿与当前 runtime-specific state / ReRoCC scope / xlate / route binding 是否有关
  - 因此下一轮主线仍应是：
    - 先静态对照 runtime-specific diff
    - 必要时用 baremetal 复原 runtime-style issue 顺序
    - 最后才 replay Linux/F2

- 2026-03-26 当前项目执行策略已临时切换为：
  - 所有 activation 先按 `RELU` 处理
  - 这是当前调试期的人为策略，不代表 artifact contract 已经补齐 activation 字段
  - 因此此前文档中把 `NO_ACTIVATION` 当作当前 Linux 主线语义的结论，现已过期

- 2026-03-26 `pipeline-runtime` 已按当前策略改成默认 `RELU`，并保留深层 descriptor marker：
  - `pipeline-runtime/src/prt_runtime.c`
    - `prt_default_conv_activation(...)`
      - 当前固定返回 `RELU`
      - 注释已明确说明：这是临时 project policy
    - `prt_default_conv_output_scale(...)`
      - 仍固定 `1.0f`
  - descriptor 级 marker 继续保留：
    - `stage-conv-desc ...`
    - 直接打印：
      - `stage/layer/type`
      - `N/IC/OC/OH/OW/KH/KW/G/stride/pad`
      - `in_stride/weight_stride/out_stride`
      - `act/scale/tiled_type`
      - `bias/weights/input/output`
  - 本地验证：
    - `make -C generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime clean all`
      - PASS
    - `METHODS=ours2 SKIP_BUILD=1 SKIP_EXPORT=1 WATCHDOG_MS=300000 ./scripts/run_bertmini_host_closure.sh`
      - `BERTMINI_HOST_CLOSURE_PASS`

- 2026-03-26 最新 Linux/F2 replay 在 `RELU` 策略下已把 live boundary 钻到更深处，并收敛出新的最深卡点：
  - FireMarshal build：
    [rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-build-2026-03-26--03-31-04-80N3K295M6R0VQAO.log](/home/ubuntu/chipyard/software/firemarshal/logs/rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-build-2026-03-26--03-31-04-80N3K295M6R0VQAO.log)
  - FireSim result dir：
    [2026-03-26--03-38-43-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime](/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-26--03-38-43-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime)
  - runworkload log：
    [2026-03-26--03-38-43-runworkload-EQ4F4BWGLYZB8O5X.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-26--03-38-43-runworkload-EQ4F4BWGLYZB8O5X.log)
  - 手工抓取证据：
    [uartlog.stall](/home/ubuntu/chipyard/tmp/firesim-aws-f2/debug-captures/bertmini-relu-stall/uartlog.stall)
    [heartbeat.stall.csv](/home/ubuntu/chipyard/tmp/firesim-aws-f2/debug-captures/bertmini-relu-stall/heartbeat.stall.csv)
  - 同一轮 guest 已连续越过：
    - Linux boot / rootfs / `S99run`
    - `runtime init-step=load-model-bin end`
    - `runtime init-step=ready`
    - `segment=0 begin`
    - `alloc-pages / xlate-install / exec-bind`
    - `stage-conv-desc ... act=1 scale=1`
    - `pointwise-inner ... matmul-call-enter`
    - `matmul-nn-stride-auto-enter`
    - `matmul-auto-enter`
    - `matmul-inner ...`
    - `matmul-os derived`
    - `matmul-os spaddrs-ab`
    - `matmul-os spaddrs-dc`
  - 当前仍未看到完整落盘的后续 marker：
    - `matmul-os bias-config`
    - `matmul-os bias-loop`
    - `pointwise-inner ... matmul-call-return`
    - `scope-drain ...`
  - 最关键的新证据：
    - `uartlog` 最后 256B 稳定收敛为：
      - `... matmul-os spaddrs-dc ...`
      - 后面只多出半截 `"[ge"`
    - 同时 `heartbeat.csv` 继续从约 `10.46B` 推进到约 `12.61B` target cycles
    - 在这段时间内 `uartlog` 文件大小固定为 `48392B`，超过 3 分钟没有再增长
  - 当前最合理表述应是：
    - active Linux/F2 boundary 已压到
      `matmul-os spaddrs-dc` 之后、下一条 `[gemmini-phase] ...` 完整写出之前
    - 下一条最可能本应出现的是 `matmul-os bias-config`
    - 但由于现场停在半截 `"[ge"`，当前还不能把它简单定性成
      “一定已经执行到 bias-config 并死在 bias-config 之后”
    - 需要把根因继续收敛在：
      - Linux guest 串口/日志写出路径是否把超细粒度日志本身卡住
      - 或用户态确实在 `spaddrs-dc -> bias-config` 之间遇到异常/阻塞
  - 本轮 run farm 已立即回收：
    - instance: `i-090baa78277de1f6d`
    - latest observed state: `shutting-down`
  - terminaterunfarm log：
    [2026-03-26--04-01-23-terminaterunfarm-19OG27NOFUCFVC6B.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-26--04-01-23-terminaterunfarm-19OG27NOFUCFVC6B.log)

- 2026-03-25 baremetal/F2 上新增的 deepest Gemmini 断点已经把 runtime-style pointwise OS 路径完整钻穿到 `CASE_RESULT`，并证明当前 stall 不在 baremetal 的 Gemmini 内核主路径里。
  - 本轮新增断点位置：
    - `include/gemmini.h`
      - `matmul-config pre/post ex/st/ld-{a,b,d}`
      - `matmul-os derived`
      - `bias-iter / b-iter / a-iter / compute-iter / mvout-iter`
      - `pre/post bias-config-ld`
  - 本轮 baremetal/F2 结果：
    - `runtime_style_only` 模式下，
      `pointwise_matmul_os_chunked_128_to_64_runtime_style_cross_1kb_contiguous`
      已完整通过：
      - `chunk_issue_begin oc_beg=0`
      - 第一块 `matmul-config -> matmul-os bias/B/A/compute/mvout -> fence`
      - `chunk_issue_end oc_beg=0`
      - `chunk_drain_begin/end oc_beg=0`
      - `chunk_issue_begin oc_beg=64`
      - 第二块再次完整越过 `matmul-config -> matmul-os bias/B/A/compute/mvout`
      - `chunk_issue_end oc_beg=64`
      - `CASE_RESULT ... PASS_STALL_DIAG`
    - 随后
      `pointwise_matmul_os_chunked_128_to_64_runtime_style_cross_1kb_interleaved`
      也已启动，并至少越过：
      - `CASE_START`
      - `CASE_MAP region=A/B/BIAS`
  - 当前最重要结论：
    - baremetal runtime-style contiguous 已证明：
      - 不会卡在 `matmul-config`
      - 不会卡在 `bias-mvin0`
      - 不会卡在 `B/A mvin`
      - 不会卡在 `preload / compute / mvout`
    - 因此先前 Linux/pipeline-runtime 侧 observed stall 不能再归因于
      “Gemmini OS pointwise/bias mvin 在 baremetal 上天然会挂”。
    - active suspicion 进一步收敛到：
      - pipeline-runtime specific state
      - chunk/case 之间的 runtime contract
      - 或 Linux/runtime 上下文，而不是 baremetal 内核本身
  - 本轮 run farm 已回收：
    - instance: `i-026987515b8a82a30`
    - latest observed state: `shutting-down`

- 2026-03-25 host closure gate 已重新恢复为绿色，之前的 `segment=3 -> action_alloc_spm failed rc=not_implemented(-8)` 已修复。
  - 复现命令：
    `METHODS=ours2 BATCH=1 SKIP_BUILD=1 SKIP_EXPORT=1 WATCHDOG_MS=120000 ./scripts/run_bertmini_host_closure.sh`
  - 当前结果：
    - `BERTMINI_HOST_CLOSURE_PASS`
  - 根因不是 mapping contract，而是 host-only PT pool sizing：
    - `segment=3` 的 `segmentSpmPageSpan=1289`
    - 需要的 PTE slice 大小为 `1289 * 8 = 10312B`
    - 按 host page 对齐后为 `12288B`
    - 旧代码在非 `__riscv` host 上把 PT chunk 错误限制成单个 host page（通常 `4KB`）
    - 所以 `pt_pool_alloc_slice()` 会直接返回 `PRT_ERR_NOT_IMPL`
  - 现已修复：
    - `pipeline-runtime/src/prt_page_table.c`
      - PT chunk sizing 现在至少覆盖单 action 最大 PT slice
    - `pipeline-runtime/src/prt_schedule_action.c`
      - 补齐 Linux feature macro，避免 `clean all` 时 `MAP_ANONYMOUS` 未声明
  - 这条 host fix 重新确认：
    - per-action dedicated alias window / PTBR 设计在 host runtime 上仍然自洽
    - 当前 active blocker 仍是 guest runtime on Linux/F2 的 pointwise OS path

- 2026-03-25 已完成一轮 fresh FireMarshal install，下一轮 F2 replay 不应再吃到旧 guest binary。
  - install log：
    [rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-install-2026-03-25--16-55-39-22LXE3XYE2TOQ013.log](/home/ubuntu/chipyard/software/firemarshal/logs/rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-install-2026-03-25--16-55-39-22LXE3XYE2TOQ013.log)
  - installed workload：
    [rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime.json](/home/ubuntu/chipyard/sims/firesim/deploy/workloads/rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime.json)
  - install 之前已先把 overlay source binary 替换为当前 rebuild 的 guest binary，
    并确认其中包含：
    - `spm-pt pool init`
    - `scope-drain begin`
    - `pointwise-inner precall`
  - 因此下一步若继续 F2 replay，重点应转向：
    - `launchrunfarm -> infrasetup -> runworkload`
    - 再用新的 deepest marker 判断 stall 是否仍停在 `matmul-call-enter` 之后

- 2026-03-25 最新 Linux/F2 replay 已确认：当前 runtime-specific diff 没有把 stall 推进到更深处，反而把 active boundary 提前到了 `pointwise-inner ... matmul-args` 之后。
  - 结果目录：
    [2026-03-25--13-31-38-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime](/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-25--13-31-38-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime)
  - manager 日志：
    [2026-03-25--13-31-38-runworkload-FKB6U0HSDTRXDY29.log](/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-25--13-31-38-runworkload-FKB6U0HSDTRXDY29.log)
  - 同一轮 guest 已连续越过：
    - Linux early boot
    - `S99run`
    - `[bertmini] hugetlb before/after`
    - `[bertmini] method=ours2 cores=2 gemmini=2 dma=2`
    - `runtime init-step=validate-artifacts end`
    - `runtime init-step=load-model-bin end`
    - `runtime init-step=load-input-blob end`
    - `runtime init-step=ready`
    - `segment=0 begin`
    - 新增的 `alloc-pages` 全量逐页日志
    - 新增的 `exec-bind` 全量逐页日志
    - `pointwise-inner ... matmul-begin`
    - `pointwise-inner ... matmul-args`
  - 当前仍未出现：
    - `matmul-nn-stride-auto-enter`
    - `matmul-auto-enter`
    - `matmul-os-enter`
    - `bias-mvin0-*`
  - live 结论：
    - `uartlog` 按 Python `splitlines()` 读取时稳定停在 `757` 行
    - 同时 `heartbeat.csv` 从约 `9.89B` 继续推进到约 `10.78B` target cycles
    - 因此这不是 Linux boot stall，而是新的、更早的 pointwise compute boundary
  - 新日志本轮已经证明：
    - all-bank 分配后的逐页 `ppn / acc_id / local_page_idx / paddr`
    - exec-view 逐页 `vpage / ppn / pte / paddr`
    都已经成功落到 guest 中
  - 与旧 replay 对比：
    - 旧 replay 在相近 heartbeat 已经进入 `matmul-os`，甚至压到首个 bias `mvin`
    - 本轮 boundary 明显更早，说明本轮 runtime-specific delta 至少引入了一个新的前移边界
  - 本轮 run farm 已回收：
    - instance: `i-0b5c4ca11a398b3fe`
    - latest observed state: `shutting-down`

- 2026-03-25 root-cause 主线已经从 Linux/F2 慢 replay 切换到 baremetal 最小复现。
  - 原因：
    - Linux/F2 的 live boundary 已经压到首个真实 `k_MVIN`
    - 继续在 Linux/F2 上只做更细粒度日志，单位信息成本过高
  - 当前用于 baremetal 复原的 Linux 现场参数已经固定：
    - 外层 pointwise subcall：
      - `I=256 J=64 K=256 fallback=OS`
    - 首个进入 `sp_tiled_matmul_os()` 的 local tile：
      - `I=32 J=8 K=32`
      - `pad_I=0 pad_J=0 pad_K=0`
      - `stride_A=256 stride_B=256 stride_D=256 stride_C=256`
      - `act=1(RELU)`
      - `repeating_bias=1`
      - `D_stride=0`
    - 首个真实 bias move-in：
      - `bias_row=0`
      - `dram=0x3fa9042000`
      - `sp=0x80000000`
      - `blocks=2`
      - `cols=16`
      - `rows=8`
      - `rs2=0x8001080000000`
  - 因此当前最有价值的下一步不是再做 Linux 缩点，而是：
    - 先在 baremetal 上复原这条首个 `bias mvin(D -> acc)` 本身
    - 先区分：
      - 单条 `k_MVIN` 就会卡住
      - 还是必须带上 pointwise/OS 的更大上下文才会卡住

- 2026-03-25 Linux/F2 最深定位已经把 live boundary 压到首个 bias `mvin` 的真实 ROCC 指令发射点。
  - 结果目录：
    [2026-03-25--10-46-01-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime](/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-25--10-46-01-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime)
  - 手工抓取：
    [uartlog.live](/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-25--10-46-01-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime/manual-capture/uartlog.live)
    [heartbeat.live.csv](/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-25--10-46-01-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime/manual-capture/heartbeat.live.csv)
  - 同一轮 guest 已连续越过：
    - `bias-mvin0` 参数拆分前后
    - `bm0c0`
    - `bm0ce`
    - `bias-mvin0-call-rs1`
    - `bias-mvin0-call-rs2`
    - `bm0ca`
  - 当前仍未出现：
    - `bm0cb`
    - `bm0c1`
    - `matmul-os-after-bias`
  - 这说明：
    - 当前不再是外层日志盲区
    - `rs1/rs2` 打包也已经完成
    - active boundary 现在就在首个 bias `mvin` 的真实 `.insn r CUSTOM_0 ... k_MVIN` 发射点
  - 本轮 run farm 已回收：
    - instance: `i-071dd910f015888f3`
    - state after terminate request: `shutting-down`
  - 因此下一阶段主线应从 Linux/F2 慢 replay 转到 baremetal 复原：
    - 用更短闭环复现“首个 bias `mvin(D -> acc)` issue”
    - 在 baremetal 上验证这条指令本身是否卡住，以及是哪些地址/配置组合触发

- 2026-03-25 Linux/F2 高细节定位已取得一轮可用于 baremetal 复原的现场。
  - 结果目录：
    [2026-03-25--08-04-25-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime](/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-25--08-04-25-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime)
  - 手工抓取：
    [uartlog.live](/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-25--08-04-25-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime/manual-capture/uartlog.live)
    [heartbeat.live.csv](/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-25--08-04-25-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime/manual-capture/heartbeat.live.csv)
  - 同一轮 guest 已连续越过：
    - `load-model-bin`
    - `load-input-blob`
    - action 级 alias/xlate/bind
    - `pointwise-inner ... matmul-begin`
    - `matmul-auto`
    - `tiled_matmul` entry
    - `matmul-outer begin`
    - `matmul-inner begin/ptrs/shape/flags`
    - `tiled-pre-innercall`
    - `matmul-os-enter`
    - `matmul-os ptrs/shape/flags`
  - 当前最新稳定可见边界：
    - `matmul-os spaddrs` 这一组日志里，最后一行只输出到
      `D=0x8000`
    - 未见：
      - `matmul-os-after-bias`
      - `matmul-os-after-b`
      - `matmul-os-after-a`
      - `matmul-os-after-compute`
      - `pointwise-inner ... matmul-end`
  - 这说明：
    - 当前问题已不再位于 Linux bring-up、artifact 装载、action/xlate/bind、`tiled_matmul` 外层入口，
      也不再位于 `matmul-inner begin` 那条日志本身
    - 当前边界已经收敛到 `sp_tiled_matmul_os()` 入口极早期，
      更具体地说是在 `matmul-os spaddrs` 参数打印附近，且早于 `D` bias move-in 完成
  - 为继续把这一步再切细，本轮代码已继续预埋但尚未 replay 的新日志：
    - 将 `matmul-os spaddrs` 拆成 `spaddrs-ab` 与 `spaddrs-dc`
    - 在两条日志前后都加了 raw guard
    - 下一轮 replay 的目标是判断：
      - 卡在 `spaddrs-ab`
      - 卡在 `spaddrs-dc`
      - 或已经进入 `D` bias move-in
    - 这版 split-spaddrs guest 已完成：
      - `HOST_INIT_CHECK_ONLY=1 bash host-init.sh` PASS
      - 完整 `bash host-init.sh` PASS
      - `marshal build` PASS
      - `marshal install` PASS
    - 因此下一轮只需基于当前 fresh rootfs 继续做：
      - `launchrunfarm`
      - `infrasetup`
      - `runworkload`
  - 本轮 run farm 已回收：
    - instance: `i-0d4390fd8b863e7a2`
    - current state: `terminated`

- 2026-03-25 Linux/F2 新一轮 replay 已再次把 live boundary 向前推进。
  - 结果目录：
    [2026-03-25--08-48-52-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime](/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-25--08-48-52-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime)
  - 手工抓取：
    [uartlog.live](/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-25--08-48-52-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime/manual-capture/uartlog.live)
    [heartbeat.live.csv](/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-25--08-48-52-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime/manual-capture/heartbeat.live.csv)
  - 同一轮 guest 已连续越过：
    - `matmul-os spaddrs-ab`
    - `matmul-os spaddrs-dc`
    - `matmul-os bias-config`
    - `matmul-os bias-loop`
    - `matmul-os-pre-bias-mvin0`
  - 当前最后稳定可见日志：
    - `[gemmini-phase] matmul-os bias-mvin0 bias_row=0 dram=0x3f9c10e00`
  - 仍未出现：
    - `matmul-os-post-bias-mvin0`
    - `matmul-os-after-bias`
    - `matmul-os-after-b`
    - `pointwise-inner ... matmul-end`
  - 这说明：
    - `spaddrs` 已不再是 active boundary
    - 当前新的盲区再次落在一条较长的 `gemmini-phase` 参数日志本身
    - 更具体地说，边界已经推进到首个 bias move-in 的参数打印行内部，而不是停在 `spaddrs` 或 bias-config 之前
  - 本轮 run farm 已回收：
    - instance: `i-0ee3dde60adc8c5df`
    - current state after terminate: `shutting-down`

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

- HybridMapper -> pipeline-runtime 的 pre-orchestrated contract 已落地。
  - segment 级 metadata 已包含：
    - `segmentSpmPageSpan`
    - `bufferBinding*List`
  - stage 级 metadata 已包含：
    - `execBaseVPage`
    - `localSpmTensorAddrList / localSpmFirstVPageList / localSpmPageCountList / localSpmTensorBytesList`
    - `entryBufferIdList / exportBufferIdList`
  - runtime 执行期现在只做：
    - action 级 accelerator 分配
    - 独立连续 alias VA window / vpage interval 分配
    - all-bank shared-spad 物理页分配
    - PTE 与 shared-spad xlate 配置
  - 每个 pipeline segment/action 都使用独立 alias VA window，避免多模型动态编排时长期复用全局 xlate VA 而碎片化。

- `bertmini` fresh exporter 已修通。
  - `python3 conference/HybridMapper/scripts/create-pipeline-runtime-artifacts.py --model bertmini`
    现在可以直接生成 `ours2 / gemini2 / tangram2` runtime artifacts。

- host closure gate 已重新打通。
  - `ours2 / gemini2 / tangram2` 当前都已通过：
    - CPU golden dump
    - host `fpga` backend golden compare
  - host runtime 不再是 active blocker。

- 当前 active blocker 已推进到 Linux packaging / FireMarshal / FireSim F2 主线。
  - baremetal 不再是 blocker。
  - host runtime 不再是 blocker。
  - 后续任何 runtime 修改，只要影响 shared-spad / resadd / pointwise 路径，都应同时回归：
    - baremetal gate
    - host closure gate

- 当前不应假定仍有 active run farm。
  - 最近一轮 on-demand 实例已经 terminate。

- `spot` 试跑已经给出结论，但当前不可用。
  - runtime config：
    [config_runtime_f2_rerocc_lc_baremetal_explicit_interleaved_small_spot.yaml](/home/ubuntu/chipyard/sims/firesim/deploy/config_runtime_f2_rerocc_lc_baremetal_explicit_interleaved_small_spot.yaml)
  - 结论：
    AWS 当前对 `f2.6xlarge spot` 持续返回 `insufficient capacity`
  - 当前判断：
    不是 workload / AGFI / FireSim manager 配置错误。

## 本轮已整理的实现落地点

### 1. HybridMapper exporter 已改成直接导出 runtime 真正要执行的 contract

- 主文件：
  `conference/HybridMapper/scripts/create-pipeline-runtime-artifacts.py`
- 已落地的关键导出字段：
  - `segmentSpmPageSpan`
  - `bufferBindingIdList / bufferBindingKindList / bufferBindingFirstPageList / bufferBindingPageCountList`
  - `execBaseVPage`
  - `entryBufferIdList / exportBufferIdList`
  - `localSpmTensorAddrList / localSpmFirstVPageList / localSpmPageCountList / localSpmTensorBytesList`
- 本轮同时修掉了两个会直接破坏 fresh artifact 的 exporter 问题：
  - `layer_id=0` 被 Python truthiness 误转成 `-1`
  - 长 flow-style list 自动折行后，runtime line-oriented loader 无法完整读入

### 2. pipeline-runtime 已从“运行时重建布局”改成“执行期兑现编排期合同”

- 关键文件：
  - `pipeline-runtime/src/prt_runtime.c`
  - `pipeline-runtime/src/prt_schedule_action.c`
  - `pipeline-runtime/src/prt_page_table.c`
  - `pipeline-runtime/include/prt_runtime.h`
  - `pipeline-runtime/include/prt_schedule_action.h`
  - `pipeline-runtime/include/prt_page_table.h`
  - `pipeline-runtime/include/prt_types.h`
- 已落地行为：
  - 每个 pipeline segment/action 申请独立 alias VA window
  - 每个 action 获得独立连续 vpage interval
  - physical page 按 exported `bufferBinding*List` 分配并绑定
  - stage 执行视图直接消费 exported `execBaseVPage` 与 `localSpm*` metadata
  - runtime 不再临时推导 segment-local tensor/buffer 拓扑

### 3. artifact 校验逻辑已收敛到“pipeline YAML 为编排期真相”

- 关键文件：
  - `pipeline-runtime/src/prt_gemmini_artifacts.c`
  - `pipeline-runtime/src/prt_yaml_loader.c`
- 已落地行为：
  - 若 pipeline YAML 已导出 exact local layout，则 validator 以这份布局为主
  - layer-mapping 在当前 contract 下主要承担布局校验，而不是反向覆盖 pipeline split/layout 语义
  - 调试期临时加入的 YAML 进度日志已回退到默认关闭状态

### 4. 回归与闭环状态已经切换

- host path 已经证明当前 page-placement / manager contract 至少在 host 路径上可执行。
- 因此 runtime 主线不再优先怀疑：
  - alias VA window 设计
  - per-action vpage 设计
  - all-bank physical page 分配主逻辑
- 当前主怀疑已经转移到 Linux packaging / FireMarshal / FireSim F2 guest 路径。

## 本轮新增闭环问题

### 1. fresh exporter 的 `layer_id=0` 误过滤已闭环

- 现象：
  - `bertmini` fresh export 在 layer 0 报：
    `expected exactly one runtime layer mapping ... got 0`
- 根因：
  - exporter 中多处使用 `int(value or -1)` 一类写法。
  - 对 `layer_id=0` 来说，`0` 被当成假值，最终被错误转成 `-1`。
- 修复：
  - 改成显式 `None` 判定，而不是依赖 Python truthiness。
- 结论：
  - 这是 exporter 的零值处理 bug。
  - 不是 HybridMapper mapping 缺失。

### 2. 长 inline list 折行导致 runtime YAML 解析截断已闭环

- 现象：
  - 新导出的 `bufferBindingKindList` 等长列表在 YAML 中被自动折行。
  - runtime 现有 line-oriented loader 只读到首行，后续 continuation line 被丢掉。
- 根因：
  - exporter 的 YAML dumper 默认会对长 flow-style list 自动换行。
- 修复：
  - 把 exporter 的 YAML 输出宽度调大，禁止这类 list 自动折行。
- 结论：
  - 这是 exporter/runtime 文本契约不一致。
  - 不是 runtime buffer-binding 语义错误。

### 3. artifact validator 的 split-kind 假阳性已闭环

- 现象：
  - pipeline 已明确导出 stage-local exact layout，但 validator 仍因 `split_kind` 与 layer-mapping candidate 不一致而拒绝执行。
- 根因：
  - validator 默认把 layer-mapping 的 `split_kind` 当成更高优先级真值。
  - 这与当前“pipeline YAML 是编排期真相、layer-mapping 主要用于布局校验”的 contract 不一致。
- 修复：
  - 若 pipeline 已导出 exact local layout 且与命中的 layout 一致，则保留 pipeline 的 `splitKind`，只把 layer-mapping 当作布局校验来源。
- 结论：
  - 当前 contract 下，pipeline YAML 的编排语义优先级高于 validator 对 split 的反推。

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

## 2026-03-25 本轮更新

- `pipeline-runtime/src/main.c` 入口已经显式关闭 `stdout/stderr` 缓冲。
  - 因此前次 Linux/F2 stall 边界误读，不应再简单归因于 `printf` 默认缓冲未关。
- 为减少 Linux/F2 前段噪声并保留必要边界：
  - `rerocc-linux-tests-coupleddma/workload/host-init.sh`
    默认 `PIPELINE_RUNTIME_PROGRESS` 已从 `1` 改为 `0`
  - `pipeline-runtime/src/prt_gemmini_adapter.c`
    pointwise chunked fallback 已移除 raw/hot path 日志
  - 当前只保留少量普通边界日志：
    `begin / chunk-begin / chunk-drain / chunk-end / end`
- Linux packaging gate 已在当前 head 重新确认：
  - `HOST_INIT_CHECK_ONLY=1 bash host-init.sh` PASS
  - 在 `cd sims/firesim && source sourceme-manager.sh --skip-ssh-setup` 环境下，
    完整 `bash host-init.sh` PASS
  - `rerocc_pipeline_runtime-linux` 已重新编译并重新 stage 到 overlay
  - rebuilt binary 已不再包含 `[prt-early] enter main`，说明低噪声编译参数已生效
- 对此前 baremetal 暴露问题的 runtime 复查结论：
  - `mvin2` accumulator dirty-row 问题：
    当前 `pipeline-runtime` bertmini 主路径未复用当时那条手写 explicit `mvin2` 覆盖序列
  - `rr_fence(cfg_id)` manager-visible completion 问题：
    当前 runtime sync path 已显式使用 `rr_fence` 语义
  - baremetal pointwise `J=128` 的 chunk-bias VA/PTE overlap：
    当前 runtime 通过 action 级独占 alias VA window 和独立 vpage 分配，结构上已避开旧问题
- 上述 runtime 复查现在已有更直接的代码锚点：
  - `pipeline-runtime/src/prt_gemmini_adapter.c`
    - `resadd_issue_scoped(...)` 已显式保持标准 WS resadd path，并保留注释说明不再回到旧的 handwritten explicit `mvin/mvin2/mvout` workaround
    - `conv_call_for_manager_sync*` 与 `resadd_call_for_manager_sync(...)` 路径都显式调用 `prt_rr_fence_scope(...)`
  - `pipeline-runtime/src/prt_schedule_action.c`
    - `alloc_action_alias_window(...)` 为每个 action 申请独立 alias window
    - `prt_action_alloc_spm(...)` 为每个 action 保留独立连续 `alias_vpage_start..alias_vpage_start+alias_page_count`
    - `configure_action_spm_xlate(...)` 把该 action 的 alias range 下发到本 action 占用的全部 Gemmini manager
  - `pipeline-runtime/src/prt_page_table.c`
    - `prt_spm_reserve_vpages(...) / prt_spm_release_vpages(...)` 负责 action 级连续 vpage interval 的保留与回收
- 因此如果下一轮 Linux/F2 仍失败，优先怀疑的已不再是这些 baremetal 已闭环根因，
  而是 guest/F2 特有的包装、启动或运行时语义。
- 为后续 baremetal 快速复现和 guest/F2 现场取证，本轮又新增了一组低噪声 marker：
  - `action=%u segment=%u alloc-acc ...`
  - `action=%u segment=%u alloc-spm ...`
  - `action=%u segment=%u xlate-range-begin/end ...`
  - `action=%u segment=%u release ...`
  - 这些日志位于 `pipeline-runtime/src/prt_schedule_action.c`
  - `HOST_INIT_CHECK_ONLY=1 bash host-init.sh` 已重新通过，说明新增 marker 没有破坏当前 guest build
- 为了进一步把 runtime/baremetal 复现上下文补齐，本轮又补了一层更细的低噪声 marker：
  - `action=%u segment=%u xlate-install mgr=%u ...`
  - `action=%u segment=%u bind-plan buffer=%u tensor=%u kind=%s ...`
  - `action=%u segment=%u bind-stage stage=%u layer=%u split=%u ...`
  - `action=%u segment=%u bind-topology weights=%u pipes=%u rings=%u ...`
  - `pointwise-inner stage=%u mgr=%u matmul-args in=... weights=... bias=... out=...`
  - 目的不是恢复旧的 noisy progress，而是把：
    - action 级资源兑现
    - buffer 级绑定计划
    - per-manager xlate 下发
    - inner matmul 的实际参数/地址
    固定在 marker-only 路径里，便于 Linux/F2 和 baremetal 共用同一套现场信息
- 这些新增 marker 已重新本地验证通过：
  - 在 `cd sims/firesim && source sourceme-manager.sh --skip-ssh-setup` 环境下
    重新执行：
    `PIPELINE_RUNTIME_GEMMINI_PHASE=1 PIPELINE_RUNTIME_ONLY_MARKER=1 bash host-init.sh`
    PASS
  - rebuilt `rerocc_pipeline_runtime-linux` 已包含：
    - `bind-plan`
    - `bind-stage`
    - `bind-topology`

## 2026-03-25 静态复查追加结论

- 当前 Linux workload 默认是：
  - `PIPELINE_RUNTIME_ONLY_MARKER=1`
  - `PIPELINE_RUNTIME_GEMMINI_PHASE=0`
- 因此本轮 F2 replay 里，本来就不应该看到：
  - `matmul-nn-stride-auto-enter`
  - `bias-mvin0-*`
  - 其它 `gemmini-phase` / `graw` 细粒度日志
- 之前把“没出现 `matmul-nn-stride-auto-enter`”当作新的稳定卡点边界，这个判断不成立。

- 更重要的静态语义问题出在 action-local alias window 与 PTE 索引的配合上：
  - baremetal 正例和 `rerocc_linux_spm_xlate.h` 都明确表明，硬件用的是：
    `vpage = (vaddr - range_base) >> page_shift`
  - 当前硬件接口只有：
    - `SPM_XLATE_CFG(ptbr, pte_count, page_shift, enable)`
    - `SPM_XLATE_RANGE(range_base, range_size)`
  - 当前接口里没有“再额外加一个 software-only `alias_vpage_start` 偏移”的硬件语义

- 因此旧实现里的这组逻辑是错误的：
  - `prt_action_alloc_spm(...)` 先为 action 保留全局 `alias_vpage_start`
  - `stage_prepare_exec_views(...)` 绑定到：
    `alias_vpage_start + stage->exec_base_vpage + local_spm_first_vpage`
- 这会把 action 的 exec view 装进全局 PTE 的偏移槽位，
  但 controller 实际访问同一个 alias VA window 时，只会从该 window 的 `vpage 0` 开始索引。

- 本轮已做的代码修正：
  - `pipeline-runtime/src/prt_runtime.c`
    - `stage_prepare_exec_views(...)` 改为按 action-local window 直接绑定：
      `exec_vpage = stage->exec_base_vpage + local_spm_first_vpage`
  - `pipeline-runtime/src/prt_schedule_action.c`
    - action path 不再为当前主执行路径保留 software-only `alias_vpage_start`
    - action cleanup / release 改为直接清理 action-local window 的 `vpage [0, alias_page_count)`

- 这也意味着一个更深的架构事实已经确认：
  - 如果未来真要支持“多模型并发、多个 action 同时 active、且它们共享同一份全局 PTE array”，
    那么仅靠 `range_base/range_size` 加 `alias_vpage_start` 是不够的。
  - 真正自洽的做法只能是二选一：
    - 单 active action：所有 action-local window 统一复用 PTE `0..N-1`
    - 多 active action：每个 action / manager 需要自己的 PTBR 视图，或者硬件新增可见的 PTE index base
- 下一轮 Linux/F2 replay 所需镜像也已预先刷新：
  - `marshal build` PASS
  - `marshal install` PASS
  - 因 FireSim workflow 约束，真正切到这版镜像时仍需重新跑一次：
    `infrasetup -> runworkload`
- 2026-03-25 最新 replay 已再次收敛出更窄的 stall 边界：
  - 结果目录：
    `/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-25--05-22-18-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime`
  - 手工现场抓取：
    - `manual-capture/uartlog.live`
    - `manual-capture/heartbeat.live.csv`
  - 该轮再次稳定走到：
    - `segment=0 begin`
    - `action=1 segment=0 alloc-acc`
    - `alloc-spm`
    - `workers-launched`
    - `pointwise-inner ... matmul-begin I=256 J=64 K=256 fallback=OS`
  - 但在 `matmul-begin` 之后：
    - heartbeat 又从约 `9.48B` 推到约 `10.46B`
    - 仍没有：
      - `matmul-end`
      - `subcall-return / chunk-end / gemm-exit`
  - 这轮 replay 只能证明：
    - marker-only 路径在 `matmul-begin / matmul-call-enter` 之后再也没有回到 caller
    - 不能再用“缺少 `[gemmini-phase] ...`”去推断更细边界，因为这轮 binary 默认根本没编入那类日志
- 基于这轮新边界，本轮又追加了两类低噪声 marker：
  - runtime init-step marker：
    - `runtime init-step=load-model-yaml ...`
    - `runtime init-step=collect-model-io ...`
    - `runtime init-step=load-pipeline-yaml ...`
    - `runtime init-step=validate-artifacts ...`
    - `runtime init-step=load-model-bin ...`
    - `runtime init-step=load-input-blob ...`
    - `runtime init-step=map-model-inputs ...`
    - `runtime init-step=ready ...`
  - Gemmini phase 入口 marker：
    - `[gemmini-phase] matmul-auto-enter ...`
- 这些新增 marker 已重新验证进入 guest binary：
  - `strings rerocc_pipeline_runtime-linux` 已确认包含：
    - `runtime init-step=load-model-yaml`
    - `runtime init-step=validate-artifacts`
    - `runtime init-step=ready`
    - `[gemmini-phase] matmul-auto-enter`
- 基于这版新 binary 的 FireMarshal 镜像已经再次刷新：
  - `marshal build` PASS
  - `marshal install` PASS
- 2026-03-25 再下一轮 replay 也已经拿到更完整的同边界证据：
  - 结果目录：
    `/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-25--05-53-06-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime`
  - 手工现场抓取：
    - `manual-capture/uartlog.live`
    - `manual-capture/heartbeat.live.csv`
  - 该轮已明确走到：
    - `runtime init-step=load-model-bin ...`
    - `segment=0 begin`
    - `bind-plan / xlate-install / bind-topology`
    - `pointwise-inner ... matmul-begin I=256 J=64 K=256 fallback=OS`
  - 但在 `matmul-begin` 之后：
    - heartbeat 继续从约 `11.12B` 推到约 `11.50B`
    - 仍没有：
      - `matmul-end`
      - 任何 `[gemmini-phase] ...`
      - `subcall-return / chunk-end / gemm-exit`
  - 这说明当前 stall 不是 outer runtime init/binding 缺失，
    而是稳定停在同一个 inner matmul entry blind spot
  - 本轮 runfarm 已及时回收：
    - 实例 `i-0be31b2a81d36b2e4`
    - AWS 状态已进入 `shutting-down`
- 为消除“phase marker 本身走 stdout 可能成为新盲点”的歧义，本轮又追加了一次更窄的诊断补丁：
  - `include/gemmini.h`
    - `PRT_GEMMINI_PHASE_LOG(...)` 改为统一走 `stderr`
  - `include/gemmini_nn.h`
    - 新增 `[gemmini-phase] matmul-nn-stride-auto-enter ...`
      用于区分：
      - 还没进 `tiled_matmul_nn_stride_auto(...)`
      - 已进 `tiled_matmul_nn_stride_auto(...)` 但还没到 `tiled_matmul_auto(...)`
- 这版新的 guest 侧工件也已重新刷新完毕：
  - `HOST_INIT_CHECK_ONLY=1 bash host-init.sh` PASS
  - 完整 `bash host-init.sh` PASS
  - rebuilt binary 已确认包含：
    - `[gemmini-phase] matmul-nn-stride-auto-enter`
    - `[gemmini-phase] matmul-auto-enter`
    - `[prt-marker]`
  - `marshal build` PASS
  - `marshal install` PASS
- 当前 FireSim replay 的唯一阻塞已转为 AWS 容量：
  - `launchrunfarm` 会话 `firesim-bertmini-prt-launch-7` 正在 on-demand `f2.6xlarge` 上重试
  - 当前现象是 `insufficient capacity`
  - 这不是新的代码回归信号
- 2026-03-25 live FireSim replay 结果：
  - `marshal build` PASS
  - `marshal install` PASS
  - `launchrunfarm` PASS
  - `infrasetup` PASS
  - Linux guest 成功越过 early boot、挂载 rootfs、进入 `S99run`
  - guest 侧已打印：
    - `[bertmini] hugetlb before ...`
    - `[bertmini] hugetlb after ...`
    - `[bertmini] method=ours2 cores=2 gemmini=2 dma=2`
  - 之后进入新的 silent window：
    - `uartlog` 停在 `method=ours2`
    - `heartbeat.csv` 仍持续推进到约 `10.65B+` cycles
  - 这说明当前低噪声重编并没有直接修复 active guest/runtime hang
  - 但也说明问题边界已进一步收窄：
    - 不在 Linux boot
    - 不在 FireMarshal packaging

## 2026-03-25 本轮追加更新

- 新 replay 结果目录：
  [2026-03-25--04-53-51-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime](/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-25--04-53-51-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime)
- 本轮追加手工抓取：
  - [uartlog.live](/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-25--04-53-51-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime/manual-capture/uartlog.live)
  - [heartbeat.live.csv](/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-25--04-53-51-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime/manual-capture/heartbeat.live.csv)
- 这轮新证据把 active stall boundary 再往里推进了一层：
  - 已看到：
    - `pointwise-chunk ... subcall-begin`
    - `pointwise-inner ... matmul-begin I=256 J=64 K=256 fallback=OS`
  - 仍未看到：
    - `pointwise-inner ... matmul-end`
    - `subcall-return`
    - `chunk-drain`
    - `chunk-end`
    - `gemm-exit`
- 在只停留于 `matmul-begin` 的同时，`heartbeat.csv` 继续从约 `8.70B` 推进到 `9.49B` target cycles。
- 因此当前最强判断已经更新为：
  - 活跃 stall 不再是 outer chunk wrapper
  - 而是在或紧跟于 `tiled_matmul_nn_stride_auto(...)` / 更深一层 Gemmini matmul phase
- 本轮 runfarm 已及时回收：
  - 实例 `i-01f156745c8842c3d`
  - AWS 状态已进入 `shutting-down`
- 同时，本地已完成一轮带新增 action-level marker 的 overlay 重编：
  - `host-init overlay stage PASS`
  - 当前下一轮诊断将不再加外层 runtime 噪声，而是改为启用更深一层的 `PIPELINE_RUNTIME_GEMMINI_PHASE=1`
    - 不在 FireSim infra bring-up
    - 在 bertmini `ours2` method 已经开始之后
- 2026-03-25 第二轮 refined replay 结果：
  - results:
    [2026-03-25--04-16-38-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime](/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-25--04-16-38-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime)
  - manual capture:
    [uartlog.live](/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-25--04-16-38-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime/manual-capture/uartlog.live)
    [heartbeat.live.csv](/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-25--04-16-38-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime/manual-capture/heartbeat.live.csv)
  - guest 成功越过：
    - `S99run`
    - `[bertmini] hugetlb before/after`
    - `runtime-init`
    - artifact validation
    - `segment=0`
  - guest 最后稳定可见边界是：
    - `worker stage=0 subbatch=0 gemm-enter`
    - `conv-sync ... dispatch=pointwise`
    - `pointwise ... chunked`
    - `pointwise-chunk ... begin`
    - `pointwise-chunk ... chunk-begin oc_beg=0 oc_tile=64`
  - 在这之后额外等待超过 4 分钟：
    - `heartbeat.csv` 从约 `9.09B` 增长到约 `11.83B` cycles
    - 仍未出现：
      - `chunk-drain`
      - `chunk-end`
      - `gemm-exit`
      - `BERTMINI_PIPELINE_RUNTIME_PASS`
  - 这说明：
    - 去掉 chunked helper 热点里的普通 progress log 后，活跃边界没有再前移
    - 当前 stall 已不再像是被原来的 chunked 普通日志直接卡住
    - 当前最值得怀疑的是首个 subchunk 真正的 matmul 调用前后，而不是 Linux bring-up 或 artifact/setup
- 为避免继续计费，本轮 run farm 已主动回收：
  - instance: `i-077f10bc12b778b92`
  - state after terminate request: `shutting-down`

## 当前活跃主线

- 当前最值得继续推进的是 Linux packaging / FireMarshal / FireSim F2 主线：
  - 保持 no CPU fallback。
  - 保持 no hardware change。
  - 保持 shared-spad `all-bank / 1KB interleaved / multi-manager` 设计目标。

### 当前主怀疑

- host runtime 已经证明当前 page placement / manager contract 至少在 host 路径上自洽。
- 因而下一轮若 guest/F2 失败，优先怀疑：
  - `host-init.sh` staging 与 guest wrapper 参数不一致
  - guest userspace HugeTLB / pagemap / DMA pinning 路径
  - FireMarshal overlay / FireSim workload 配置
  - 只在 guest 证据直接指向 runtime 语义错误时，再回看 runtime 内部地址空间合同

### 下一步工作

1. Linux packaging gate
   - 先做 `HOST_INIT_CHECK_ONLY=1 bash host-init.sh`
   - 再做完整 `bash host-init.sh`
   - 确认 overlay 中的 runtime binary 与 canonical artifacts 一致
2. FireMarshal build/install gate
   - 用当前 fresh artifacts 重建 workload image
   - 确认 guest 侧看到的是新 contract 下的 artifacts
3. FireSim F2 replay gate
   - 走 on-demand `f2.6xlarge`
   - 用 tmux wrapper 执行 `launchrunfarm -> infrasetup -> runworkload -> terminaterunfarm`
4. 若 guest/F2 失败
   - 优先看 `uartlog / heartbeat.csv / tmux pane log`
   - 先定位 guest-specific blocker，再决定是否需要回到 runtime 代码
5. 任何触及 shared-spad / resadd / pointwise 语义的修复
   - 都必须回归 baremetal gate 与 host closure gate

## 2026-03-25 Baremetal Init Probe Update

- baremetal replay:
  [2026-03-25--11-49-09-rerocc-lc-baremetal-coupleddma-explicit-interleaved-small-f2-rerocc-baremetal-explicit-interleaved-small](/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-25--11-49-09-rerocc-lc-baremetal-coupleddma-explicit-interleaved-small-f2-rerocc-baremetal-explicit-interleaved-small)
- this replay moved the observable boundary from:
  - `banner only`
- to:
  - `thread_entry_after_banner`
  - `before_init_resadd_a`
  - `after_init_resadd_a`
  - `before_init_resadd_b`
  - `after_init_resadd_b`
  - `before_resadd_reference`
  - `after_resadd_reference`
  - `before_init_pw_a`
- updated baremetal conclusion:
  - the guest is alive and advancing through CPU-side fixture generation before the first `CASE_START`
  - the old silent window was not enough evidence of a true hang
- second conclusion from the same replay:
  - dense per-quarter-step logging inside tiny loops severely inflates target cycles
  - the next replay should keep phase markers and cycle deltas, but use sparse progress only for the large pointwise reference path

## 2026-03-25 Baremetal Pointwise Probe Update

- sparse-logging replay:
  [2026-03-25--12-03-47-rerocc-lc-baremetal-coupleddma-explicit-interleaved-small-f2-rerocc-baremetal-explicit-interleaved-small](/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-25--12-03-47-rerocc-lc-baremetal-coupleddma-explicit-interleaved-small-f2-rerocc-baremetal-explicit-interleaved-small)
- this replay showed:
  - `pointwise_matmul_reference()` for `I=256, J=64, K=256` starts
  - it reaches sparse checkpoints `row=0/63/127/191/255`
  - it completes with `delta=1220901896` target cycles
  - guest then continues into `pointwise_matmul_reference_dim_j(..., 128)`
- strongest current baremetal conclusion:
  - the long pre-`CASE_START` window is dominated by CPU golden/reference generation
  - it is not enough, by itself, to diagnose a dead FireSim guest
  - the old “wait ~2B cycles then kill” heuristic is too aggressive for this workload
- operational decision:
  - terminate the runfarm after capturing this boundary, to avoid paying for the remaining CPU-only golden path

## 2026-03-25 Baremetal Stall-Triage Mode Update

- new code path now in tree:
  - [`rerocc_lc_resadd_explicit_interleaved.c`](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/bareMetalC/learn-gemmini/rerocc_lc_resadd_explicit_interleaved.c)
    supports `REROCC_STALL_DIAG_ONLY=1`
- this mode is specifically for stall localization, not numeric closure:
  - skip pointwise/diag golden precompute
  - skip numeric compare
  - prioritize `bias_first_mvin_*`
- build plumbing also updated:
  - [`host-init-explicit-interleaved.sh`](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/rerocc-baremetal-tests-coupleddma/workload/host-init-explicit-interleaved.sh)
  - [`firemarshal-tmux-run.sh`](/home/ubuntu/chipyard/scripts/firemarshal-tmux-run.sh)
- verification already completed:
  - default local baremetal compile: PASS
  - `REROCC_STALL_DIAG_ONLY=1` local baremetal compile: PASS
  - FireMarshal build/install with `REROCC_STALL_DIAG_ONLY=1`: PASS
- updated operational rule:
  - future baremetal F2 runs whose purpose is stall triage should start from this mode by default

## 2026-03-25 Baremetal Stall-Triage Live Run Update

- current live F2 baremetal replay:
  [2026-03-25--12-28-35-rerocc-lc-baremetal-coupleddma-explicit-interleaved-small-f2-rerocc-baremetal-explicit-interleaved-small](/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-25--12-28-35-rerocc-lc-baremetal-coupleddma-explicit-interleaved-small-f2-rerocc-baremetal-explicit-interleaved-small)
- runtime status at capture time:
  - runworkload still active
  - host `192.168.1.38`
  - `heartbeat.csv` already advanced to about `2.276B` target cycles
- latest confirmed UART boundary:
  - `STALL_DIAG mode=1`
  - `before_init_resadd_a`
  - `after_init_resadd_a`
  - `before_init_resadd_b`
  - `after_init_resadd_b`
  - `before_resadd_reference`
  - `after_resadd_reference`
  - `before_init_pw_a`
  - `after_init_pw_a`
  - `before_init_pw_b`
  - `after_init_pw_b`
  - `before_init_pw_bias`
  - `after_init_pw_bias`
  - `before_init_pw_chunk_bias`
  - `after_init_pw_chunk_bias`
  - `before_memset_pw_init_c`
  - `after_memset_pw_init_c`
  - `stall_diag_skip_pointwise_goldens`
  - `before_diag_memset_ab`
  - `after_diag_memset_ab`
- current conclusion:
  - stall-only mode is working as intended and has already skipped the large pointwise/diag golden precompute
  - however, even this reduced path still contains non-trivial fixture initialization before the first `bias_first_mvin_*` case
  - therefore, at this point there is still no evidence that the guest is dead or that the first-bias `mvin` boundary has been reached

## 2026-03-25 Baremetal Stall-Triage F2 Outcome

- final replay:
  [2026-03-25--12-28-35-rerocc-lc-baremetal-coupleddma-explicit-interleaved-small-f2-rerocc-baremetal-explicit-interleaved-small](/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-25--12-28-35-rerocc-lc-baremetal-coupleddma-explicit-interleaved-small-f2-rerocc-baremetal-explicit-interleaved-small)
- decisive UART evidence from this run:
  - `CASE_START bias_first_mvin_linux_tile_cross_1kb_contiguous`
  - `[graw] bm0ce`
  - `bias-mvin0-call-rs1`
  - `bias-mvin0-call-rs2`
  - `[graw] bm0ca`
  - `[graw] bm0cb`
  - `CASE_RESULT bias_first_mvin_linux_tile_cross_1kb_contiguous PASS`
  - `CASE_START bias_first_mvin_linux_tile_cross_1kb_interleaved`
  - `[graw] bm0ce`
  - `bias-mvin0-call-rs1`
  - `bias-mvin0-call-rs2`
  - `[graw] bm0ca`
  - `[graw] bm0cb`
  - `CASE_RESULT bias_first_mvin_linux_tile_cross_1kb_interleaved PASS`
  - `ALL_TESTS_PASS`
- operational follow-up:
  - runfarm has been reclaimed with `terminaterunfarm --forceterminate`
  - target instance `i-08ea62a5b822d6efd` has entered `shutting-down`
- strongest updated conclusion:
  - the Linux/F2 pipeline-runtime stall previously seen near the first bias `mvin(D -> acc)` is not reproduced by the corresponding baremetal/F2 minimal replay
  - therefore the current prime suspect is no longer the raw Gemmini bias-`mvin` instruction sequence itself
  - the active search surface should move to Linux/pipeline-runtime-specific preconditions and state preparation
- narrowed next-debug surface:
  - segment virtual-region construction and per-buffer VA layout
  - per-core shared-spad xlate base programming
  - runtime-installed PTE contents and lifetime
  - runtime-only buffer semantics such as shared/single/double/ring buffer aliasing
  - Linux-side orchestration before the first pointwise issue path

## 2026-03-25 Runtime-Specific Diff Update

- confirmed contract mismatch:
  - docs / `NEXT_SESSION_PROMPT.md` / MudnacSim all describe fixed-order `all-bank` page interleave
  - runtime implementation in `pipeline-runtime/src/prt_page_table.c`
    - `alloc_pages_from_order(...)`
    - previously rotated the bank cursor once per local page layer
    - for 2 banks this yielded a sequence like:
      - `0, 1024, 1025, 1, 2, 1026, ...`
    - this is not the same contract as fixed interleave:
      - `0, 1024, 1, 1025, 2, 1026, ...`
- code landed in this round:
  - `alloc_pages_from_order(...)` now keeps stable bank order for every `local_page_idx`
  - action allocation now emits marker-level page logs:
    - `alloc-pages`
    - includes `buffer/tensor/stage/slot`
    - plus per-page `ppn / acc_id / local_page_idx / paddr`
  - stage exec-view install now emits marker-level bind logs:
    - `exec-bind`
    - includes `action/segment/stage/slot/tensor`
    - plus per-page `vpage / ppn / acc_id / local_page_idx / pte / paddr`
  - detailed `exec-bind` page dump is currently focused on:
    - `segment 0`
    - `stage 0`
    - to keep noise bounded while still capturing the current stall boundary
- local verification completed:
  - host default build:
    - `make -C generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime -j4`
  - host marker build:
    - `make -C generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime PIPELINE_RUNTIME_ONLY_MARKER=1 -j4`
- updated working conclusion:
  - the runtime now matches the documented fixed `all-bank` allocation contract more closely
  - the next Linux/F2 replay should no longer rely on inferred allocator behavior
  - if the stall persists, the next decisive evidence should be the exact `alloc-pages` and `exec-bind` dump around `segment 0 / stage 0`

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
