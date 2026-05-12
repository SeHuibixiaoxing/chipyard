# Shared SPM 编址导读

这份文档从头梳理 pipeline runtime 里 shared scratchpad memory，也就是 shared SPM，是如何编址的。

重点不是只记一个地址公式，而是把几层地址概念分清：

- 硬件 TileLink 物理地址窗口；
- runtime 里的 shared SPM page 编号；
- action-local alias VA window；
- SPM xlate PTE；
- stage 局部 tensor 布局；
- Gemmini / DMA 最后实际看到的地址。

如果你只记一句话，可以先记这个：

> **shared SPM 的真实硬件地址是 `0x40000000 + ppn * page_bytes + page_offset`；Gemmini 常常不直接看这个地址，而是看 action-local alias VA，再由 SPM xlate 查 PTE 翻译到真实 shared SPM 地址。**

---

## 1. 总览：shared SPM 地址链路

pipeline runtime 里，一个 tensor 进入 Gemmini 之前，地址会经过这些层：

```text
模型 / pipeline artifact
  -> stage 局部布局
     local_spm_tensor_addr
     local_spm_first_vpage
     local_spm_page_count
     exec_base_vpage

runtime action 分配
  -> prt_page_t
     acc_id
     local_page_idx
     ppn

shared SPM 真实物理地址
  -> PRT_SHARED_SPAD_GLOBAL_ADDR_BASE + ppn * page_bytes + page_offset

action-local alias window
  -> alias_base_va + exec_base_vpage * page_bytes + local_spm_tensor_addr

SPM xlate PTE
  -> alias vpage -> shared SPM physical page

Gemmini / Coupled DMA
  -> 访问 alias VA 或 shared SPM physical address
```

这里最重要的是不要把三种“地址”混在一起：

| 地址类别 | 例子 | 谁使用 |
| --- | --- | --- |
| shared SPM 真实物理地址 | `0x40000000 + ppn * 1024` | DMA page helper、PTE 内容、TileLink |
| action-local alias VA | `alias_base_va + ...` | Gemmini 描述符、xlate range |
| stage 局部偏移 | `local_spm_tensor_addr[slot]` | artifact / runtime 组织 stage 视图 |

---

## 2. 硬件全局地址窗口：谁决定 shared SPM 放在哪里

### 2.1 P12 pair-manager 配置里的 shared SPM 参数

当前你关注的配置是：

```text
GemminiLearningConfigSpadReRoCCGlobalNoC4C2x2P12x4x3CoupledDMAPairManagerDummy16x16Sbus128
```

它最终走 `GemminiLearningConfigSpadReRoCCNoCPairManagerParametric`，里面创建：

```scala
gemmini.SharedScratchpadConfig(
  enable = true,
  global_base_addr = BigInt("40000000", 16),
  local_size_bytes = sharedSpadBytes,
  local_banks = 1,
  local_bank_interleaved_bytes = gemminiBeatBytes.max(64),
  local_bank_beat_bytes = gemminiBeatBytes,
  use_page_table_xlate = true,
  share_xlate_with_coupled_dma = true
)
```

对 P12 Sbus128 这版来说，关键值是：

- `global_base_addr = 0x40000000`
- `sharedSpadBytes = 1024 * 1024`
- `numPairs = 12`
- `sbusWidthBits = 16 * 8 = 128`
- `gemminiBeatBytes = 16`
- `local_banks = 1`
- `local_bank_interleaved_bytes = max(16, 64) = 64`
- `local_bank_beat_bytes = 16`
- `use_page_table_xlate = true`
- `share_xlate_with_coupled_dma = true`

所以硬件上，每个 pair / Gemmini manager 都有一段 1 MiB 的 shared SPM 地址窗口。

### 2.2 每个 manager 的硬件物理窗口

`SharedScratchpadConfig` 里定义：

```scala
def local_base_addr(spad_id: Int): BigInt = {
  global_base_addr + local_size_bytes * spad_id
}
```

所以 manager / Gemmini id 为 `i` 时：

```text
local_base_addr(i) = 0x40000000 + 1MiB * i
```

P12 的 12 个 pair manager 对应：

| manager id | shared SPM 物理窗口 |
| --- | --- |
| `0` | `0x40000000` ~ `0x400fffff` |
| `1` | `0x40100000` ~ `0x401fffff` |
| `2` | `0x40200000` ~ `0x402fffff` |
| `...` | `...` |
| `11` | `0x40b00000` ~ `0x40bfffff` |

这是**硬件 TileLink 地址图**，由 Chisel 配置决定，不是 runtime 临时决定的。

### 2.3 bank 地址如何参与

`SharedScratchpadConfig` 还有：

```scala
def local_bank_base_addr(spad_id: Int, bank_id: Int): BigInt = {
  local_base_addr(spad_id) + local_bank_interleaved_bytes * bank_id
}

def local_bank_addr_sets(spad_id: Int): Seq[AddressSet] = {
  (0 until local_banks).map { bank_id =>
    AddressSet(local_bank_base_addr(spad_id, bank_id), local_bank_addr_mask)
  }
}
```

