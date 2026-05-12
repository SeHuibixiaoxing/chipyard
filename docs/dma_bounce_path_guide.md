# DMA Bounce Path 导读

更新时间：`2026-04-21`

## 这份文档回答什么问题

这份文档只回答下面几个问题：

1. 现在 `pipeline-runtime` 里的 DMA，到底有没有启用 `bounce path`。
2. 为什么不能简单地“全部走 direct path”。
3. `bounce path` 的实现原理到底是什么。
4. 这是不是 Gemmini / CoupledDMA 硬件里的独立通路。

本文尽量写成“你不先读源码，也能先把原理想清楚”的形式。

## 一句话结论

当前代码里，`bounce path` 是：

- `pipeline-runtime` 在 Linux/RISC-V host `<->` shared-SPM DMA 路径上的一条软件中转路径；
- 不是 Gemmini 硬件 DMA 内部一条叫 `bounce` 的硬件通路；
- 没有“全局总开关”，而是每个 DMA chunk 在提交前动态判断是否要走；
- 判定条件目前很明确：
  `bytes >= 64 && src_mod64 != dst_mod64`。

换句话说：

- 现在确实“支持并启用了 bounce path 代码”；
- 但不是每次 DMA 都走；
- 大多数对齐正常的请求，仍然优先走 direct path。

## 1. 先分清：这里的 bounce 不是硬件概念

从当前仓库代码能直接确认：

- `bounce path` 的主要实现位于
  `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_dma.c`
- 运行时状态保存在
  `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/include/prt_runtime.h`
  里的 `stage_dma_bounce[]`
- 生命周期释放在
  `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_action_exec.c`

而在硬件侧：

- `generators/gemmini/src/main/scala/gemmini/GemminiCoupledDMA.scala`
- `generators/gemmini/src/main/scala/gemmini/GemminiDirectDMA.scala`

源码里并没有一条叫做 `bounce path` 的硬件分支。

硬件 DMA 的核心语义仍然只是：

- 接收 `src_addr`
- 接收 `dst_addr`
- 接收 `len`
- 然后发 TileLink 读写，完成 copy

所以这里所谓的 `bounce`，本质上是软件先把数据搬到一个“更适合 DMA 的 host buffer”，再把那个 buffer 的物理地址交给硬件。

## 2. 现在到底算不算“开启了 bounce path”

如果你问的是“代码里有没有这条路径，而且运行时会不会走到”，答案是：

- 有；
- 会；
- 但只在特定场景下。

更准确地说：

- 只有 Linux/RISC-V 的 host `<->` SPM 路径会用到这套逻辑；
- 只有当某个 chunk 满足 `dma_chunk_needs_bounce(...)` 时才进入；
- 不满足条件时，仍然走 direct path。

当前精确判定函数是：

```c
static int dma_chunk_needs_bounce(uint64_t src_addr, uint64_t dst_addr, uint64_t bytes) {
  return bytes >= 64ULL && dma_debug_mod64(src_addr) != dma_debug_mod64(dst_addr);
}
```

它的意思很直接：

- 小于 `64B` 的传输，不进 bounce；
- 大于等于 `64B` 的传输，如果源地址和目标地址的 `mod64` 残差不同，就进 bounce；
- 如果两边 `mod64` 一样，就仍然 direct。

这里没有额外配置开关，也没有 YAML 级别的 enable/disable。
它就是一条编译进 runtime 的 guardrail。

### 2.1 `mod64` 到底是什么意思

先看源码里的定义：

```c
static inline uint64_t dma_debug_mod64(uint64_t addr) {
  return addr & 63ULL;
}
```

所以这里的 `mod64`，本质上就是：

```text
addr mod 64
```

也就是：

- 一个地址落在当前 `64B` 对齐块里的第几个字节；
- 或者说，这个地址相对最近一个 `64B` 边界的偏移量。

例如：

- `0x1000 mod 64 = 0`
- `0x1010 mod 64 = 16`
- `0x103f mod 64 = 63`
- `0x1040 mod 64 = 0`

因此你可以把任意地址写成：

```text
addr = 64B边界基址 + 块内偏移
```

其中：

- `64B边界基址 = addr & ~63`
- `块内偏移 = addr & 63`

这里的 `mod64`，说的就是这个“块内偏移”。

### 2.2 这里说的“不对齐”不是“某一端没按 64B 对齐”

这是最容易误解的地方。

当前 bounce 规则里的“`mod64` 不对齐”，真正指的是：

```text
src_mod64 != dst_mod64
```

也就是：

- 源地址在自己的 `64B` 块里处于什么偏移；
- 目标地址在自己的 `64B` 块里处于什么偏移；
- 这两个偏移不一样。

所以它不是在问：

- “源地址是不是 64B 对齐”
- “目标地址是不是 64B 对齐”

而是在问：

- “源和目标是不是落在各自 `64B` 块里的相同位置”

这两者差别很大。

例如：

1. `src = 0x1000`, `dst = 0x2000`
   - `src_mod64 = 0`
   - `dst_mod64 = 0`
   - 两边相同，属于“匹配”
2. `src = 0x1010`, `dst = 0x2010`
   - `src_mod64 = 16`
   - `dst_mod64 = 16`
   - 两边虽然都不是 64B 对齐，但仍然“匹配”
3. `src = 0x1010`, `dst = 0x2000`
   - `src_mod64 = 16`
   - `dst_mod64 = 0`
   - 这才是当前文档里说的 `mod64` 不匹配

所以更准确的说法其实是：

> 这里的“对齐”说的是**源端和目的端的 64B 相对偏移是否一致**，不是看单边地址是否整齐地落在 64B 边界上。

### 2.3 为什么偏移一致这么重要

如果把一次大于等于 `64B` 的搬运想成：

- 从源地址开始，连续取一串字节；
- 再从目标地址开始，连续放一串字节；

那么最理想的情况是：

- 源的第 `0` 个字节正好对应目标的第 `0` 个字节；
- 源的第 `64` 个字节正好对应目标的第 `64` 个字节；
- 两边的 `64B` 边界关系是一致的。

这正对应：

```text
src_mod64 == dst_mod64
```

一旦两边偏移不同，比如：

- 源从块内偏移 `16` 开始；
- 目标从块内偏移 `0` 开始；

那这次 copy 的逻辑就变成：

```text
源块 [16..63] -> 目标块 [0..47]
下一块源块 [0..15] -> 同一个目标块 [48..63]
```

也就是说：

- 源端和目的端的 `64B` 边界不再同步；
- 一个逻辑 chunk 会跨不同的 `64B` 边界关系；
- runtime 目前把这种形状当成 Linux host `<->` SPM 路径上的危险形状。

### 2.4 它会在什么情况下发生

