# Lessons Learned

日期：2026-03-26

本文件只记录已经被验证过、后续不应反复踩的经验教训。

## 0.6 `cfg` 不是 “可同时 issue 的 Gemmini lane” 数量；当前单 hart 只有一条 Gemmini live 路由

- ReRoCC client 当前确实有：
  - `16` 个 `cfg`
  - `4` 个 `rropc`
- 但当前软件栈里：
  - Gemmini 指令固定走 `custom3`
  - DMA 指令固定走 `custom2`
- 所以当前真实语义是：
  - 单 hart 可以按时间复用去管理很多 manager
  - 但单 hart 当前只有 `1` 条 Gemmini live route lane
  - 不是“因为有 4 个 opcode，所以自动能并发 4 个 Gemmini core”

工程准则：

- 后续讨论并发能力时，先分清三件事：
  - acquire 了多少 manager
  - 持有了多少 cfg context
  - 不重绑时到底有多少 live issue lane
- 不要再把这三者混成一个“核心数上限”。

## 0.7 `rr_set_opc()` 可以提前重绑；`rr_release()` 不会顺带清 opcode 槽

- 当前 ReRoCC 路径里，已发出的请求会带着自己的：
  - `cfg/client_id`
  - `manager_id`
- 因此后续执行 `rr_set_opc()` 去重绑 opcode，不会把已发出的请求重定向到新 manager。
- 但 `rr_release(cfg)` 只释放 `cfg`，不会自动清空 `rropc` 映射。

工程准则：

- 如果旧 lane 上已经不再需要继续 issue/fence/fault 操作，那么可以在远端执行结束前重绑。
- 如果后续还要继续对旧 manager 走同一条 lane 发命令，就不能想当然提前重绑。
- 之后设计“多个 action 同时 active”的路由策略时，要把 `cfg` 生命周期和 `opcode` 生命周期分开设计。

## 0.8 当前 `cfg = ((stage_id * 2) + lane) % 16` 的散列，只能提供单 hart `8` 组无冲突 stage-context

- 目前 runtime 里：
  - `lane=0` 给 DMA
  - `lane=1` 给 Gemmini
  - `cfg = ((stage_id * 2) + lane) % 16`
- 所以单 hart 若想同时保留很多 stage 的 Gemmini/DMA context，
  超过 `8` 个 stage 后就会出现 `cfg` alias。

工程准则：

- 之后不要再把当前这套 `stage_id` 取模散列误当成最终可扩展方案。
- 若未来要支持更多 active stage/action，先做显式 allocator，再谈更大负载。

## 0.3 `spaddrs-dc` 不是最终边界；必须继续把断点钻进 `config_ld` 调用前后

- 之前 Linux/F2 live 只能看到：
  - `matmul-os spaddrs-dc`
  - 后面半截 `"[ge"`
- 这一轮把长日志替换成短 `write()` 型 `gcrit` 断点后，边界进一步被验证成：
  - 已出现：
    - `matmul-os-biascfg-enter`
    - `matmul-os-biascfg-pre-ld`
  - 未出现：
    - `matmul-os-biascfg-post-ld`
- 这说明：
  - 旧结论“可能卡在 spaddrs-dc 到 bias-config 之间”已经不够精确
  - 当前更准确的表述应是：
    - 卡点已经压缩到 bias `config_ld` 调用内部/之后

工程准则：

- 后续如果一条长 `fprintf` 本身可能误导边界：
  - 优先改成短 `write()` 型 marker
  - 并在关键 marker 后追加几条固定垃圾行，增加落盘概率
- 对关键函数调用要成对放：
  - `pre-call`
  - `post-call`
  这样才能把边界直接压缩到单个调用

## 0.4 OS bias 路径不能再把 `config_ld/mvin0` 当成默认正确实现

- 同一份 [gemmini.h](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/include/gemmini.h) 里，参考实现已经明确：
  - bias/`D` 配置使用 `gemmini_extended3_config_ld(..., low_D, 2)`
  - bias move-in 使用 `gemmini_extended_mvin3(...)`
  - bias stride 取决于 `sizeof_D = low_D ? sizeof(elem_t) : sizeof(acc_t)`