这说明硬件支持多 bank shared SPM，并且可以按 `local_bank_interleaved_bytes` 做 bank interleave。

但在 P12 pair-manager Sbus128 配置里：

```text
local_banks = 1
```

所以当前目标里可以先把 bank 层理解成：

> **每个 manager 只有一个 shared SPM bank，因此 shared SPM 物理地址就是一个连续 1 MiB 窗口。**

多 bank 公式保留在硬件结构里，但当前 P12 配置下不会让你在阅读 runtime 时额外遇到 bank 交错。

---

## 3. runtime 的 page 编号：谁决定 `ppn`

### 3.1 `prt_page_t` 表示什么

runtime 不直接用“字节地址”管理 shared SPM，而是先按页管理。

核心结构是：

```c
typedef struct {
  uint32_t ppn;
  uint32_t acc_id;
  uint32_t local_page_idx;
} prt_page_t;
```

三个字段含义是：

- `acc_id`
  - 这个 page 属于哪个 accelerator / manager 的 shared SPM 区域；
- `local_page_idx`
  - 这个 manager 本地 shared SPM 里的第几页；
- `ppn`
  - runtime 使用的全局 shared SPM 页号。

runtime 里构造 `ppn` 的公式是：

```text
ppn = acc_id * pages_per_acc + local_page_idx
```

这个公式来自 `alloc_pages_from_order()`：

```c
idx = acc * rt->cfg.pages_per_acc + lp;
slot->pages.data[slot->pages.size].acc_id = acc;
slot->pages.data[slot->pages.size].local_page_idx = lp;
slot->pages.data[slot->pages.size].ppn = idx;
```

所以 `ppn` 是软件定义的线性页号，它把二维坐标：

```text
(manager id, manager 内本地页号)
```

压平成：

```text
global page number
```

### 3.2 `page_bytes` 和 `pages_per_acc` 是谁决定的

runtime 配置里有两个关键字段：

```c
cfg.page_size_bytes
cfg.pages_per_acc
```

默认值在 `main.c` 里：

```c
cfg.page_size_bytes = PRT_PAGE_SIZE_BYTES;  // 1024
cfg.pages_per_acc = 256;
```

但 P12 pairdummy 固定 profile 会设置：

```bash
export PAGES_PER_ACC="1024"
```

host closure 脚本也会显式传：

```bash
--pages-per-acc 1024
```

这件事很关键。P12 硬件每个 manager 的 shared SPM 是 1 MiB，而 runtime 默认页大小是 1 KiB：

```text
1 MiB / 1 KiB = 1024 pages
```

所以对 P12 Sbus128 配置，正确的 runtime 建模应该是：

```text
page_bytes = 1024
pages_per_acc = 1024
```

这样：

```text
acc_id * pages_per_acc * page_bytes
  = acc_id * 1024 * 1024
  = acc_id * 1MiB
```

正好与硬件的 `local_base_addr(acc_id)` 对齐。

### 3.3 这里的一个重要约束

runtime 的 shared SPM 物理地址公式是：

```text
paddr = PRT_SHARED_SPAD_GLOBAL_ADDR_BASE + ppn * page_bytes + page_offset
```

结合：

```text
ppn = acc_id * pages_per_acc + local_page_idx
```

可得：

```text
paddr =
  0x40000000
  + acc_id * pages_per_acc * page_bytes
  + local_page_idx * page_bytes
  + page_offset
```

为了让 `acc_id` 真正落到硬件 manager `acc_id` 的 1 MiB 窗口，必须满足：

```text
pages_per_acc * page_bytes == hardware local_size_bytes
```

对 P12：

```text
1024 * 1024B == 1MiB
```

如果误用默认 `pages_per_acc = 256`，那么 `acc_id = 1` 会映射到：

```text
0x40000000 + 256 * 1024 = 0x40040000
```

这仍然在 manager 0 的 1 MiB 窗口里，而不是 manager 1 的窗口。
因此 P12 pair-manager 路线必须确保运行参数与硬件容量一致。

---

## 4. shared SPM 真实物理地址：谁产生它

runtime 中有多处 helper 统一使用同一个公式。

常量定义：

```c
#define PRT_SHARED_SPAD_GLOBAL_ADDR_BASE 0x40000000ULL
```

page 到物理地址的公式：

```c
PRT_SHARED_SPAD_GLOBAL_ADDR_BASE
  + (uint64_t)page->ppn * page_bytes
```

这在几个地方反复出现：

- `prt_schedule_action.c`
  - `action_page_paddr()`
  - 用于日志里打印 `ppn@acc/local/paddr`
- `prt_dma.c`
  - `page_base_addr()`
  - DMA page copy helper 用它生成 shared SPM 源 / 目的地址；
- `prt_page_table.c`
  - `prt_spm_bind_vpages_ctx()`
  - PTE 里写入的也是这个 shared SPM 物理页地址。

所以：

> **shared SPM 真实物理地址不是从 alias VA 直接算出来的，而是先由 runtime page allocator 选出 `prt_page_t`，再由 `ppn` 转成物理地址。**