当前最常见的几类场景是：

1. 一边是 `64B` 对齐，另一边不是
   - 例如 `src_mod64 = 0`, `dst_mod64 = 16`
   - 或 `src_mod64 = 32`, `dst_mod64 = 0`
2. 两边都不对齐，但偏移不同
   - 例如 `src_mod64 = 16`, `dst_mod64 = 48`
3. host buffer 不是从对象起始地址开始传，而是从某个 slice / 子张量偏移开始传
   - 比如 host 上这个 tensor 从 `base + 16` 开始
4. shared SPM 一侧的目标地址或源地址带了非零局部偏移
   - 比如不是整个 page 从头写，而是 `page_base + copied`
   - 或者 tensor 在 stage-local layout 里本来就不是从 `64B` 边界开始

尤其在当前 runtime 里，host `<->` SPM 搬运常常是：

- host 端来自某个 Linux 用户态 buffer
- SPM 端来自 `page_base_addr(...) + copied`

其中：

- host 指针是否 `64B` 对齐，不一定；
- 但 shared SPM page base 往往是页对齐的，而页大小 `1024B` 又是 `64` 的倍数；
- 所以如果第一次 host 起点已经是 `+16`、`+32` 这类偏移，就很容易形成：
  - host 端 `mod64 != 0`
  - SPM 端 `mod64 == 0`

这正是最典型的 mismatch。

### 2.5 为什么它常常“一开始不匹配，后面就一直不匹配”

对同一条连续 copy 来说，源码里每一轮循环基本都是：

- `src_ptr = src_base + copied`
- `dst_addr = dst_base + copied`

两边都加的是同一个 `copied`。

所以如果一开始：

```text
src_base mod 64 != dst_base mod 64
```

那么后面通常仍然保持：

```text
(src_base + copied) mod 64 != (dst_base + copied) mod 64
```

也就是说：

- chunk 切分本身不会自动把 mismatch 修好；
- host page 分 chunk、SPM page 分 chunk，通常只是把大传输拆小；
- 但“源和目的的相对偏移关系”仍然保留下来。

这也是为什么 runtime 不会指望“下一轮 chunk 也许就自然好了”，而是直接在当前 chunk 上决定是否 bounce。

### 2.6 一个最典型的例子

你前面看到文档里提到的例子：

- host 端起点 `0x...10`
- shared-SPM 端起点 `0x...00`
- 传输长度 `1024B`

它对应的就是：

```text
src_mod64 = 16
dst_mod64 = 0
bytes = 1024 >= 64
```

于是命中：

```text
bytes >= 64 && src_mod64 != dst_mod64
```

这正是 coverage 里那个：

- `dma_dram_to_shared_misaligned_fullpage`

它不是说：

- “host 指针没页对齐”
- “shared SPM 地址非法”

而是在说：

- 这笔 copy 从源端和目的端看，落在各自 `64B` 块里的起始相位不同。

### 2.7 什么时候虽然“不整齐”，但仍然不算 mismatch

这一点也很重要。

下面这类情况，虽然两边都不是 `64B` 边界起点，但不会触发当前 bounce 规则：

```text
src = 0x...10
dst = 0x...10
bytes >= 64
```

因为：

```text
src_mod64 = 16
dst_mod64 = 16
```

两边相位一样。

所以当前规则不是：

- “只要看到非 64B 对齐就 bounce”

而是：

- “只要大 chunk 的源/目的 `64B` 相位不同，就 bounce”

### 2.8 为什么这种 copy 会被认为是“危险形状”

这里要分三层来讲：哪些是**已确认事实**，哪些是**结合硬件实现的合理推断**，哪些是**还没有被单点硬件 bug 证据完全钉死**的部分。

#### 第一层：已确认事实

当前仓库里已经可以确认三件事：

1. 这类请求形状在历史 Linux host `<->` shared-SPM 路径上，曾经稳定触发过 hang 或失败。
2. 当前 runtime 对它加了专门 guardrail：
   - `bytes >= 64`
   - `src_mod64 != dst_mod64`
   - 优先改走 bounce
3. 一旦把 host 端换成一个 `mod64` 与非 host 端一致的 bounce buffer，
   同一个 DMA 硬件路径通常就能稳定工作。

也就是说，当前软件团队其实已经接受了下面这个工程事实：

> **“对这条平台主线来说，`src_mod64 != dst_mod64` 的大 chunk 不是一个值得继续冒险直接提交的形状。”**

这里“危险”首先不是理论词，而是：

- 它在已有平台/配置上有过稳定坏历史；
- 而 bounce 能把它稳定改成好历史。

#### 第二层：为什么从数据形状上看，它本来就更复杂

当：

```text
src_mod64 == dst_mod64
```

时，源端和目的端的 `64B` 边界关系是同步的。

例如：

```text
src = ...10
dst = ...10
copy 128B
```

那么这笔传输可以理解成：

- 先处理一个相同长度的前缀残块；
- 后面源/目的的 `64B` 边界会继续同步前进。

而当：

```text
src_mod64 != dst_mod64
```

比如：

```text
src = ...10
dst = ...00
copy 128B
```

这时每个目标 `64B` 块，往往要由两个不同源 `64B` 块的片段拼出来：

```text
源块0[16..63] + 源块1[0..15]  ->  目标块0[0..63]
源块1[16..63] + 源块2[0..15]  ->  目标块1[0..63]
```

也就是说：

- 目标端的 `64B` 边界，不再和源端的 `64B` 边界同步；
- 每一段逻辑 `64B` 输出，都跨了两段源侧 `64B` 区域；
- 这种形状比“相位一致”的 copy 更碎、更难被底层路径平滑处理。

所以从纯数据搬运形状上说，它就不是“最干净”的那类请求。

#### 第三层：为什么这里偏偏盯住 `64B`

这不是随便拍脑袋选的常数。

当前 P12 pair-manager `sbus128` 配置里：

- DMA / TL 的 `beatBytes` 是 `16B`
- 但 shared scratchpad 配置里的
  `local_bank_interleaved_bytes = max(gemminiBeatBytes, 64) = 64`

也就是说，在当前目标上：

- `16B` 是总线 beat 粒度；
- `64B` 是 shared-SPM 这边更“结构化”的 stripe / interleave 粒度。

因此软件用 `mod64` 做 guardrail，关注的不是：

- “这笔请求能不能发出单个 16B beat”

而更像是在关注：

- “这笔请求在 shared-SPM 的 64B 结构边界上，是不是保持了同相位”

这也是为什么当前规则比硬件 `beatBytes` 条件更保守。

#### 第四层：结合当前 DMA 硬件实现，可以做出的合理推断

从 `GemminiCoupledDMA.scala` 可以确认：