- 但旧的 OS bias 路径却实际写成了：
  - `gemmini_extended_config_ld(D_stride, D_scale_factor)`，默认 `id=0`
  - `bias-mvin0`
  - `gemmini_extended_mvin(...)`
  - `D_stride` 固定 `sizeof(acc_t)`
- 最新 Linux/F2 replay 恰好就卡在这条旧路径的 `pre-ld` 与 `post-ld` 之间。

工程准则：

- 之后碰到 bias / preload / `D` 路径时，不要再凭“能跑起来”假设 `mvin0` 就对。
- 先对照参考实现确认三件事是否同时一致：
  - `config_ld` 的 `id`
  - 实际使用的 `mvin` 通道
  - `low_D / sizeof_D / stride_D`

## 0.5 `mvin3/id=2` 有文档和模型支撑，但单独切到这条路径并没有消除当前 Linux/F2 stall

- 本轮已经做过一次“按参考实现对齐”的修补：
  - bias `config_ld` 改为 `id=2`
  - bias move-in 改为 `mvin3`
  - `D_stride` 改为按 `sizeof_D/low_D`
- 随后的 fresh Linux/F2 replay 仍然稳定卡在：
  - `matmul-os-biascfg-pre-ld`
  - heartbeat 持续增长
  - `post-ld` 始终不出现
- 同时文档和模型层已经确认：
  - Gemmini 的确存在三条 `mvin`
  - `config_mvin` 的 `state_id=2` 就是 `mvin3`

工程准则：

- 后续不要再把“换成 `mvin3/id=2`”当作当前 stall 的充分修复条件。
- 这条 diff 只能说明：
  - 旧实现与参考实现不一致，值得修正
  - 但当前真实根因仍在更深一层
- 下一步应优先排查：
  - `config_mvin` 在当前 ReRoCC/shared-spad/xlate 上下文中的阻塞条件
  - 而不是继续在 `mvin0`/`mvin3` 命名层面反复切换

## 0.1 activation contract 仍未导出，但当前调试主线必须以用户指定策略为准

- 静态 diff 依然成立：
  - 当前 `model.layers.yaml / pipeline_mapping.* / gemmini_layer_mapping.*` 仍没有 fused activation 字段。
  - 这意味着 artifact contract 依旧没有把 activation 显式导出。
- 但本轮之后，当前有效执行策略已经变成：
  - 先按用户要求把 `pipeline-runtime` 中相关 activation 临时统一视为 `RELU`
  - 因此此前把 `NO_ACTIVATION` 当成当前 Linux 主线语义的文档结论，已经不再适用
- 这两件事要同时记住：
  - 从 contract 设计角度看，runtime 仍然不应长期私自发明 activation
  - 但在这轮调试期里，Linux/F2 与 host closure 的最新结果都应按 `RELU` 语义理解

后续约束：

- 短期：
  - 继续按当前用户指定策略维护 `RELU`
  - 不要再用旧的 `NO_ACTIVATION` 现场去解释新的 Linux/F2 replay
- 长期：
  - 仍应把 activation 明确加入 artifact contract
  - 然后把当前这个临时 `RELU` override 删掉，改成按 artifact 恢复

## 0.2 live stall 若停在半截行，必须把“半截字节”本身当作最深边界，而不是只看上一条完整行

- 本轮 Linux/F2 最新 replay 的最深证据不是：
  - “最后一条完整行是 `matmul-os spaddrs-dc`”
- 而是：
  - `uartlog` 尾部稳定停在
    - `... matmul-os spaddrs-dc ...`
    - 后面还有半截 `"[ge"`
- 同时：
  - `heartbeat` 持续推进
  - `uartlog` 大小长时间不再增长
- 这说明只看 `splitlines()` 或普通 `tail` 还不够；
  必须连最后几百个原始字节一起看，否则会把 live boundary 粗化到上一条完整日志。

工程准则：

- 后续 Linux/F2 stall triage，除了：
  - Python `read_bytes + decode + splitlines()`
- 还要补一条：
  - 直接记录 `repr(data[-N:])`
- 若尾部存在半截 marker：
  - active boundary 应表述为
    “上一条完整 marker 之后，下一条 marker 的写出过程中”
  - 不要过早把它简化成“卡在上一条完整 marker”