---

## 5. page 是怎么被分给 tensor / buffer 的

### 5.1 page allocator 的全局 bitmap

`prt_page_table_init()` 会初始化：

```c
total_pages = rt->cfg.num_cores * rt->cfg.pages_per_acc;
rt->page_used = calloc(total_pages, sizeof(uint8_t));
```

这里的 `num_cores` 在 shared SPM allocator 里更像“可分配的 SPM 域数量”。
在 P12 pairdummy runtime profile 里，运行参数通常是：

```text
NUM_CORES = 4
NUM_GEMMINI = 12
NUM_DMA = 12
PAIR_MANAGER_MODE = 1
PAGES_PER_ACC = 1024
```

`prt_runtime_init()` 里还有保护逻辑：

```c
if (rt->cfg.num_cores < rt->cfg.num_gemmini_mgrs) {
  rt->cfg.num_cores = rt->cfg.num_gemmini_mgrs;
}
```

所以虽然环境里 `NUM_CORES=4`，但如果 `num_gemmini_mgrs=12`，allocator 最终会至少按 12 个 SPM 域建模。

### 5.2 `prt_alloc_tensor_pages()` 如何选页

真正分配 page 的函数是：

```c
prt_alloc_tensor_pages(rt, tensor_id, bytes,
                       preferred_accs, preferred_cnt,
                       out_pages)
```

它做几件事：

1. 根据 `bytes` 和 `page_bytes` 算需要多少页：

```text
need_pages = ceil(bytes / page_bytes)
```

2. 如果有 preferred managers，就优先在这些 manager 的页里分；

3. 如果 preferred managers 不够，再按 fallback order 去其它 manager 找；

4. 分配时外层先遍历 `local_page_idx`，内层遍历 manager order：

```text
for lp in 0 .. pages_per_acc-1:
  for acc in order:
    ppn = acc * pages_per_acc + lp
```

代码注释说这是为了保持稳定的 all-bank interleave contract：

```text
bank0:lp0, bank1:lp0, ..., bank0:lp1, ...
```

在 P12 语境里，你可以把这里的 “bank0/bank1” 先理解成不同 manager / accelerator 域，而不是 `SharedScratchpadConfig.local_banks` 那个硬件 bank。

### 5.3 action 层如何形成 page list

如果你想继续追问：

- `buffer_id` 到底是谁编号的
- `slot_id` 是怎么从 `slot_count` 展开出来的
- `pipebuf / ringbuf` 如何把这些 page list 真正接进运行时同步逻辑

建议同时配合阅读：

- [`pipebuffer_management_guide.md`](./pipebuffer_management_guide.md)

`prt_action_alloc_spm()` 会根据 pipeline segment 的 buffer bindings 分配三类 pages：

- `weight_pages`
  - 固定 tensor / 权重；
- `in_stage_pages`
  - stage entry / export pipe buffer；
- `ring_pages`
  - ring buffer 槽位。

对应结构：

```c
typedef struct {
  uint32_t buffer_id;
  uint32_t tensor_id;
  uint32_t stage_id;
  uint32_t slot_id;
  prt_page_list_t pages;
} prt_spm_page_binding_t;
```

可以把 `prt_spm_page_binding_t` 理解成：

> **“某个逻辑对象，在某个运行时槽位上，对应的那组 shared SPM pages。”**

这里最容易混淆的是：

- `tensor_id`
- `buffer_id`
- `slot_id`

它们都在“描述 page 属于谁”，但描述的是不同层次。

#### 5.3.1 page list 是怎么生成出来的

`prt_action_alloc_spm()` 的工作顺序可以直接概括成：

1. 遍历 `segment->buffer_bindings[]`
2. 对每个 binding 按 `slot_count` 循环
3. 每个 slot 调一次 `alloc_slot_pages_for_action(...)`
4. 得到一个 `prt_page_list_t`
5. 再把这组 pages 连同 `buffer_id / tensor_id / stage_id / slot_id`
   一起塞进：
   - `weight_pages`
   - `in_stage_pages`
   - `ring_pages`

也就是说，action 层不是只分配“裸 page”。
它分配的是：

- 一组 `prt_page_t`
- 再加一组“这组 page 在运行时属于谁”的标签

最后形成一个 `prt_spm_page_binding_t`。

#### 5.3.2 `tensor_id`：语义层身份，回答“这是哪块数据”

`tensor_id` 描述的是：

- 模型 / pipeline 语义里的哪一个 tensor；
- 也就是 stage 执行时真正关心的“这是谁的数据”。

它来自更上层的 tensor binding / stage binding：

- `prt_tensor_binding_t.tensor_id`
- `prt_buffer_binding_t.tensor_id`
- `prt_stage_map_t.tensor_ids[]`

所以 `tensor_id` 是**数据语义身份**，不是存储容器身份。

你可以把它理解成：

- `tensor_id` 说的是“这组 pages 里装的是 tensor 17 还是 tensor 23”
- 它不直接说明这些 pages 挂在哪个 pipebuf / ringbuf 上