- DMA 是一个 `Get -> 暂存读回数据 -> Put` 的 copy engine；
- 它优先走 wide copy；
- 只有当 `src` 和 `dst` 都满足 beat 对齐时，才进入当前轮的 wide 传输；
- 否则就退成更细粒度的处理。

同时在当前配置里：

- shared-SPM 一侧有自己更粗的 `64B` 结构粒度；
- host `<->` SPM 这条路径又叠加了：
  - host page chunk
  - `virt_to_phys`
  - TLFragmenter / TLWidthWidget
  - local shared-spad ingress

因此**合理推断**是：

> 当 `src_mod64 != dst_mod64` 时，整笔大 copy 会长期保持“64B 相位错位”，从而让这条 host `<->` SPM 路径持续处在一种更碎、更偏、边界关系更差的传输形状上。

这不等价于“我们已经精确证明某一行硬件代码有 bug”，但它足以解释为什么：

- 相位一致的 direct path 通常更稳；
- 相位不一致的 direct path 历史上更容易出问题；
- bounce 通过把 host 端改造成“相位一致”，能明显改善稳定性。

#### 第五层：目前还不能过度声称的部分

到当前仓库证据为止，我们**还不能**严谨地说成：

- “硬件唯一根因就是 `GemminiCoupledDMA.scala` 的 wide/byte 选择逻辑本身”
- 或者
- “已经精确证明是某个 TL adapter / 某个 local bank 处理器在 `src_mod64 != dst_mod64` 时必错”

现有证据更像是：

1. 这类请求形状在真实平台上曾经稳定危险；
2. 当前配置下 `64B` 确实是一个有硬件结构意义的边界；
3. bounce 通过把两端相位拉齐，能把危险形状改造成稳定形状。

所以更准确的表述应当是：

> **它之所以“危险”，不是因为我们已经把单点硬件 bug 逐门逐线地证明完了，而是因为现有平台证据反复表明：大 chunk 的 `64B` 相位错位，会把 host `<->` shared-SPM DMA 推进到一类更复杂、历史上确实更容易挂的传输形状。**

## 3. bounce path 出现在什么数据路径上

当前最重要的是两条 Linux host 路径：

1. host -> SPM
2. SPM -> host

也就是：

- `prt_dma_copy_dram_to_spm_pages_prefix(...)`
- `prt_dma_copy_spm_pages_to_dram_prefix(...)`

在 Linux/RISC-V 下，这两个入口都会转进：

- `dma_copy_host_to_spm_pages_linux(...)`
- `dma_copy_spm_pages_to_host_linux(...)`

而纯 SPM -> SPM 的页拷贝不走这一套 bounce 逻辑。

所以你可以把它理解成：

- bounce 是“只要一端是 Linux host buffer”时的保护机制；
- 不是所有 DMA 请求的通用硬件模式。

## 4. 为什么不能全部走 direct path

这是最核心的问题。

### 4.1 第一层原因：host 虚拟地址不等于硬件可直接 DMA 的地址

在 Linux 用户态：

- 程序手里拿到的是虚拟地址；
- DMA 硬件真正需要的是物理地址；
- 而且用户态 buffer 跨页时，通常只保证虚拟连续，不保证物理连续。

所以只要一端是 host buffer，就不能偷懒地：

- 拿第一页的物理地址；
- 再配一个总长度；
- 就当整个大 buffer 都连续。

当前 runtime 的正确做法是：

- 按 host page 分 chunk；
- 每个 chunk 单独做一次 `virt_to_phys`；
- 再把该 chunk 的物理地址提交给 DMA。

这就是为什么 direct path 本身也已经不是“原始 host 指针直接交硬件”，而是：

- 先 `virt_to_phys`
- 再 direct DMA

### 4.2 第二层原因：某些大块 misaligned 形状，被 runtime 当成高风险形状

当前代码里的 bounce 不是“为了快”，而是“为了避开有坏历史的请求形状”。

从当前仓库能直接确认三件事：

- runtime 里确实把
  `bytes >= 64 && src_mod64 != dst_mod64`
  编成了真实 guardrail；
- 旧版 Linux DMA guardrail 文档把这种形状记成
  “曾经踩过的坑”；
- Linux coverage 里专门保留了一个针对这类形状的 case。

典型例子是：

- host 端偏移 `0x10`
- shared-SPM 端偏移 `0x00`
- 传 `1024B`

仓库里专门有对应的 Linux coverage case：

- `rerocc-linux-tests-coupleddma/rerocc_lc_coverage_linux_coupleddma.c`
- case 名字是 `dma_dram_to_shared_misaligned_fullpage`

所以这里的设计逻辑不是：

- “direct path 理论上永远最好”

而是：

- “对齐形状安全时走 direct”
- “有坏历史的形状先做软件中转”

### 4.2.1 风险复核：它不是“当前一跑就必挂”的硬约束

这次重新核对仓库里的现成结果后，还必须补上一层更谨慎的结论。

当前仓库里能找到的小 Linux 回归结果表明：

- `2026-03-20--06-16-29-rerocc-lc-linux-coupleddma-regression-small-pipelinefiles-...`
  的 `uartlog` 里有：
  - `CASE_START dma_dram_to_shared_misaligned_fullpage`
  - `CASE_RESULT dma_dram_to_shared_misaligned_fullpage PASS`
  - `ALL_TESTS_PASS`
- `2026-03-23--06-54-07-rerocc-lc-linux-coupleddma-regression-small-pipelinefiles-...`
  的 `uartlog` 里也有同样的 `PASS`
- `2026-03-22--12-56-59-...` 这轮虽然 coverage 整体失败，但这一个
  `misaligned_fullpage` case 本身仍然是 `PASS`

这说明一件很重要的事：

- `bytes >= 64 && src_mod64 != dst_mod64`
  不是一个“在当前仓库里必然挂死”的语义硬错误；
- 至少从现有小回归结果看，direct path 在某些环境/版本下是跑通过的。

因此，当前最稳妥的说法应该是：

- 这是一种**有坏历史、值得防守的危险形状**；
- runtime 选择对它加保守 guardrail；
- 但仅凭当前仓库可见证据，还不能把它升级成
  “现在这版底层一定很容易挂” 的普遍结论。

更准确地说，它更像是：

- 一种平台相关、路径相关、时序相关的风险形状；
- 而不是源码层面已经能证明“只要出现就必错”的硬约束。

### 4.2.2 如果明知不对齐，仍然强走 direct path，软件到底会怎么做

这一步最容易被误解成：

- “软件会先把头尾拆开，对齐后再交硬件”
- “软件会自动把 misaligned 请求改写成某种半对齐形式”

当前实现里，**direct path 不会做这种修正**。