## 0. baremetal 最小链接环境里不能直接靠 `fflush(stdout)` 解决日志缓冲

- 本轮尝试把 `PRT_GEMMINI_RAW_LINE` / `PRT_GEMMINI_PHASE_LOG` 在 baremetal 下改成 `printf + fflush(stdout)`。
- 结果直接导致裸机链接失败：
  - `undefined reference to '_impure_ptr'`
  - `undefined reference to 'fflush'`

这说明：

- 当前 baremetal payload 的最小 C 运行时并没有把 `fflush` 这套 newlib 依赖完整带进来。
- 之后如果需要更可靠的日志，可行方向应优先是：
  - 增加更细粒度、更多行的显式日志
  - 必要时改成已有 baremetal-safe 的输出原语
- 不要再在这个最小 baremetal 路径里直接塞 `fflush(stdout)`，否则会先把编译链打坏。

## 1. FireSim live `uartlog` 不能再用 `wc -l` 或普通 `tail` 判断真实卡点

- `uartlog` 里可能混有：
  - `\r`
  - `\0`
  - 半截长行
- 直接用：
  - `wc -l`
  - `tail -n`
  很容易把 live boundary 误判成停在更早的位置。

本轮直接证据：

- 普通 `tail` 一度看起来像是停在 `software IO TLB`
- 但改为：
  - `Path(...).read_bytes().decode('latin1', errors='ignore').splitlines()`
  之后，马上看到 guest 其实已经越过：
  - `/init`
  - `EXT4 rootfs mount`
  - `S99run`
  - `runtime init-step`
  - `segment=0 begin`

工程准则：

- 后续所有 live FireSim UART 监控统一改成：
  - Python `read_bytes + decode('latin1') + splitlines()`
- 只有在 Python 视图与 heartbeat 一起停滞时，才把它升级为真实 hang。

## 2. Gemmini accumulator 地址语义不能想当然

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

## 3. `gemmini_fence()` 不是 ReRoCC manager 可见的 completion barrier

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

## 4. shared-spad `1KB` interleaved 页表设计本身没有被证明有问题

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

## 4b. 终止 runfarm 前若要保 live 证据，必须先抓日志

- 一旦 `terminaterunfarm --forceterminate` 发出，F2 host 只剩很短窗口可抓：
  - `/home/ubuntu/sim_slot_0/uartlog`
  - `/home/ubuntu/sim_slot_0/heartbeat.csv`
- 本轮就是先 terminate，再尝试 `scp`，结果窗口已经不稳定，未能把这轮 live 文件完整抄回本地。

工程准则：

- 如果当前 run 的 live boundary 是重要证据：
  - 先抓 `uartlog/heartbeat.csv`
  - 再 terminate
- 若已决定 terminate，也要给抓取命令显式加 `timeout`，避免挂住 shell。

## 5. shared-spad xlate 的软件 contract 必须显式收尾

- xlate table 不会自己恢复默认状态。
- `cfg` / `range` / `flush` / `reset` 都要显式做。
- case-local mapping 之间不能默认“天然隔离”。

直接后果：

- pointwise `J=128` 主线的真实根因不是硬件，而是：
  chunk-bias VA 覆盖进了 `C` region 的 VA 范围，后续 `C` 的 PTE 安装把 bias PTE 覆盖掉了。

工程准则：

- 每个 case 单独验证 installed xlate set 的 VA overlap。
- 不要只看“全局 region 命名上不冲突”；要看“当前实际安装集合是否冲突”。

## 6. `MAX_BLOCK_LEN` 不是“整层 J 的合法性上限”

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

## 7. 显式 overlap 序列必须写成完整依赖链

已验证通过的经验是：

- 若手写 explicit resadd，需要把依赖链写完整：
  `A mvin -> rr_fence -> B mvin2 -> rr_fence -> mvout`

只补其中一半不够：

- 只补 `B -> mvout` 不够。
- `A -> B` 同样要保证 manager-visible completion。

工程准则：

- 写 explicit overlap 时，先把数据依赖图画清楚。
- 每一条真正依赖链都要有对应的 manager-visible completion。

## 8. standard WS path 优先级高于手写 workaround

baremetal 已证明：