这也是为什么运行阶段真正取“当前 tensor pages”时，代码主要按 `tensor_id` 查：

- fixed tensor 走 `runtime_find_weight_pages(rt, stage_id, tensor_id)`
- pipe tensor 走 `find_stage_pipebuf(rt, stage_id, tensor_id, is_entry)`

也就是说，执行阶段提问的方式更像：

> **“stage 0 现在要读 tensor 2，它当前对应哪组页？”**

而不是：

> “buffer 137 当前对应哪组页？”

所以如果你只看 `tensor_id`，你看到的是：

- page list 在计算图 / stage 输入输出语义中的身份。

#### 5.3.3 `buffer_id`：运行时容器层身份，回答“这块数据挂在哪个 buffer 上”

`buffer_id` 描述的是：

- 这组 pages 属于哪个 runtime buffer binding；
- 也就是属于哪个 pipe / ring / weight 容器实例。

它来自：

- `prt_buffer_binding_t.buffer_id`

随后会被复制到真正的 runtime 拓扑对象里：

- `prt_pipebuf.buffer_id`
- `prt_ringbuf.buffer_id`
- `prt_spm_page_binding_t.buffer_id`

这一层很关键，因为 runtime 在把 action 分好的 pages 绑定回运行时拓扑时，主要不是靠 `tensor_id` 找，而是靠：

```text
buffer_id + slot_id
```

对应代码是：

```c
find_action_binding(arr, n, buffer_id, slot_id)
```

它只比较：

- `arr[i].buffer_id == buffer_id`
- `arr[i].slot_id == slot_id`

这说明在“把 page list 接回 pipebuf/ringbuf 拓扑”这个步骤里，`buffer_id` 才是主身份。

为什么需要这一层，而不能只靠 `tensor_id`？

因为：

- 同一个 `tensor_id` 可能同时出现在不同 stage 的 entry / export；
- 同一个 shared tensor 还可能因为 `alias_group_id` 被多个 pipe binding 复用；
- runtime 真正管理的是“缓冲对象”，而不只是“张量名字”。

所以你可以把 `buffer_id` 理解成：

> **tensor 的“存储/传输容器编号”。**

如果说 `tensor_id` 回答“是谁的数据”，那 `buffer_id` 回答的是：

> **“这份数据当前挂在哪个 runtime buffer 实例上。”**

#### 5.3.4 `slot_id`：容器内部槽位层身份，回答“这个 buffer 的哪一份 backing pages”

`slot_id` 描述的是：

- 同一个 buffer 内部的第几个 page-set；
- 也就是这个容器的“槽位编号”。

这是因为一个 buffer 往往不只对应一组页。

最常见的三种情况：

1. `WEIGHT`
   - 固定 tensor 通常只有一组 backing pages
   - 所以 `slot_id` 基本固定为 `0`
2. `PIPE`
   - 如果开启 double buffer，就会有 `slot 0 / slot 1`
   - 最终落到 `prt_pipebuf.slot_pages[0/1]`
3. `RING`
   - 一个 ring buffer 有多个环槽
   - `slot_id` 就是 ring 的槽号
   - 最终落到 `prt_ringbuf.slot_pages[slot]`

所以 `slot_id` 不是“模型里第几个 tensor slot”，而是：

> **同一个 runtime buffer 容器里的第几个后备页组。**

这也是为什么 action 分配 pages 时，要按 `slot_count` 循环；
因为真正被分配的不是一个 buffer 对应一组页，而是：

```text
一个 buffer
  -> 多个 slot
     -> 每个 slot 一组 pages
```

运行时切换活跃 backing pages 时，最终就是在这些 `slot_pages[]` 之间切：

- pipebuf 用 `in_use_idx / no_use_idx`
- ringbuf 用 `subbatch_offset % ring->size`

#### 5.3.5 `stage_id`：执行视角的作用域，不是主要存储主键

虽然你这次主要问的是 `tensor_id / buffer_id / slot_id`，但还需要补一句 `stage_id` 的位置：

- `stage_id` 说明这组 pages 是从哪个 stage 视角分配出来的；
- 对 `WEIGHT` 和 `PIPE` 它是有意义的；
- 对 `RING`，当前代码会写成 `UINT32_MAX`，因为 ring 是 segment 级 transport 对象，不是某个单独 stage 私有。

所以：

- `stage_id` 更像“执行作用域”
- `tensor_id / buffer_id / slot_id` 才是“这组 pages 的具体身份标签”

#### 5.3.6 把三层身份合起来看

如果只用一句话概括三者的分工，可以记成：

- `tensor_id`
  - 语义层：这是哪一个 tensor
- `buffer_id`
  - 容器层：这份 tensor 当前挂在哪个 runtime buffer 对象上
- `slot_id`
  - 槽位层：这个 buffer 当前/可切换的哪一组 backing pages

也就是：

```text
tensor_id  = “谁的数据”
buffer_id  = “挂在哪个容器上”
slot_id    = “容器里的哪一份页”
```