它做的事情其实很直接：

- 仍然按 host page 分 chunk；
- 仍然对当前 chunk 做 `virt_to_phys`；
- 但不会改写这笔 chunk 的起始相位；
- 然后把这笔 chunk 的原始 `src/dst/bytes` 直接交给 DMA。

对 `host -> SPM` 来说，direct path 的关键动作是：

- `src_ptr = 原始 host 指针 + copied`
- `src_pa = virt_to_phys(src_ptr)`
- `req.src_addr = src_pa`
- `req.dst_addr = dst_addr`
- `req.bytes = chunk`

也就是说：

- host 端如果原来是 `...10` 起步，
- SPM 端如果原来是 `...00` 起步，
- direct path 不会把它改成 `...00 -> ...00`
- 而是原样提交 `...10 -> ...00`

对 `SPM -> host` 来说也一样：

- `dst_ptr = 原始 host 指针 + copied`
- `dst_pa = virt_to_phys(dst_ptr)`
- `req.src_addr = src_addr`
- `req.dst_addr = dst_pa`
- `req.bytes = chunk`

这里也不会有：

- 预先 `memcpy` 到中转页
- 软件补一个对齐前缀
- 软件把一笔请求拆成“头/中/尾”三类 DMA 请求

所以 direct path 的真实语义可以概括成一句话：

> **软件负责把 host VA 变成 host PA，但不会负责把 misaligned 相位修正成 aligned 相位。**

换句话说：

- direct path 会保留这笔 copy 原始的低位地址关系；
- 硬件最终看到的，就是这对原始相位关系。

### 4.2.3 硬件收到 misaligned 的 direct request 后，会怎么处理

从软件接口上看，CoupledDMA 并没有一个“misaligned 模式”。

软件最终只做两件事：

- `set_dst(dst_addr, completion_addr)`
- `set_src(src_addr, num_bytes)`

也就是说，硬件拿到的只是：

- 源地址
- 目标地址
- 长度

没有额外的“这是 bounce / 这是 direct / 这是 misaligned 例外处理”标志。

真正的处理逻辑在
`GemminiCoupledDMA.scala`
内部状态机里。

它每次 copy 的基本循环是：

1. 先发一次 `Get`，从 `src` 读数据
2. 等返回数据
3. 再发一次 `Put`，往 `dst` 写数据
4. 更新 `curSrc / curDst / curRemaining`
5. 如果还没搬完，就继续下一轮
6. 全部搬完后，再写 completion flag

也就是说，它不是“一条神奇的块搬运原语”，而是一个小状态机，自己循环发 TileLink 读写。

### 4.2.4 它靠什么决定走宽传输还是窄传输

关键判定是：

- 当前 `src` 是否按 `beatBytes` 对齐
- 当前 `dst` 是否按 `beatBytes` 对齐
- 剩余长度是否至少还有一个 `beatBytes`

在当前 `sbus128` 目标上：

- `beatBytes = 16`

所以硬件真正看的第一层对齐，其实是：

- **16B beat 对齐**

不是 runtime guardrail 用的：

- **64B phase 对齐**

源码里的判定大意是：

```text
如果 src 和 dst 都 16B 对齐，并且 remaining >= 16B：
    这轮搬 16B
否则：
    这轮只搬 1B
```

也就是说：

- 能 wide，就发一轮 `16B Get + 16B Put`
- 不能 wide，就退化成 `1B Get + 1B Put`

### 4.2.5 “退化成 1B 模式”时，硬件具体在做什么

这部分非常关键，因为它解释了为什么 direct path 并不会“修正”错相位。

当硬件发现这轮不能 wide 时，它会：

- 只请求 `1B` 的 `Get`
- 从返回的 beat 数据里，用 `src` 的低位偏移把目标字节移出来
- 再把这个字节按 `dst` 的低位偏移，摆到 `Put` 数据总线的对应位置
- 然后发一个 `1B` 的 `Put`

所以它做的是：

- **按字节精确复制**

而不是：

- “先把源和目标重新对齐，再按块复制”

这意味着：

- 功能语义上，它仍然是在正确地复制字节；
- 但地址相位关系并没有被修正；
- 只是硬件自己用更细粒度的事务把这件事完成。

### 4.2.6 不同 misaligned 形状下，硬件会落到两种很不一样的 direct 行为

这里要特别区分两种情况。

第一种：

- `src mod 16 == dst mod 16`

虽然它们可能：

- 都不是 64B 边界
- 甚至 `src_mod64 != dst_mod64`

但只要它们对 **16B beat** 的相位一致，硬件就有机会进入 wide 模式。

例如强行 direct 提交：

- `src = ...10`
- `dst = ...00`

在当前硬件里：

- `src mod 16 = 0`
- `dst mod 16 = 0`

所以从 **16B 对齐** 角度看，它们其实都是对齐的。

这会导致：

- 硬件完全可能从第一拍开始就按 `16B` 宽传输推进；
- 但整个 copy 在 **64B 相位** 上仍然是错开的。

这正说明：

- **64B mismatch 不等于硬件一定会退化成 byte mode**

第二种：

- `src mod 16 != dst mod 16`

例如：

- `src = ...18`
- `dst = ...00`

这时当前轮次即使都加同一个 `copied`，两边也不会同时落到 `16B` 边界。

原因很简单：

- 每一轮更新时，`src` 和 `dst` 加的是同样的字节数；
- 如果一开始两边的 `mod16` 不同，
- 那后面它们的 `mod16` 差值会一直保持不变。

于是结果就是：

- 它们永远不会“同时 16B 对齐”
- `canWide` 会一直为假
- 整笔 direct request 会长期停留在 `1B Get + 1B Put` 模式

因此，misaligned 的 direct path 至少有两种完全不同的硬件后果：

1. `mod16` 相同：
   硬件仍可能进入或保持 `16B` wide copy
2. `mod16` 不同：
   硬件会长期甚至全程停留在 `1B` copy

### 4.2.7 为什么软件还要盯着 `mod64`，而不是只盯 `mod16`

这正是软件 guardrail 与硬件局部对齐规则的区别。

硬件当前直接处理的是：

- `beatBytes = 16`

它关心的是：

- 当前这一拍能不能发一个 `16B` 事务

但软件的 bounce 规则盯的是：

- `mod64`

因为它想防守的不是“当前这一拍能不能 wide”，而是：

- 整笔 host `<->` SPM copy 在更大结构粒度上，是否长期保持错相位。

在当前配置里：

- shared scratchpad 的 `local_bank_interleaved_bytes = 64`

所以哪怕硬件还能继续发 `16B` 宽事务，只要：

- `src_mod64 != dst_mod64`