- standard WS resadd 在当前 interleaved shared-spad 设计下是可用的。
- dual-manager split 也已有正向证据。

因此：

- 如果 standard path 已经满足语义，应优先使用 standard path。
- 手写 explicit workaround 只能在必要时存在，而且必须严格遵守本文件的语义约束。

## 9. FireSim 运行流程的工程教训

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

## 10. `spot` 失败不等于配置错误

当前已验证：

- `f2.6xlarge spot` 在当前 AWS 容量下持续报 `insufficient capacity`。

因此：

- 当前 `spot` 不可用的结论已经成立。
- 这不是 workload / AGFI / FireSim config 本身的错误。

工程准则：

- `spot` 如果持续是 capacity fail，就先切回 on-demand 完成 correctness closure。
- 不要在 capacity fail 上浪费调试时间。

## 11. baremetal runtime-style deepest replay 已证明：当前 stall 不能再默认归因于 Gemmini OS 内核本身

- 本轮通过 baremetal `REROCC_RUNTIME_STYLE_ONLY=1` + F2 deepest logging，已经把
  `pointwise_matmul_os_chunked_128_to_64_runtime_style_cross_1kb_contiguous`
  整条路径钻穿，并拿到：
  - `CASE_RESULT ... PASS_STALL_DIAG`
- 已确认完整越过的阶段包括：
  - `matmul-config pre/post ex/st/ld`
  - `matmul-os bias-config`
  - `bias-mvin0`
  - `B/A mvin`
  - `preload`
  - `compute-preloaded`
  - `compute-accumulated`
  - `mvout`
  - `inner end`
  - `outer fence`
  - `chunk_issue_end`
  - `chunk_drain`

工程准则：

- 之后遇到 Linux/pipeline-runtime stall，不能再先假设：
  - “Gemmini OS pointwise 内核本身会卡”
  - “首个 bias mvin 在 baremetal 上也会卡”
- 优先怀疑：
  - runtime-specific state
  - chunk/case 之间的 contract
  - action/runtime 上下文差异

## 12. 后续调试时，默认优先级

出现新问题时，优先级应是：

1. 先查软件 contract 是否自洽：
   - 地址语义
   - manager completion
   - page placement
   - PTE 安装顺序
2. 再查 runtime 是否误用了 Gemmini ISA。
3. 最后才考虑硬件问题。

## 11. `SPM_XLATE_RANGE` 的窗口基址决定了 PTE 必须从窗口内 `vpage 0` 开始

本轮静态对照：

- baremetal 正例里的 `spm_xlate_map_page(...)`
- `rerocc_linux_spm_xlate.h`
- runtime 里的 `prt_spm_translate_range(...)`

已经共同说明：

- PTE 索引统一按：
  `vpage = (vaddr - range_base) >> page_shift`
  计算。
- 当前硬件接口只有：
  - `CFG(ptbr, pte_count, page_shift, enable)`
  - `RANGE(range_base, range_size)`
- 现有硬件语义里没有额外的“PTE index base / alias_vpage_start”。

直接后果：

- 如果软件给某个 action 申请了新的独立 alias VA window，
  那么这个 window 里的有效映射必须写进该 window 对应的 PTE `0..N-1`。
- 把它们写到全局表里的某个偏移槽位，例如：
  `alias_vpage_start + exec_base_vpage + local_first_vpage`
  在当前硬件上是不成立的。

工程准则：

- 单 active action 模式下：
  - action-local alias window 可以复用 PTE `0..N-1`
  - 不要再引入 software-only 的 `alias_vpage_start`
- 若未来要支持多 action 并发：
  - 不能只靠 `range_base` 换窗口、再在同一份全局 PTE 里切子区间
  - 必须给每个 action / manager 单独 PTBR，或者给硬件增加可见的 PTE index base

## 12. 看不到 `gemmini-phase` 日志前，先确认这轮 binary 是否真的编进去了

本轮 replay 使用的 Linux workload 默认是：

- `PIPELINE_RUNTIME_ONLY_MARKER=1`
- `PIPELINE_RUNTIME_GEMMINI_PHASE=0`

这意味着：

- `matmul-call-enter` 这类 marker 可以出现
- 但 `matmul-nn-stride-auto-enter`、`bias-mvin0-*` 这类 `gemmini-phase` 日志本来就不会出现