#### 5.3.7 一个最实用的阅读视角

后面读代码时，你可以把 page list 的身份分成两种“查找问题”：

1. action 绑定阶段
   - 重点看 `buffer_id + slot_id`
   - 因为这里是在把 page list 接回 `pipebuf.slot_pages[] / ringbuf.slot_pages[]`
2. stage 执行阶段
   - 重点看 `stage_id + tensor_id`
   - 因为这里是在问“当前这个 stage 看到的这个 tensor，实际应取哪组 pages”

这两种查找方式同时存在，正是因为：

- action 分配关心“页分给哪个运行时容器”
- stage 执行关心“当前算子语义上这块 tensor 对应哪组页”

如果把这两层混在一起，就很容易误以为：

- `tensor_id` 就足够唯一确定一组 pages

但实际代码并不是这样设计的。

---

## 6. action-local alias window：谁决定 Gemmini 看到的虚拟地址范围

### 6.1 为什么需要 alias window

pipeline runtime 希望 Gemmini 的输入 / 权重 / 输出地址看起来像普通连续指针。

但实际 shared SPM pages 可能是：

- 分散在不同 manager 里；
- 同一个 logical tensor 跨多个 pages；
- 不同 subbatch / ring slot 会换当前活跃 pages。

因此 runtime 给每个 action 建一个 alias VA window：

```text
alias_base_va ... alias_base_va + alias_bytes
```

Gemmini 访问 alias VA，硬件 xlate 查 PTE，再转到真实 shared SPM physical page。

### 6.2 alias window 是怎么产生的

`prt_action_alloc_spm()` 里先算：

```c
page_count = seg->segment_spm_page_span;
if (page_count == 0) {
  for each stage:
    page_count += stage.local_spm_page_span;
}
action->alias_page_count = page_count;
```

然后如果启用 xlate：

```c
alloc_action_alias_window(rt, action, page_count);
prt_spm_xlate_ctx_alloc(rt, &action->spm_xlate, page_count);
```

Linux 下 `alloc_action_alias_window()` 用：

```c
mmap(NULL, alloc_bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)
```

这说明 alias window 本身不是一段普通可读写内存。它只是保留一段 guest 虚拟地址范围，让 Gemmini / DMA 侧识别：

> **这个 VA 落在 xlate range 内，应该走 shared SPM xlate。**

### 6.3 alias window 的大小

alias window 大小来自：

```text
alias_bytes = alias_page_count * page_bytes
```

其中 `alias_page_count` 主要来自 pipeline artifact 里的：

- `segmentSpmPageSpan`
- 或每个 stage 的 `localSpmPageSpan` fallback。

也就是说：

- hardware 决定 shared SPM 物理空间多大；
- pipeline artifact 决定一个 action 的执行视图需要多少 alias pages；
- runtime 按这个 page_count 分配 alias VA window 和 PTE 表。

---

## 7. SPM xlate PTE：alias VA 如何翻译到 shared SPM 物理地址

### 7.1 PTE 表是谁分配的

`prt_spm_xlate_ctx_alloc()` 为当前 action 创建 xlate context：

```c
ctx->pte = ...
ctx->pte_count = page_count;
ctx->ptbr_pa = ctx->pt_chunk->base_pa + ctx->pt_slice_offset;
ctx->free_vpages[0].start = 0;
ctx->free_vpages[0].count = page_count;
```

这里有两个地址：

- `ctx->pte`
  - 软件写 PTE 用的虚拟地址；
- `ctx->ptbr_pa`
  - 硬件 page table walker 读 PTE 用的物理地址。

PTE 表是 action-local 的。不同 action 有不同的：

- alias window；
- PTE backing；
- PTBR；
- PTE count。

### 7.2 PTE 格式

runtime 打包 PTE 的函数是：

```c
static uint64_t pack_spm_pte(const prt_runtime_t *rt, uint64_t paddr) {
  uint32_t page_shift = rt && rt->cfg.spm_page_shift ? rt->cfg.spm_page_shift : 10U;
  return ((paddr >> page_shift) << 1) | PRT_SPM_PTE_VALID_MASK;
}
```

所以 PTE 格式可以理解为：

```text
bit 0      : valid
bits 63:1  : physical page number, 即 paddr >> page_shift
```

硬件恢复物理页基址时做：

```text
paddr_base = (pte[63:1] << page_shift)
```

### 7.3 硬件如何查 PTE

在 `FrontendTLB.scala` 里，shared SPM xlate 的核心计算是：

```scala
spmOffset = reqVaddr - spm_xlate_range_base
spmVpn = spmOffset >> spmPageShift
spmPageOffset = spmOffset - (spmVpn << spmPageShift)
```

当 `use_page_table_xlate = true` 且 `spm_xlate_enable = true` 时：

1. 检查 `reqVaddr` 是否落在：

```text
[range_base, range_base + range_size)
```

2. 用：

```text
vpn = (reqVaddr - range_base) >> page_shift
```

3. page table walker 访问：

```text
ptbr + vpn * 8
```