整笔 copy 仍然可能长期保持一种
“16B 事务能发，但 64B 结构相位始终错开”
的形状。

也就是说：

- 硬件的 `16B` wide/byte 判定，解决的是“这一拍怎么发”
- 软件的 `64B` bounce guardrail，防守的是“整笔请求处在什么结构形状里”

这两层不是一回事。

### 4.2.8 用一句话概括 misaligned + direct 的真实语义

如果把这件事压缩成一句话，那就是：

> **direct path 在 misaligned 时不会替你修相位；软件只会原样提交，硬件只会按自己的 beat 对齐规则把这笔“错相位请求”硬搬过去。**

所以 direct path 的问题不在于：

- “软件完全不会处理 misaligned”

而在于：

- 软件不会替你把它改造成更好形状；
- 硬件也不会替你把 `64B` 相位拉齐；
- 它只会把这笔原始 shape 尽量搬完。

### 4.2.9 不改硬件、也不增加额外拷贝，能不能彻底解决

先给最短结论：

- **不能用“透明小修补”的方式彻底解决**
- **但可以用“地址相位合同”或“布局合同”的方式规避**

也就是说：

- 如果你的要求是：
  - 不改硬件
  - 不引入 bounce 那种额外中转拷贝
  - 还想让 runtime 对任意输入地址都自动安全
- 那当前模型下没有一个完全通用、完全透明的软件小补丁。

但如果你的要求放宽成：

- 不改硬件
- 不增加额外 copy
- 可以修改源/目的缓冲区的布局约束

那还是有办法的。

### 4.2.10 为什么“只靠切 chunk”不能从根上修掉 mismatch

这是最关键的一个不变量。

假设原始 copy 是：

```text
src = S
dst = D
```

如果你不引入中转缓冲区，那么任意一个子请求都只能是：

```text
src = S + k
dst = D + k
```

这里的 `k` 只是“已经复制了多少字节”。

于是有：

```text
((S + k) - (D + k)) mod 64
= (S - D) mod 64
```

也就是说：

- `src` 和 `dst` 的 **64B 相位差是常数**
- 你把一笔大请求拆成很多笔小请求，这个相位差也不会自己消失

因此下面这些想法，都**不能从根上把 mismatch 变成 match**：

- 只做 `head/body/tail` 切分
- 只把 chunk 改小
- 只把一笔 1024B 改成很多笔 64B、32B、16B

它们能做的最多只是：

- 改变“每一笔 DMA 有多大”
- 改变“硬件落到 wide 还是 byte mode”
- 改变“风险暴露得更强还是更弱”

但它们**不能改变这笔 copy 的原始相位关系**。

所以如果目标是“把坏形状变成好形状”，那单纯切 chunk 不是根解。

### 4.2.11 真正可行的零拷贝方案，本质上都在“改一端的相位”

如果不许 bounce，不许额外 copy，那想把
`src_mod64 != dst_mod64`
变成
`src_mod64 == dst_mod64`
，唯一思路就是：

- **在 DMA 开始之前，就让源端或目的端的起始相位本身变掉**

这类方案本质上都属于“地址相位合同”。

#### 方案 A：约束 host 侧缓冲区，让它从一开始就满足期望相位

这是最干净的一类零拷贝方案。

做法不是：

- 运行时拿到一个随便什么指针后再去补救

而是：

- 在更上游就规定：
  - 输入/输出 tensor 的逻辑起点必须满足某个 `mod64`
  - 或至少要求 `64B` 对齐
  - 并避免把真正传给 DMA 的 tensor 起点放在 `+16/+32/+48` 这种 slice 偏移上

例如：

- host buffer 用 `posix_memalign(64, ...)`
- tensor 有 padding，但逻辑起点仍然放在约定相位
- 上游框架不要把传给 DMA 的 base pointer 设成“对齐大 buffer 中间的一个偏移子视图”

它的优点是：

- 真正零拷贝
- 不改 DMA 硬件
- 不改 DMA 指令协议

它的限制是：

- 这不是 runtime 的“透明修复”
- 而是调用方 / 上游框架 / 内存分配器必须配合

一旦外部仍然给你一个任意偏移的用户态指针，这个办法就失效。

#### 方案 B：约束 SPM 侧布局，让目的端相位主动去匹配 host

如果 host 端地址不容易控制，另一条路就是：

- 不改 source
- 去改 destination 在 shared SPM 里的放置相位

本质上是：

- 让 tensor 在 shared SPM 里的逻辑起点，不再总是“天然 page base”
- 而是带一个精心选择的局部偏移
- 使得：
  `dst_mod64 == src_mod64`

这同样是零拷贝：

- 数据没有进 bounce buffer
- 也没有多一次 host copy
- 只是 tensor 在 SPM 里的落点变了

但它比方案 A 更侵入系统设计，因为它会影响：

- tensor local address 的形成
- 后续 Gemmini / DMA / export 对这个 tensor 的所有地址计算
- allocator 的碎片与容量利用率

并且它通常要求 runtime 在真正分配 SPM 地址时，已经知道：

- 这块 tensor 将来主要和哪个 host buffer 对接
- 对方希望的相位是多少

如果这两个条件不成立，这条路也难走。

#### 方案 C：让 producer 和 consumer 共同遵守固定相位合同

前两种其实可以合并成一个更一般的说法：

- 不要让 runtime 在“最后一公里”补救地址形状
- 而是在系统层规定：
  - host tensor 的起点相位
  - shared SPM tensor 的起点相位
  - 两边必须兼容

这才是“零拷贝但又想稳”的最像工程解的一条路。

它的本质是：

- **把 bounce 从运行时补救，前移成布局期合同**

### 4.2.12 哪些办法只能算缓解，不能算根解

下面这些办法可以考虑，但都不应被叫做“彻底解决”。

#### 缓解 1：把大请求拆小，强行 direct 提交

例如：

- 原来 1024B 一笔
- 改成很多笔 32B 或 16B

它的特点是：

- 不改硬件
- 不增加额外 copy
- 实现也简单

但问题是：

- 它没有改变 `src-dst` 的相位差
- 只是缩短了每一笔 DMA 的长度

因此它更像：

- **经验性降风险**

而不是：

- **从原理上消除坏形状**

另外它还会带来明显代价：

- 更多 `set_dst/set_src`
- 更多完成等待 / fence
- 更低吞吐

所以它最多适合：

- debug
- 临时保底
- 证明“问题是否与大 chunk 持续形状相关”

不适合当长期主线解法。

#### 缓解 2：只依赖硬件自动退化到 byte mode

这也不是根解。

原因是硬件 byte mode 的语义只是：

- 按字节把数据搬对

而不是：

- 把结构相位重新修正好

所以它解决的是：

- “这拍怎么搬”

不是：