工程准则：

- 以后用“某类日志没出现”来划边界前，先确认：
  - 这轮 binary 的编译开关
  - `host-init.sh` 是否允许该类字符串进入 guest binary

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

## 12. baremetal 卡死排查时，不能沿用 full regression 的 golden 前置策略

这次重新对照已有 baremetal 正例后，已经确认：

- 常见排查用例通常是：
  - 只生成当前 case 需要的那一份 golden，然后立刻进入 issue path
  - 或者先 warmup/cache 一次 fixture，后续复用
- 当前 `rerocc_lc_resadd_explicit_interleaved.c` 之前则是：
  - 在第一个 `CASE_START` 之前
  - 一次性前置计算多份 pointwise/diag golden
  - 包括：
    - `pw_gold_dram`
    - `pw_j128_gold_dram`
    - `pw_chunk_gold_dram`
    - `pw_diag_gold_dram`

直接后果：

- 这个 workload 更像 full regression gate，而不像快速 stall triage gate。
- 如果排查目标是“卡死边界”，那么大段 guest CPU golden 生成本身就会遮住真正要看的 Gemmini issue path。

工程准则：

- 排查卡死时，优先使用：
  - lazy golden
  - cached fixture
  - 或 stall-only 模式
- 只有在排查数值错误时，才恢复 full regression 的全量 golden 预计算。

## 13. baremetal 上“长时间没有 `CASE_START`”本身不能当作卡死证据

本轮 sparse-logging replay 已经验证：

- guest 在第一个 `CASE_START` 之前，会先走完整的 CPU-side fixture generation。
- 单个：
  - `pointwise_matmul_reference(I=256, J=64, K=256)`
  - 就消耗了约 `1.22B` target cycles。
- 后面还会继续进入：
  - `pointwise_matmul_reference_dim_j(..., 128)`
  - `pointwise_matmul_chunked_reference()`

直接后果：

- “约 `2B` cycles 还没看到 `CASE_START` 就判卡死”的经验，在这个 workload 上不成立。
- 如果只看 `CASE_START` 是否出现，很容易把“长 CPU preprocessing”误判成“Gemmini/FireSim 卡死”。

工程准则：

- stall triage 时，优先看：
  - `INIT_PHASE`
  - `INIT_FN_START/END`
  - `heartbeat.csv`
- 只有在：
  - phase 日志长期无前进
  - 且 heartbeat 也停滞
  - 或已经拿到更深的决定性边界
  才结束 runfarm。

## 14. 调试日志本身也可能成为 target-cycle 热点

这次已经验证：

- 在小循环里加入过密的 `printf`
  - 会显著放大 baremetal/F2 target cycles
  - 甚至把本来很短的 init/reference 阶段拖成数百 M cycles

工程准则：

- 对小循环：
  - 用 `start/end + delta`
- 对真正的大循环：
  - 用稀疏 checkpoint
- 若主要目标只是定位 stall，不要默认打印每个 quarter-step。

## 15. baremetal stall triage 需要专用运行模式

当前已经落地的做法是：

- 为 `rerocc_lc_resadd_explicit_interleaved.c` 增加：
  - `REROCC_STALL_DIAG_ONLY=1`
- 该模式下：
  - 跳过前置 pointwise/diag golden 预计算
  - 跳过数值 compare
  - 优先直达最相关的 `bias_first_mvin_*` case

配套约束：

- `host-init-explicit-interleaved.sh` 已支持把该宏透传进 baremetal build
- `firemarshal-tmux-run.sh` 已支持透传 `REROCC_STALL_DIAG_ONLY`

工程准则：

- 以后凡是“主要目标是定位 stall，不是定位数值错”的 baremetal replay：
  - 默认先用 `REROCC_STALL_DIAG_ONLY=1`
- 只有在 stall 边界已经收敛后，才切回 full regression 模式检查数值。

## 16. 即使进入 stall-only 模式，也不能把“尚未看到 CASE_START”直接当成卡死

本轮 live F2 baremetal replay 已进一步说明：

- 即使已经启用 `REROCC_STALL_DIAG_ONLY=1`
  - guest 仍然要完成一段必要的 fixture/init 路径
  - 包括 `resadd` 参考计算、pointwise 输入/bias 初始化、以及若干 `memset`