4. 读出 PTE，得到：

```text
paddr = (pte[63:1] << page_shift) + page_offset
```

这和软件写 PTE 的方式正好对应。

### 7.4 xlate context 是怎么安装到 manager 的

`prt_action_bind_topology()` 最后调用 `configure_action_spm_xlate()`。

它会对当前 action 涉及的每个 Gemmini manager 执行：

```c
prt_gemmini_spm_xlate_program(manager_id,
                              action->spm_xlate.ptbr_pa,
                              action->spm_xlate.pte_count,
                              rt->cfg.spm_page_shift,
                              action->alias_base_va,
                              action->alias_bytes,
                              1U);
```

这一步给硬件写入：

- `ptbr`
- `pte_count`
- `page_shift`
- `range_base = alias_base_va`
- `range_size = alias_bytes`
- `enable = 1`

注意：`shared_base` 不是软件传进去的。硬件里它来自：

```scala
outer.config.shared_scratchpad_config.global_base_addr
```

也就是 P12 配置中的 `0x40000000`。

---

## 8. stage 局部布局：谁决定一个 tensor 在 alias window 里的位置

### 8.1 artifact 中的四个核心字段

每个 stage 里，runtime 关心这几个字段：

```c
stage->exec_base_vpage
stage->local_spm_tensor_addr[slot]
stage->local_spm_first_vpage[slot]
stage->local_spm_page_count[slot]
stage->local_spm_tensor_bytes[slot]
```

它们主要来自 pipeline YAML / Gemmini layer mapping artifact：

- `execBaseVPage`
- `localSpmTensorAddrList`
- `localSpmFirstVPageList`
- `localSpmPageCountList`
- `localSpmTensorBytesList`
- `localSpmPageSpan`

`prt_yaml_loader.c` 负责从 YAML 读这些字段。
`prt_gemmini_artifacts.c` 也会把 layer mapping entry 应用到 `prt_stage_map_t`。

### 8.2 这些字段分别代表什么

| 字段 | 含义 | 决定者 |
| --- | --- | --- |
| `exec_base_vpage` | 这个 stage 在 action alias window 里的起始虚页 | pipeline artifact |
| `local_spm_first_vpage[slot]` | 这个 tensor 在 stage 局部 SPM 视图里的起始页 | layer mapping / pipeline artifact |
| `local_spm_tensor_addr[slot]` | 这个 tensor 在 stage 局部 SPM 视图里的字节偏移 | layer mapping / pipeline artifact |
| `local_spm_page_count[slot]` | 这个 tensor 需要多少 SPM 页 | layer mapping / pipeline artifact |
| `local_spm_tensor_bytes[slot]` | 这个 tensor 的精确字节数 | layer mapping / pipeline artifact |

通常：

```text
local_spm_tensor_addr ~= local_spm_first_vpage * page_bytes
```

但代码不会要求你只靠这个等式推导；artifact 会显式给出 byte address 和 vpage 信息，runtime 会分别使用。

### 8.3 Gemmini 描述符里最终放什么地址

当 `spm_xlate_enable = 1` 时，`stage_tensor_exec_addr()` 返回：

```text
action->alias_base_va
  + stage->exec_base_vpage * page_bytes
  + stage->local_spm_tensor_addr[slot]
```

这就是 Gemmini 描述符里看到的 `input / weights / bias / output` 指针。

所以从 Gemmini 的角度看：

> **它拿到的是一个普通-looking VA；但这个 VA 落在 action alias window 里，会被 SPM xlate 翻译成 shared SPM 物理地址。**

---

## 9. stage 运行前：谁把当前 pages 写进 PTE

### 9.1 为什么 stage 前还要刷新 PTE

action 分配阶段只回答：

> **哪些 shared SPM pages 属于哪些 buffer / tensor / slot？**

但 stage 真正运行时还要回答：

> **这一轮 stage 当前应该看到哪一组 pages？**

这取决于：

- fixed tensor；
- entry pipebuf；
- export pipebuf；
- double buffer 当前 `in_use_idx`；
- ring buffer 当前 `subbatch_offset`。

所以 stage 开跑前，runtime 会在 `stage_prepare_exec_views()` 里刷新 PTE。

### 9.2 当前 tensor pages 怎么确定

`stage_tensor_current_pages()` 的规则是：

1. fixed tensor：

```text
runtime_find_weight_pages(rt, stage_id, tensor_id)
```

从 `exec->topo_weight_pages` 找。

2. entry tensor：

```text
find_stage_pipebuf(rt, stage_id, tensor_id, 1)
```

默认取：

```text
buf->slot_pages[buf->in_use_idx]
```

3. export tensor：

```text
find_stage_pipebuf(rt, stage_id, tensor_id, 0)
```

同样默认取：

```text
buf->slot_pages[buf->in_use_idx]
```

4. all-ring fallback：

如果是 `PRT_BUF_C7_ENTRY_ALL_RING` 或 `PRT_BUF_C8_EXPORT_ALL_RING`，并且当前 direct slot 没有 pages，则取：