- “整笔请求是否仍处在坏结构形状里”

#### 缓解 3：只改 wait/fence/阻塞语义

这同样不是根解。

因为 wait/fence 改的是：

- 提交后的完成语义

而 mismatch 的核心问题在于：

- 你提交给硬件的地址形状本身

提交形状没变，wait/fence 本身不会把坏形状变成好形状。

### 4.2.13 当前系统里，最现实的零拷贝方向是什么

如果你坚持：

- 不动硬件
- 不做 bounce copy

那我认为当前最现实的方向不是“继续找一个更聪明的 direct helper”，而是：

- **phase-aware placement / alignment contract**

也就是二选一：

1. host 侧保证输入输出 tensor 的 DMA 起点相位可控
2. runtime / allocator 让 SPM 侧 tensor 落点相位可控

其中：

- 如果上游 buffer 是你自己分配的，优先做 host 侧合同
- 如果 host 侧来自外部框架、不容易约束，才考虑做 SPM 侧 phase-aware 布局

### 4.2.14 一个需要特别注意的现实限制

即使你愿意做 phase-aware 布局，也不是所有场景都能无痛成功。

因为同一块 tensor 可能同时面对多个路径：

- host -> SPM fixed-load
- SPM -> host export
- SPM -> SPM internal copy
- Gemmini compute local access

如果不同路径希望的“最佳相位”不一样，那么你仍然会遇到冲突。

这时通常只能三选一：

1. 选一个主路径优先优化
2. 接受某些路径仍然需要 bounce
3. 回到硬件层做真正的结构性支持

所以“零拷贝 + 不改硬件”的路不是完全没有，但它通常意味着：

- 更强的地址布局合同
- 更高的 allocator 复杂度
- 更强的上层调用约束

而不是一个 runtime 内部的几行小补丁。

### 4.2.15 Gemmini 内部 DMA 是怎么处理 misaligned 的

这是一个很有价值的对照，因为它确实提供了一条：

- 不靠 bounce page
- 不增加额外 host copy
- 仍然能正确处理 misaligned

的思路。

但先说结论：

- **Gemmini 内部 DMA 能做到，不是因为软件更聪明**
- **而是因为它的硬件本来就比当前 CoupledDMA 更强**

它不是一个“当前 CoupledDMA 软件路径可直接照抄”的方案，
而更像是：

- 一个硬件设计参考
- 一个能力边界参考

#### 4.2.15.1 Gemmini 读内存时，不是“从原始地址直接硬搬”

在 Gemmini 的读路径里，外部内存地址如果是不对齐的，它不会简单地说：

- “那我就按这个 misaligned 地址直接发一笔同形状 copy”

它会做两步：

1. 先把外部读地址**向下对齐**到某个合法请求粒度
2. 再把多读进来的前缀字节，在内部用 `shift` / `discard` 逻辑丢掉

也就是说，它允许：

- **对外多读一点**
- **对内精确取想要的那一段**

源码上能看到它会：

- 枚举一组合法 `read_sizes`
- 把 `vaddr` 对齐到这个 size
- 记录原始地址在这个对齐块里的 `offset`
- 把这个 `offset` 作为 `shift` 带进后面的内部整理逻辑

然后 `BeatMerger` 会做的事就是：

- 把回来的 beat 放到内部 buffer
- 根据 `shift` 丢掉不需要的前缀字节
- 再按 Gemmini scratchpad 行宽重新打包

所以 Gemmini 读 DMA 的关键能力不是：

- “misaligned 也照着搬”

而是：

- **先对外发 aligned 读，再在硬件内部做字节级重组**

#### 4.2.15.2 Gemmini 写内存时，也不是“只能整块 aligned 写”

Gemmini 的写路径同样更强。

它会：

- 先根据目标地址偏移，选择一个对齐后的写包
- 生成针对每个 beat 的 `mask`
- 如果不是完整覆盖，就发 `PutPartial`
- 同时把数据按目标偏移做 `shift`

也就是说，它支持：

- **对齐后的地址**
- **部分字节有效**
- **写数据在 beat 内部重定位**

这和当前 CoupledDMA 的思路很不一样。

当前 CoupledDMA 更像：

- 给我 `src/dst/len`
- 我自己循环做 `Get/Put`
- 能 wide 就 whole-beat
- 不能 wide 就 1B

而 Gemmini 写 DMA 更像：

- 我可以把一次 misaligned 写，翻译成
  - 对齐地址
  - 局部 mask
  - 经过 shift 的写数据

这就是为什么它能在**不做 bounce copy**的情况下，仍然把 misaligned 的 host-side 数据正确落到目标位置。

#### 4.2.15.3 它真正依赖的核心能力是什么

把 Gemmini 内部 DMA 的做法抽象一下，本质上它依赖三件能力：

1. **aligned overfetch**
   - 读的时候，允许比逻辑请求多拿一点前后缀字节
2. **internal shift / discard / repack**
   - 在硬件内部把真正需要的字节重新拼出来
3. **partial masked write**
   - 写的时候，不要求整块覆盖，可以只写目标块里的一部分字节

这三件事合在一起，才构成了：

- 真正的零拷贝 misaligned 处理能力

#### 4.2.15.4 为什么这不能直接变成当前 CoupledDMA 的纯软件修复

关键障碍在于：

- Gemmini 内部 DMA 暴露给软件的接口，不是一个“通用 memcopy engine”
- 当前 CoupledDMA 暴露给软件的接口，也没有这些可编程能力

当前 CoupledDMA 软件能下发的核心信息只有：

- `dst_addr`
- `completion_addr`
- `src_addr`
- `num_bytes`

它没有给软件一个接口去表达：

- “请把读地址向下对齐到 64B/16B”
- “请把有效载荷右移/左移 N 个字节”
- “请只写 mask 覆盖的那几字节”

所以当前软件层缺的不是：

- “一个更复杂的 if/else”

而是：

- **底层 DMA 原语本身就没有 Gemmini DMA 那种 shift/mask/repack 能力**

因此，如果不改 CoupledDMA 硬件和接口，
单靠软件是没法把 Gemmini 这套办法完整搬过来的。

#### 4.2.15.5 它能给当前问题什么真正的参考价值

它最重要的参考价值不是：

- “我们现在就能照着改几行 C 代码”

而是下面这条判断基准：

> 如果你想在“不加 bounce copy”的前提下，真正稳妥地处理 misaligned copy，那么底层至少要有“对齐读 + 内部重组 + 掩码写”这类能力。

也就是说，Gemmini DMA 告诉我们的不是：

- 当前 CoupledDMA 软件应该怎么 patch

而是：

- **什么样的 DMA 设计，才真的有资格说自己支持零拷贝 misaligned 处理**