- 在这些 phase 全都完成之前
  - 仍可能长时间看不到 `bias_first_mvin_*` 的 `CASE_START`
  - 但 heartbeat 会继续稳定推进

工程准则：

- stall-only 模式下也要同时看：
  - `INIT_PHASE`
  - `INIT_FN_START/END`
  - `heartbeat.csv`
- 只有在这些 init phase 停止推进，且 heartbeat 也不再增长时，才把它升级为真实 hang 候选。

## 17. baremetal/F2 若能穿过首个 bias mvin，全局怀疑面就必须收缩到 runtime-specific 差异

这轮 F2 baremetal stall-only replay 已确认：

- `bias_first_mvin_linux_tile_cross_1kb_contiguous`
  - 完整穿过了
    - `bm0ce`
    - `bias-mvin0-call-rs1`
    - `bias-mvin0-call-rs2`
    - `bm0ca`
    - `bm0cb`
  - 并 `PASS`
- `bias_first_mvin_linux_tile_cross_1kb_interleaved`
  - 也完整穿过同一条首个 bias `mvin(D -> acc)` 路径
  - 并 `PASS`

因此这次可以排除的，不是“所有问题”，而是更窄的一层：

- 不能再把主嫌疑继续放在：
  - 裸的 Gemmini bias `mvin` 指令本身
  - 这条 baremetal case 所覆盖到的 all-bank 物理页分配方式
  - 这条 baremetal case 所覆盖到的 1KB 页跨界 contiguous/interleaved bias 访问模式

工程准则：

- 当 Linux/pipeline-runtime stall，但 baremetal/F2 同地址形态的最小复现已经通过时：
  - 下一轮优先排查 runtime-only 差异
  - 不要继续围绕“这条 Gemmini 指令是不是天然会卡”重复做低收益 replay
- 该类 runtime-only 差异优先级应按下列顺序展开：
  - segment VA 区间申请与子图私有基址传递
  - shared-spad xlate 区间编程
  - PTE 安装内容与生存期
  - buffer alias/shared/single/double/ring 语义
  - Linux orchestration 中 issue 前的额外状态修改

## 18. `all-bank interleave` 必须区分“固定交错”和“旋转交错”

本轮 runtime-specific diff 已确认：

- 文档、MudnacSim、以及当前 HybridMapper/runtime contract 所说的 `all-bank`
  - 指的是固定 bank 顺序的页交错
  - 即每个 `local_page_idx` 都按同一 bank 顺序展开
- `pipeline-runtime` 旧实现里
  - `alloc_pages_from_order(...)`
  - 每推进一层 `local_page_idx` 就轮转一次 bank 起点
  - 这会生成不同 contract 的物理页顺序

工程准则：

- 以后看到“all-bank / interleaved”这类术语时，不要停留在字面相似
- 必须明确：
  - bank 顺序是否固定
  - 是否每层 `local_page_idx` 都复用同一顺序
  - 是否引入 round-robin cursor rotation
- 若需要对照参考实现：
  - 优先看 MudnacSim `allocateFixedBankInterlace(...)`
  - 不要只根据本地 allocator 名字推断语义一致

## 19. Linux/F2 深挖前，先把 page-level 绑定日志做成 marker-only 并本地编过

这轮的有效做法是：

- 在 action 分配侧打印：
  - `alloc-pages`
  - 输出 `buffer/tensor/stage/slot`
  - 以及每页 `ppn / acc_id / local_page_idx / paddr`
- 在 stage exec-view 绑定侧打印：
  - `exec-bind`
  - 输出 `vpage / ppn / acc_id / local_page_idx / pte / paddr`
- 详细页日志只对当前最可疑边界打开：
  - `segment 0 / stage 0`
  - 其他位置保留 summary

工程准则：

- 面向慢启动的 Linux/F2 回放，优先把“下一轮一定会用到的现场信息”一次打全
- 但这些日志应做成 `marker-only`
  - 默认 host build 不受影响
  - 深挖时再打开
- 每次加这类日志后，至少本地验证两种构建：
  - 默认构建
  - `PIPELINE_RUNTIME_ONLY_MARKER=1`