```text
buf->ring->slot_pages[buf->subbatch_offset % buf->ring->size]
```

### 9.3 PTE 绑定的 vpage 起点

找到当前 pages 后，runtime 计算：

```text
exec_vpage = stage->exec_base_vpage + stage->local_spm_first_vpage[slot]
```

然后调用：

```c
prt_spm_bind_vpages_ctx(rt,
                        &action->spm_xlate,
                        exec_vpage,
                        &view,
                        stage->local_spm_page_count[slot])
```

这个调用会把：

```text
PTE[exec_vpage + i]
```

写成：

```text
valid | ((0x40000000 + pages[i].ppn * page_bytes) >> page_shift)
```

绑定结束后，runtime 调：

```text
runtime_flush_stage_spm_xlate(rt, stage_id)
```

让 stage 对应的 Gemmini managers 看到最新 PTE。

---

## 10. DMA 和 Gemmini 对 shared SPM 地址的使用差异

### 10.1 Gemmini 主计算路径

Gemmini 主计算路径通常使用 alias VA：

```text
alias_base_va
  + exec_base_vpage * page_bytes
  + local_spm_tensor_addr
```

硬件 TLB / xlate 把它翻译到 shared SPM physical address。

### 10.2 DMA page helper

pipeline runtime 里的 DMA page copy helper 往往直接基于 `prt_page_list_t` 生成真实 shared SPM 物理地址：

```text
page_base_addr(rt, &pages->data[i])
  = 0x40000000 + ppn * page_bytes
```

例如：

- `prt_dma_copy_dram_to_spm_pages_prefix()`
- `prt_dma_copy_spm_pages_to_dram_prefix()`
- `prt_dma_copy_spm_pages_prefix()`

它们逐页提交 DMA request，每页最多拷 `page_bytes`。

### 10.3 Coupled DMA 也能共享 xlate

硬件配置里：

```text
share_xlate_with_coupled_dma = true
```

因此 Coupled DMA 侧也接收同一套 `SharedSpadXlateConfig`。

在 `GemminiCoupledDMA.scala` 里，DMA 对 `src` 和 `dst` 都会检查：

```text
addr in [range_base, range_base + range_size)
```

如果命中 alias range，就可以按同一套 PTE 做翻译。

但要注意 pipeline runtime 当前很多 page-copy helper 已经直接拿 `ppn` 生成 shared SPM physical address；这些路径不需要先构造 alias VA。

所以可以这样理解：

- Gemmini compute 描述符主线：偏向 alias VA；
- runtime DMA page helper：偏向真实 shared SPM physical address；
- Coupled DMA 硬件能力：支持 alias VA xlate，但是否使用取决于软件传入的地址。

---

## 11. P12 pair-manager 的完整例子

假设：

```text
manager id / acc_id = 5
page_bytes = 1024
pages_per_acc = 1024
local_page_idx = 7
page_offset = 128
```

runtime 先算：

```text
ppn = 5 * 1024 + 7 = 5127
```

真实 shared SPM 物理地址是：

```text
paddr = 0x40000000 + 5127 * 1024 + 128
      = 0x40501c80
```

也可以按硬件窗口理解：

```text
manager 5 base = 0x40000000 + 5 * 1MiB
               = 0x40500000

local page 7 base = 7 * 1024
                  = 0x1c00

paddr = 0x40500000 + 0x1c00 + 0x80
      = 0x40501c80
```

如果某个 stage tensor slot 又有：

```text
alias_base_va = 0x7f0000000000
exec_base_vpage = 32
local_spm_first_vpage[slot] = 4
local_spm_tensor_addr[slot] = 4096
local_spm_page_count[slot] = 3
```

Gemmini 描述符会拿到：

```text
tensor_alias_va =
  0x7f0000000000
  + 32 * 1024
  + 4096
```

PTE 绑定会从：

```text
exec_vpage = 32 + 4 = 36
```

开始写 3 项：

```text
PTE[36] -> pages[0] 的 shared SPM physical page
PTE[37] -> pages[1] 的 shared SPM physical page
PTE[38] -> pages[2] 的 shared SPM physical page
```

Gemmini 访问 `tensor_alias_va` 时：

```text
vpn = (tensor_alias_va - alias_base_va) >> 10
```

会命中 `PTE[36]`，最终落到对应的 shared SPM physical page。

---

## 12. 每一部分由谁决定