#### 4.2.15.6 那能不能直接改用 Gemmini 内部 DMA 来搬这些数据

这在概念上不是完全不可能，但不能把它当成“现成替代”。

原因是 Gemmini 内部 DMA 的语义是：

- 面向 Gemmini 的 `mvin/mvout`
- 面向 scratchpad / accumulator 行格式
- 带着 Gemmini 自己的行宽、mask、row offset、block 语义

而当前 CoupledDMA 路径的语义是：

- 面向通用的 shared-SPM page copy
- 面向任意 page list / export / fixed-load
- 把它当成一个更通用的 copy engine 在用

所以如果要“改用 Gemmini DMA 代替 CoupledDMA”：

- 不是简单替换底层函数调用
- 而是要把 runtime 的 page-copy 语义，重新映射成 Gemmini 的 mvin/mvout 语义
- 同时还要考虑 manager 占用、Gemmini 计算与搬运的调度冲突

因此这更像：

- 一条架构重构路线

而不是：

- 一个现成的软件小修复

#### 4.2.15.7 把这件事压缩成一句最有用的话

Gemmini 内部 DMA 可以作为参考，但它给出的参考不是：

- “当前软件还能怎么绕一下”

而是：

- **“如果你真的想零拷贝处理 misaligned，底层 DMA 至少要有 Gemmini DMA 这种对齐读、内部重组、掩码写的能力。”**

换句话说：

- Gemmini DMA 证明了这件事在系统层面是可以做到的；
- 但它同时也说明，当前 CoupledDMA 之所以做不到，不是因为软件写得不够花，而是因为底层 primitive 本来就不一样。

### 4.3 第三层原因：当前 runtime 采用的是保守 guardrail，而不是极限最优策略

从硬件源码能直接确认的一件事是：

- `GemminiCoupledDMA.scala` 里硬件会根据当前地址是否对齐 `beatBytes`
  来决定发 wide copy 还是 byte copy；
- 它并不存在一条叫做 `bounce` 的硬件路径。

但 runtime 侧采用的是更保守的规则：

- 不是只看 `beatBytes`；
- 而是用 `mod64` 规则先过滤已知危险请求形状。

这说明 runtime 的 bounce 规则不是在追求“最少分支”，而是在追求“当前平台上更稳”。

## 5. bounce path 的核心原理

一句话说，bounce path 的本质是：

- 在 host 内存里准备一个 stage-local 的中转页；
- 让这个中转页里的起始偏移，故意和非 host 一侧的地址具有相同的 `mod64`；
- 这样真正发给硬件的那笔 DMA，请求两端的 `mod64` 就一致了。

你可以把它想成：

- 原始 host buffer 的“起点残差”不合适；
- 那就换一个 host buffer；
- 并把这个新 buffer 的起点挪到一个“残差合适”的位置。

### 5.1 bounce buffer 是怎么分配出来的

当前 helper 是：

- `dma_stage_bounce_ensure(...)`

它做的事是：

1. 发现当前 stage 还没有 bounce buffer。
2. 取 host page 大小 `host_page_bytes`。
3. 用 `posix_memalign(&buf, host_page_bytes, host_page_bytes)` 分配一整页、页对齐的 host buffer。
4. 用 `dma_prefault_and_lock_buffer(...)` 先逐页触碰，再 `mlock(...)`。
5. 把结果保存到当前 action 的：
   - `exec->stage_dma_bounce[stage_idx]`
   - `exec->stage_dma_bounce_bytes[stage_idx]`

这里有两个重要点：

- 它是 stage-local 的，不是全局共享一个 bounce page；
- 它是 action-private 的，因为这些状态挂在 `prt_action_exec_t` 下面。

也就是说，不同 action 不共享同一个 bounce buffer。

### 5.2 为什么要 prefault + mlock

因为 runtime 后面要拿这个 host buffer 去做 DMA source 或 destination。

如果不先处理：

- 页面可能还没真正分配出来；
- 也可能在 DMA 期间被换出；
- `virt_to_phys` 的时机和结果也会更不稳定。

所以当前实现会先：

- 逐页写一个字节，确保物理页真正 materialize；
- 再 `mlock`，尽量把这段内存固定住。

这与 completion flag pool 的处理思路是一致的。

### 5.3 bounce buffer 的“对齐修正”是怎么做的

核心 helper 是：

- `dma_stage_bounce_region(rt, stage_idx, align_mod64, &bounce_ptr, &bounce_room)`

它做的事情非常简单：

```c
*out_ptr = exec->stage_dma_bounce[stage_idx] + align_mod64;
*out_room = exec->stage_dma_bounce_bytes[stage_idx] - align_mod64;
```

也就是：

- bounce page 的基址本身是页对齐的，因此 `mod64 == 0`；
- 再加上一个 `align_mod64` 偏移；
- 就得到一个新的 `bounce_ptr`；
- 这个 `bounce_ptr` 的 `mod64` 恰好等于 `align_mod64`。

然后 runtime 会把 `align_mod64` 选成“非 host 一侧地址的 `mod64`”。

于是最终效果就是：

- 真正参与 DMA 的 host 端地址残差
  被改造成
  和另一端完全一致。

## 6. host -> SPM 时，bounce 是怎么走的

对应函数：

- `dma_copy_host_to_spm_pages_linux(...)`

每个 chunk 的主流程是：

1. 先根据 host page 边界切 chunk。
2. 计算这次 chunk 的：
   - `src_ptr`
   - `dst_addr`
   - `chunk`
3. 调 `dma_chunk_needs_bounce(src_ptr, dst_addr, chunk)`。

如果不需要 bounce：

1. 对 `src_ptr` 做 `virt_to_phys`，得到 `src_pa`
2. 提交 DMA：
   - `req.src_addr = src_pa`
   - `req.dst_addr = dst_addr`
   - `req.bytes = chunk`

如果需要 bounce：

1. 调 `dma_stage_bounce_region(...)`
2. 这里传进去的 `align_mod64` 是 `dst_addr mod 64`
3. 得到 `bounce_ptr`
4. 先 `memcpy(bounce_ptr, src_ptr, chunk)`
5. 再对 `bounce_ptr` 做 `virt_to_phys`，得到 `src_pa`
6. 最终提交 DMA：
   - `req.src_addr = bounce buffer 的物理地址`
   - `req.dst_addr = 原始 SPM 目标地址`
   - `req.bytes = chunk`

为什么这里用 `dst_addr mod 64`？

因为 host -> SPM 时，DMA 的 source 在 host 侧。
我们能自由调整的是 host 侧这个中转 buffer 的起点残差。
所以要把它调成和 SPM 目标地址一致。

## 7. SPM -> host 时，bounce 是怎么走的