| 部分 | 典型字段 / 公式 | 决定者 | 产生位置 |
| --- | --- | --- | --- |
| shared SPM 全局基址 | `0x40000000` | 硬件配置 | `SharedScratchpadConfig.global_base_addr` |
| 每个 manager 窗口大小 | `1MiB` | 硬件配置 | `sharedSpadBytes` / `local_size_bytes` |
| manager 窗口起点 | `base + local_size * id` | 硬件配置 | `local_base_addr(spad_id)` |
| runtime 页大小 | `page_size_bytes` | runtime 参数 | `main.c` / `--spm-page-bytes` |
| xlate page shift | `log2(page_size_bytes)` | runtime 初始化 | `prt_runtime_init()` |
| 每 manager 页数 | `pages_per_acc` | runtime 参数 / profile | `--pages-per-acc` |
| runtime 全局页号 | `acc_id * pages_per_acc + local_page_idx` | runtime allocator | `alloc_pages_from_order()` |
| shared SPM 物理页地址 | `0x40000000 + ppn * page_bytes` | runtime helper | `page_base_addr()` / `prt_spm_bind_vpages_ctx()` |
| action alias base | `alias_base_va` | runtime mmap | `alloc_action_alias_window()` |
| action alias size | `alias_page_count * page_bytes` | pipeline artifact + runtime | `prt_action_alloc_spm()` |
| PTE 表 | `ctx->pte` / `ctx->ptbr_pa` | runtime xlate context | `prt_spm_xlate_ctx_alloc()` |
| stage 起始虚页 | `exec_base_vpage` | pipeline artifact | `prt_yaml_loader.c` |
| tensor 局部偏移 | `local_spm_tensor_addr` | layer mapping / pipeline artifact | `prt_gemmini_artifacts.c` / `prt_yaml_loader.c` |
| tensor 局部起始页 | `local_spm_first_vpage` | layer mapping / pipeline artifact | `prt_gemmini_artifacts.c` / `prt_yaml_loader.c` |
| tensor 页数 | `local_spm_page_count` | layer mapping / pipeline artifact | `prt_gemmini_artifacts.c` / `prt_yaml_loader.c` |
| Gemmini tensor 指针 | `alias_base + exec_base * page + local_addr` | runtime | `stage_tensor_exec_addr()` |
| stage 前 PTE 绑定 | `PTE[exec_base + first_vpage + i]` | runtime 当前执行态 | `stage_prepare_exec_views()` |

---

## 13. 阅读代码建议顺序

如果你想对照代码看，建议按这个顺序：

1. `generators/gemmini/src/main/scala/gemmini/SharedScratchpad.scala`
   - 先看硬件全局地址窗口怎么定义；
   - 重点看 `global_base_addr`、`local_size_bytes`、`local_base_addr()`。

2. `generators/chipyard/src/main/scala/config/GemminiLearningReRoCCPairManagerConfigs.scala`
   - 看 P12 配置实际给 shared SPM 填了哪些参数；
   - 重点看 `sharedScratchpadConfig`。

3. `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/include/prt_types.h`
   - 看 `PRT_SHARED_SPAD_GLOBAL_ADDR_BASE`；
   - 看 `prt_page_t` 和 `prt_stage_map_t`。

4. `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_page_table.c`
   - 看 `prt_page_table_init()`；
   - 看 `alloc_pages_from_order()`；
   - 看 `prt_alloc_tensor_pages()`；
   - 看 `pack_spm_pte()`；
   - 看 `prt_spm_bind_vpages_ctx()`。

5. `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_schedule_action.c`
   - 看 `alloc_action_alias_window()`；
   - 看 `prt_action_alloc_spm()`；
   - 看 `configure_action_spm_xlate()`。

6. `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_runtime.c`
   - 看 `stage_prepare_exec_views()`；
   - 看 `stage_tensor_current_pages()`；
   - 看 `stage_tensor_exec_addr()`。

7. `generators/gemmini/src/main/scala/gemmini/FrontendTLB.scala`
   - 看硬件如何从 `range_base`、`page_shift`、`ptbr` 查 PTE；
   - 对照软件的 `pack_spm_pte()`。

8. `generators/gemmini/src/main/scala/gemmini/GemminiCoupledDMA.scala`
   - 看 Coupled DMA 如何共享同一套 xlate 配置。

---

## 14. 常见误解

### 14.1 `acc_id` 不是 CPU core id

在 page allocator 里，`acc_id` 表示 shared SPM 分配域 / accelerator manager id。
P12 pair-manager 模式下，它应该对应 pair manager / Gemmini manager id。

### 14.2 `pages_per_acc` 不是硬件自动告诉 runtime 的

它是 runtime 参数。
P12 要让软件页号和硬件 1 MiB stride 对齐，就要用：

```text
pages_per_acc = 1024
page_bytes = 1024
```

### 14.3 alias VA 不是 shared SPM 物理地址

alias VA 是给 xlate 看的虚拟窗口。
PTE 才把 alias VA 的 vpage 指向真实 shared SPM physical page。

### 14.4 PTE index 从 `range_base` 开始算

硬件用：

```text
vpn = (vaddr - range_base) >> page_shift
```

所以 action-local alias window 里，PTE 0 对应的是：

```text
alias_base_va + 0 * page_bytes
```

不是某个全局虚页编号。

### 14.5 `local_spm_tensor_addr` 和 `local_spm_first_vpage` 都重要

`local_spm_tensor_addr` 用于生成 Gemmini 描述符里的 alias pointer。
`local_spm_first_vpage` 用于决定从哪个 PTE slot 开始绑定 pages。

两者通常一致对应，但 runtime 分别使用，阅读时不要只看其中一个。