对应函数：

- `dma_copy_spm_pages_to_host_linux(...)`

每个 chunk 的主流程类似，但方向反过来了。

如果不需要 bounce：

1. 对 `dst_ptr` 做 `virt_to_phys`，得到 `dst_pa`
2. 提交 DMA：
   - `req.src_addr = src_addr`
   - `req.dst_addr = dst_pa`
   - `req.bytes = chunk`

如果需要 bounce：

1. 调 `dma_stage_bounce_region(...)`
2. 这里传进去的 `align_mod64` 是 `src_addr mod 64`
3. 得到 `bounce_ptr`
4. 对 `bounce_ptr` 做 `virt_to_phys`，得到 `dst_pa`
5. 先提交 DMA：
   - `req.src_addr = 原始 SPM 地址`
   - `req.dst_addr = bounce buffer 的物理地址`
   - `req.bytes = chunk`
6. DMA 完成后，再 `memcpy(dst_ptr, bounce_ptr, chunk)`

为什么这里改成用 `src_addr mod 64`？

因为 SPM -> host 时，DMA 的 destination 在 host 侧。
我们此时能调整的是 host 端这个 bounce buffer 的目标地址残差。
所以要把它调成和 SPM 源地址一致。

## 8. 这条路径对硬件来说长什么样

从硬件视角看，并没有“bounce 模式”这个额外协议。

硬件最终看到的仍然只是普通 DMA 请求：

- `src_addr`
- `dst_addr`
- `bytes`

区别只在于：

- direct path 提交的是原始 host chunk 的物理地址；
- bounce path 提交的是中转页里某个偏移位置的物理地址。

对硬件而言，这两者没有语义差别。

也正因为如此，bounce 可以理解为：

- “软件在提交前重写了 host 端参与 DMA 的地址选择”

而不是：

- “硬件多实现了一条中转 copy engine”

## 9. direct path 不是被废弃了

这是另一个常见误解。

当前实现不是“DMA 现在改成主要靠 bounce”。
更准确地说：

- direct path 仍然是默认路径；
- bounce path 只是对特定 misaligned chunk 的修正。

只要某个 chunk：

- 小于 `64B`
  或
- 两端 `mod64` 相同

它就直接走 normal direct path。

所以如果你看到程序里有 bounce 支持，不要把它理解成：

- “系统已经不信任 direct path”

更接近真实情况的理解是：

- “系统仍然依赖 direct path；
  只是对已知危险形状补了一层软件护栏。”

## 10. 与当前硬件实现的关系

当前可以明确区分两层：

### 10.1 runtime 层

runtime 做的是：

- host page 分 chunk
- `virt_to_phys`
- misalignment guardrail
- bounce buffer 分配与复用

### 10.2 CoupledDMA / DirectDMA 硬件层

硬件做的是：

- 真正收下地址和长度
- 发 TileLink 读写
- 在内部根据地址对齐情况决定 wide 还是 byte 方式推进 copy

例如在 `GemminiCoupledDMA.scala` 里，源码直接写着：

- `canWide = srcBeatAligned && dstBeatAligned && curRemaining >= wideBytes`
- 否则会退化成 `1B` 级别推进

但这里并没有一个软件可见的 “bounce mode”。

因此，当前 `mod64` bounce 规则应该理解为：

- runtime 针对 Linux host 路径建立的保守对齐 guardrail；
- 而不是硬件接口本身自带的显式模式位。

## 11. 你应该怎么读代码

如果你想顺着源码把这个机制彻底看明白，建议按下面顺序读。

### 第一步：先看判定规则

读：

- `pipeline-runtime/src/prt_dma.c`

先定位：

- `dma_chunk_needs_bounce(...)`
- `dma_stage_bounce_ensure(...)`
- `dma_stage_bounce_region(...)`

把下面三件事先看明白：

1. 什么时候判定为 bounce。
2. bounce page 什么时候分配。
3. bounce_ptr 的偏移是怎么选出来的。

### 第二步：看 host -> SPM 路径

继续读同一个文件里的：

- `dma_copy_host_to_spm_pages_linux(...)`

重点看：

1. `bounce_needed` 在哪里算。
2. direct path 对谁做 `virt_to_phys`。
3. bounce path 为什么先 `memcpy` 到 bounce，再对 bounce 做 `virt_to_phys`。

### 第三步：看 SPM -> host 路径

再读：

- `dma_copy_spm_pages_to_host_linux(...)`

重点对照：

1. 为什么这次 `align_mod64` 改成取 `src_addr mod 64`。
2. 为什么 DMA 完成后还要再做一次 `memcpy(dst_ptr, bounce_ptr, chunk)`。

### 第四步：看 host `virt_to_phys`

读：

- `pipeline-runtime/src/prt_page_table.c`

重点看：

- `prt_host_virt_to_phys(...)`

把下面一点理解透：

- direct 和 bounce 最终都不是把 host 虚拟地址直接喂给硬件；
- 它们都必须先转成物理地址；
- 区别只是转的是“原始 host chunk”还是“bounce chunk”。

### 第五步：看状态挂在哪里、何时释放

读：

- `pipeline-runtime/include/prt_runtime.h`
- `pipeline-runtime/src/prt_action_exec.c`

重点看：

- `stage_dma_bounce[]`
- `stage_dma_bounce_bytes[]`

以及 action 销毁时的 `free(...)`。

这样你会明白：

- bounce buffer 是 action-private、stage-local 的执行态资源；
- 不是某个全局单例。

### 第六步：最后再回头看硬件

读：

- `generators/gemmini/src/main/scala/gemmini/GemminiCoupledDMA.scala`

这一步的目的不是找 `bounce`，而是确认：

- 硬件里没有软件意义上的 bounce path；
- 硬件只是在根据地址对齐决定如何发起真正的 copy。

## 12. 最后给一个最实用的心智模型

你可以把这套机制记成下面三句话：

1. 只要一端是 Linux host buffer，就必须按 host page 分 chunk，并对每个 chunk 单独 `virt_to_phys`。
2. 如果某个大 chunk 出现 `src_mod64 != dst_mod64`，runtime 会优先怀疑这是危险形状，于是改走 bounce。
3. bounce 的本质不是多一次硬件通路，而是软件先换一个“残差合适”的 host buffer，再把它的物理地址交给同一个 DMA 硬件。

## 13. 相关源码与文档

- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_dma.c`
- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_page_table.c`
- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/include/prt_runtime.h`
- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_action_exec.c`
- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/docs/reference/linux_dma_guardrails.md`
- `generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests-coupleddma/rerocc_lc_coverage_linux_coupleddma.c`
- `generators/gemmini/src/main/scala/gemmini/GemminiCoupledDMA.scala`
