# SPM xlate 用法与接口 API 导读

这份文档解释 shared scratchpad xlate 在软件看来怎么用。它不从硬件页表实现开始讲，而是围绕“软件如何构造一段 alias VA，并让 Gemmini / Coupled DMA 按这段 VA 访问 shared scratchpad”来读。

配合阅读：

- `generators/gemmini/software/gemmini-rocc-tests/include/rerocc_gemmini_spm_xlate.h`
- `generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests-coupleddma/rerocc_linux_spm_xlate.h`
- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/include/prt_page_table.h`
- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_page_table.c`
- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/include/prt_rerocc.h`
- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_rerocc.c`
- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_schedule_action.c`
- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_runtime.c`
- `docs/software_rerocc_api_guide.md`

---

## 1. SPM xlate 解决什么问题

pipeline runtime 希望 Gemmini 看到的输入、权重、输出地址像普通指针一样连续，但数据实际可能在 shared scratchpad 的不同页里。

SPM xlate 做的就是这件事：

- 软件建立一张 PTE 表；
- 软件选择一段 alias virtual address window；
- 软件把 alias window 的每个 virtual page 映射到 shared scratchpad 的 physical page；
- Gemmini / DMA 访问 alias VA 时，硬件根据 PTE 翻译到真实 shared scratchpad 地址。

直觉上可以把它当成一个很小的、专门给 shared scratchpad 用的软件管理页表。

---

## 2. 最底层指令 API

入口文件：

- `generators/gemmini/software/gemmini-rocc-tests/include/rerocc_gemmini_spm_xlate.h`

SPM xlate 使用 `XCUSTOM_ACC = 3`，也就是 `custom3`。

这里一定要先建立一个正确心智模型：

- SPM xlate **不是**一套独立于 Gemmini 的新 opcode；
- 它和 Gemmini 共用的就是 `custom3`；
- 二者的区别主要不是靠 opcode，而是靠 **`funct` 字段**。

更准确地说，解码分两层：

1. **pair wrapper / RoCC 入口层**
   - 只看 opcode
   - `custom2` 送 DMA
   - `custom3` 送 Gemmini 这一侧

2. **Gemmini controller 内部**
   - 再看 `funct`
   - `funct = 23/24/25/26` 解释成 SPM xlate 控制
   - 其它 Gemmini `funct` 继续解释成普通 Gemmini 指令

所以：

```text
custom3
  -> 先进入 Gemmini side
  -> 再由 funct 决定这是：
       - 普通 Gemmini 指令
       - 还是 SPM xlate 指令
```

你可以把它理解成：

- opcode 负责“选哪扇门”
- funct 负责“进门以后办哪类业务”

底层 funct：

| API | funct | 含义 |
| --- | ---: | --- |
| `rerocc_gemmini_spm_xlate_cfg()` | `23` | 配置 PTBR、PTE 数量、page shift、enable |
| `rerocc_gemmini_spm_xlate_range()` | `24` | 配置 alias VA window 的 base 和 size |
| `rerocc_gemmini_spm_xlate_flush()` | `25` | 刷新 xlate 状态 |
| `rerocc_gemmini_spm_xlate_fault()` | `26` | 读取 fault 信息 |

### 2.1 `cfg`

API：

```c
void rerocc_gemmini_spm_xlate_cfg(uint64_t ptbr_pa,
                                  uint32_t pte_count,
                                  uint32_t page_shift,
                                  uint32_t enable);
```

编码：

- `rs1 = ptbr_pa`
- `rs2[63:16] = pte_count`
- `rs2[15:8] = page_shift`
- `rs2[0] = enable`

含义：

- `ptbr_pa` 是 PTE 表的物理地址；
- `pte_count` 是表里有多少项；
- `page_shift` 决定一页大小，例如 `10` 表示 `1024B`；
- `enable = 1` 打开翻译，`enable = 0` 关闭翻译。

### 2.2 `range`

API：

```c
void rerocc_gemmini_spm_xlate_range(uint64_t range_base,
                                    uint64_t range_size);
```

含义：

- `range_base` 是 alias VA window 的起始地址；
- `range_size` 是 window 覆盖的字节数。

硬件按下面方式找 PTE：

```text
vpage = (vaddr - range_base) >> page_shift
```

所以 `range_base` 很重要：PTE index 是相对 range base 计算的，不是相对整个虚拟地址空间计算的。

### 2.3 `flush`

API：

```c
void rerocc_gemmini_spm_xlate_flush(void);
```

用途：

- 软件改了 PTE；
- 或者重新配置了 `cfg/range`；
- 就需要 flush，让硬件看到新的翻译上下文。

### 2.4 `fault`

API：

```c
uint64_t rerocc_gemmini_spm_xlate_fault(void);
```

runtime 的解释方式是：

- 高位：fault vaddr；
- 低 8 位：fault cause。

对应封装在：

- `prt_gemmini_spm_xlate_fault_read()`
- `prt_gemmini_spm_xlate_fault_read_scoped()`

### 2.5 为什么 runtime 里还要“临时借用” `custom3`

既然 SPM xlate 和 Gemmini 都是 `custom3`，那为什么 runtime 还要做一套保存/恢复 `CSR_RROPC3` 的动作？

答案是：

- 在 ReRoCC 软件模型里，opcode lane 不是永久天然绑定某个 manager；
- 它是通过 `cfg slot + opcode_id + manager_id` 这组关系临时建立的。

所以 runtime 如果要给某个 manager 安装 xlate，仍然要先保证：

- 当前 `custom3` 这条 lane 在 ReRoCC 看来，已经绑定到目标 manager。

因此 `prt_gemmini_spm_xlate_program()` 这一类封装会：

1. 读出当前 `CSR_RROPC3` 的旧绑定；
2. acquire 目标 manager 的 `custom3` scope；
3. 发 `cfg / range / flush` 这些 `custom3` 指令；
4. release scope；
5. 恢复旧的 `CSR_RROPC3` 绑定。

把代码层面的动作展开，其实就是：

1. `prev_binding = rr_read_csr(CSR_RROPC3)`
2. 用固定的 `cfg15` 去 acquire 目标 manager
3. `rr_set_opc(3, 15)`，让 `custom3` 临时指向 `cfg15`
4. 发：
   - `funct=23` 的 `cfg`
   - `funct=24` 的 `range`
   - `funct=25` 的 `flush`
5. `rr_fence(15)`、`rr_release(15)`
6. `rr_write_csr(CSR_RROPC3, prev_binding)` 恢复旧绑定

也就是说，xlate helper 的真实运行链可以直接记成：

```text
read CSR_RROPC3
acquire cfg15 -> manager
write CSR_RROPC3 = 15
emit custom3/funct=23/24/25
fence + release cfg15
restore CSR_RROPC3
```

这说明“共用一个 opcode”并不等于：

- Gemmini compute 和 xlate 同时抢同一个硬件入口而互相说不清；

而是：

- 它们共享同一个 **Gemmini-side opcode namespace**
- runtime 再通过 scope 和保存/恢复绑定，把短暂的 xlate 配置与正常 Gemmini 计算串行化。

最实用的理解图是：

```text
ReRoCC:
  acquire(manager_id, opcode_id=3)
        |
        v
pair wrapper:
  custom3 -> Gemmini side
        |
        v
Gemmini controller:
  funct=23 -> xlate cfg
  funct=24 -> xlate range
  funct=25 -> xlate flush
  funct=26 -> xlate fault
  其它 funct -> 普通 Gemmini 指令
```

### 2.6 当前实现风险：`cfg15` 是“固定使用”，不是“正式保留”

这里需要明确记录一个实现层面的风险：

- 当前 xlate helper 固定使用 `cfg15`
- 但普通 stage 路径的 `cfg_id` 计算公式并没有把 `15` 剔除出去

普通路径使用：

```text
rr_cfg_id_for_stage(stage_id, opcode_id)
  = ((stage_id * 2) + lane) % 16
```

其中：

- `lane = 0` 对应 `opcode_id = 2`
- `lane = 1` 对应 `opcode_id = 3`

所以：

```text
stage_id = 7, opcode_id = 3
=> cfg_id = 15
```

这意味着：

- 普通 Gemmini/custom3 scope 也可能用到 `cfg15`
- 因而当前实现并不能严格宣称“`cfg15` 已经被保留给 xlate helper”

更准确的说法应该是：

- **当前 helper 代码把 `cfg15` 当作 xlate 的固定 slot**
- 但**整个 runtime 还没有一个统一 cfg allocator 来保证 `cfg15` 不会被普通 stage/custom3 路径撞上**

因此这件事应被记为：

- 一个当前已知的 RR cfg slot 复用风险
- 而不是一个已经被制度性消除的冲突

这也是为什么读 runtime / test plan 时，不能再把“`cfg15` 已正式保留”当成前提。

---

## 3. Linux 单测层 helper

入口文件：

- `generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests-coupleddma/rerocc_linux_spm_xlate.h`

这层 helper 主要给 Linux bring-up / 小测试用，它做三件事：

1. 分配一页内存作为 PTE backing；
2. 通过 pagemap 找到这页内存的物理地址，作为 `ptbr_pa`；
3. 提供 window include / map / program 的便利函数。

关键结构：

```c
typedef struct {
  uint64_t *pte;
  uint32_t pte_cap;
  uint64_t ptbr_pa;
  void *alloc;
  size_t alloc_bytes;
} rerocc_linux_spm_xlate_ctx_t;
```

关键 API：

- `rerocc_linux_spm_xlate_init()`
  - 分配 PTE 表并解析物理地址。
- `rerocc_linux_spm_xlate_window_include()`
  - 把某段 VA 纳入当前 alias window。
- `rerocc_linux_spm_xlate_map_range()`
  - 把 `[vaddr, vaddr + bytes)` 映射到 `[paddr, paddr + bytes)`。
- `rerocc_linux_spm_xlate_program()`
  - 发 `cfg + range` 指令。
- `rerocc_linux_spm_xlate_disable()`
  - 关闭 xlate 并 flush。

PTE 格式：

```text
pte[vpage] = ((physical_page_number) << 1) | valid_bit
```

也就是：

- bit 0 是 valid；
- 高位保存 physical page number。

---

## 4. pipeline runtime 的 xlate 上下文

入口文件：

- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/include/prt_page_table.h`
- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_page_table.c`

runtime 使用自己的上下文：

```c
typedef struct {
  uint64_t *pte;
  uint32_t pte_count;
  uint32_t next_vpage;
  prt_vmap_desc_t *free_vpages;
  prt_spm_tensor_map_t *tensor_maps;
  uint64_t ptbr_pa;
  uint64_t fault_count;
  uint64_t last_fault_vaddr;
  uint32_t last_fault_cause;
  ...
} prt_spm_xlate_ctx_t;
```

你可以把它理解为：

- `pte`：软件维护的页表；
- `pte_count`：可用 PTE 数量；
- `free_vpages`：alias window 内还没用的虚拟页；
- `tensor_maps`：tensor id 到 alias vpage 的映射；
- `ptbr_pa`：PTE 表给硬件看的物理地址；
- `fault_*`：runtime 自己记录的翻译错误。

### 4.1 分配 xlate context

API：

```c
int prt_spm_xlate_ctx_alloc(prt_runtime_t *rt,
                            prt_spm_xlate_ctx_t *ctx,
                            uint32_t page_count);
```

做的事：

1. 从 runtime 的 PTE pool 里分配一段物理可见内存；
2. 清零 PTE；
3. 记录 `ctx->ptbr_pa`；
4. 初始化 free vpage 列表，初始时整个 `[0, page_count)` 都可用。

### 4.2 把 tensor 映射成 alias VA

API：

```c
int prt_spm_map_tensor_ctx(prt_runtime_t *rt,
                           prt_spm_xlate_ctx_t *ctx,
                           uint32_t tensor_id,
                           const prt_page_list_t *pages,
                           uint64_t alias_base_va,
                           uint64_t *out_va_base);
```

做的事：

1. 给 tensor 分配连续的 virtual pages；
2. 对每个 virtual page 写 PTE；
3. PTE 指向 shared scratchpad global address：

```text
PRT_SHARED_SPAD_GLOBAL_ADDR_BASE + ppn * page_bytes
```

其中：

- `PRT_SHARED_SPAD_GLOBAL_ADDR_BASE = 0x40000000`
- `ppn` 来自 runtime 的 page allocator。

最后返回：

```text
out_va_base = alias_base_va + vpage_start * page_bytes
```

### 4.3 直接绑定一段 vpage

API：

```c
int prt_spm_bind_vpages_ctx(prt_runtime_t *rt,
                            prt_spm_xlate_ctx_t *ctx,
                            uint32_t vpage_start,
                            const prt_page_list_t *pages,
                            uint32_t page_count);
```

这个 API 不分配新 vpage，而是把指定 vpage 范围直接绑定到一组 pages。

pipeline runtime 在执行 stage 前会用它刷新 stage-local view：

- 某个 stage 的输入/权重/输出 tensor 当前对应哪些 SPM pages；
- 就把这些 pages 绑定到 action-local alias window 里的指定 vpage。

### 4.4 软件侧翻译检查

API：

```c
int prt_spm_translate_range_ctx(prt_runtime_t *rt,
                                prt_spm_xlate_ctx_t *ctx,
                                uint64_t alias_base_va,
                                uint64_t vaddr,
                                uint64_t bytes,
                                prt_spm_xlate_seg_t *segs,
                                uint32_t seg_cap,
                                uint32_t *out_seg_count);
```

它在软件里模拟一次 alias VA 到 physical address 的翻译，用于 runtime 自查和 DMA 分段。

失败常见原因：

- `vaddr < alias_base_va`
- `vpage >= pte_count`
- PTE invalid
- 输出 segment 数组不够大

---

## 5. runtime 如何把 xlate 装进硬件

入口文件：

- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_rerocc.c`
- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_schedule_action.c`
- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_runtime.c`

### 5.1 硬件编程封装

runtime 不直接在业务代码里发 `rerocc_gemmini_spm_xlate_*`，而是通过：

```c
int prt_gemmini_spm_xlate_program(uint32_t manager_id,
                                  uint64_t ptbr_pa,
                                  uint32_t pte_count,
                                  uint32_t page_shift,
                                  uint64_t range_base,
                                  uint64_t range_size,
                                  uint32_t enable);
```

这个函数内部做：

1. acquire 目标 manager 的 `custom3` scope；
2. 发 `cfg`；
3. 发 `range`；
4. 发 `flush`；
5. release scope；
6. 恢复原来的 `CSR_RROPC3` 绑定。

为什么要恢复 `CSR_RROPC3`？

- SPM xlate 和 Gemmini 都走 `custom3`；
- xlate 配置只是短暂借用 `custom3`；
- 配完后不能破坏后续 Gemmini 指令的 ReRoCC 路由。

### 5.2 action 级安装

`prt_action_bind_topology()` 最后会调用 `configure_action_spm_xlate()`。

它会遍历当前 action 用到的所有 Gemmini manager：

```text
for manager_id in action->acc_source.all_gemmini_mgr_ids:
    prt_gemmini_spm_xlate_program(manager_id, ptbr, pte_count, page_shift, alias_base, alias_bytes, 1)
```

所以 xlate context 是 action-local 的，但会安装到这个 action 涉及的所有 Gemmini manager 上。

### 5.3 stage 运行前刷新 PTE

stage 执行前，runtime 会在 `stage_prepare_exec_views()` 里：

1. 找到当前 stage 的 tensor pages；
2. 用 `prt_spm_bind_vpages_ctx()` 更新 PTE；
3. 对该 stage 关联的 Gemmini managers 调 `prt_gemmini_spm_xlate_flush()`。

更准确地说，这一步不是“重新分配 SPM 页”，而是：

> **把当前这一轮 stage 真正要访问的那组 shared SPM pages，重新绑定到 action-local alias window 对应的虚页上。**

如果你第一次看这段代码，最容易混淆的是两件事：

- **页的归属是谁决定的**：更早，在 `prt_action_alloc_spm()` 里就决定了；
- **这一轮 stage 该看哪一组页**：真正执行前，在 `stage_prepare_exec_views()` 里按当前运行态重新挑出来。

#### 5.3.1 先别急着看 bind，先看 page list 是从哪来的

stage 运行前刷 PTE，前提是 runtime 手里已经有“可供选择的 page list”。这些 page list 不是临时算出来的，而是前两个阶段就准备好的：

1. `prt_action_alloc_spm()`
   - 为当前 action 规划 shared SPM 物理页；
   - 产出三类绑定：
     - `action->spm_source.weight_pages`
     - `action->spm_source.in_stage_pages`
     - `action->spm_source.ring_pages`

2. `prt_action_bind_topology()`
   - 把这些绑定复制到当前 action 的执行态对象里；
   - 主要落到：
     - `exec->topo_weight_pages`
     - `exec->pipebufs[*].slot_pages`
     - `exec->ringbufs[*].slot_pages`

所以到了 `stage_prepare_exec_views()`，程序不是“重新找空闲 SPM 页”，而是：

> **从当前 action 的执行拓扑里，挑出这个 tensor 当前应该看的那份 `prt_page_list_t`。**

#### 5.3.2 `stage_tensor_current_pages()` 到底怎么确定 tensor pages

真正决定“当前 tensor pages 是谁”的 helper 是 `stage_tensor_current_pages()`。

它不是根据 `tensor_id` 直接算物理地址，而是按 tensor 在当前 stage 里的角色，去执行态结构里查：

1. **固定 tensor / 权重**
   - 先看 `stage_has_fixed_tensor(stage, tensor_id)`；
   - 如果为真，就调用 `runtime_find_weight_pages(rt, stage_id, tensor_id)`；
   - 它会在 `exec->topo_weight_pages` 里按 `(stage_id, tensor_id)` 找匹配项；
   - 也就是说，固定 tensor 用的是“这条 stage 预先绑定好的 weight pages”。

2. **entry tensor**
   - 如果不是 fixed tensor，就先调 `find_stage_pipebuf(rt, stage_id, tensor_id, 1)`；
   - 找到 entry pipebuf 后，默认取 `buf->slot_pages[buf->in_use_idx]`；
   - 这表示：当前这个 pipebuf 正在提供给 stage 使用的那个槽位。

3. **export tensor**
   - 如果 entry 没找到，再调 `find_stage_pipebuf(rt, stage_id, tensor_id, 0)`；
   - 默认同样取 `buf->slot_pages[buf->in_use_idx]`；
   - 这表示：当前 stage 这一轮准备写出的那份页面。

4. **ring fallback**
   - 如果 buffer 类型是 `PRT_BUF_C7_ENTRY_ALL_RING` 或 `PRT_BUF_C8_EXPORT_ALL_RING`；
   - 且当前 direct slot 还没有 pages：
     - `!buf->slot_pages[buf->in_use_idx].data`
     - 或 `buf->slot_pages[buf->in_use_idx].size == 0`
   - 就退回：

```text
buf->ring->slot_pages[buf->subbatch_offset % buf->ring->size]
```

这意味着程序会根据当前 `subbatch_offset`，选出 ring 里这一轮真正活跃的槽位。

所以，“程序如何确定 tensor pages”的更准确答案是：

- 先判断它是 fixed、entry 还是 export；
- 再从当前 action 的执行态对象中找到对应 buffer；
- 最后结合 `in_use_idx` 或 `subbatch_offset`，拿到本轮真正活跃的 `prt_page_list_t`。

它依赖的是**当前运行态**，而不是单靠 `tensor_id` 做静态推导。

#### 5.3.3 找到 pages 之后，PTE 是怎么刷新的

对每个要进 SPM 的 tensor slot，`stage_prepare_exec_views()` 会先检查：

- `stage->spm_bypass[slot] == 0`
- `stage->local_spm_page_count[slot] > 0`

只有满足这两个条件，才说明这个 slot 真的需要在 shared SPM 里建立视图。

然后 runtime 计算：

```text
exec_vpage = stage->exec_base_vpage + stage->local_spm_first_vpage[slot]
```

这里三个字段分别表示：

- `stage->exec_base_vpage`
  - 这个 stage 在 action alias window 里的起始虚页；
- `stage->local_spm_first_vpage[slot]`
  - 这个 tensor 在 stage 局部 SPM 视图里的起始虚页；
- `stage->local_spm_page_count[slot]`
  - 这个 tensor 这一轮需要绑定多少页。

拿到 `pages` 后，代码会构造一个局部 `view`：

- `view.data = pages->data`
- `view.size = stage->local_spm_page_count[slot]`
- `view.cap = stage->local_spm_page_count[slot]`

然后调用：

```text
prt_spm_bind_vpages_ctx(rt, &action->spm_xlate,
                        exec_vpage,
                        &view,
                        stage->local_spm_page_count[slot])
```

把这段 alias vpage 对应的 PTE 改写成“指向当前 pages”。

注意这里有一个很关键的细节：

- `pages` 可能比当前 stage 真正要看的页更多；
- 但 runtime 只会绑定前 `local_spm_page_count[slot]` 页；
- 因为 stage 关心的是“当前执行视图需要多少页”，不是把 buffer 的所有备用状态都暴露给 Gemmini。

#### 5.3.4 一个简化例子

假设当前 stage 某个 tensor slot 的信息是：

- `stage->exec_base_vpage = 32`
- `stage->local_spm_first_vpage[slot] = 4`
- `stage->local_spm_page_count[slot] = 3`

而 `stage_tensor_current_pages()` 最终选到的当前 pages 是：

```text
[pageA, pageB, pageC]
```

那么 runtime 实际做的是：

- 把 alias window 中的 vpage `36`
  绑定到 `pageA`
- 把 alias window 中的 vpage `37`
  绑定到 `pageB`
- 把 alias window 中的 vpage `38`
  绑定到 `pageC`

之后 Gemmini 只要访问落在这 3 个虚页里的 alias VA，就会被 xlate 翻译到这 3 个 shared SPM 页上。

#### 5.3.5 为什么要在每次 stage 开跑前刷新

因为“当前 tensor 对应哪几页”是会随运行态变化的：

- double buffer 下，`in_use_idx` 会切换；
- ring buffer 下，`subbatch_offset` 会推进；
- fixed tensor 虽然通常对应固定 pages，但本轮是否要先 DMA preload，取决于 lazy-fetch 状态；
- 不同 action 也各自拥有独立的 alias window 和 PTE context。

所以 runtime 不能只在 action 初始化时 bind 一次就完事，而必须在 stage 真正运行前，把**本轮有效页**重新写到 PTE 里。

#### 5.3.6 flush 在这里的意义

所有需要绑定的 tensor 都处理完之后，`stage_prepare_exec_views()` 会调用：

```text
runtime_flush_stage_spm_xlate(rt, stage_id)
```

它会收集该 stage 实际关联的 Gemmini managers，并对这些 manager 做 xlate flush。

这样后续 Gemmini 通过 alias VA 发出的访问，才能看到刚刚更新后的 PTE。

如果只改软件侧 PTE、不做硬件 flush，那么 Gemmini 看到的仍可能是旧翻译状态。

这里有一个非常重要的注释：

> SPM xlate 的 PTE index 来自 `(vaddr - range_base)`，所以 action-local alias window 内的绑定必须从 vpage 0 开始理解。

也就是说，不要把 `alias_vpage_start` 直接叠加到硬件 PTE index 上。当前硬件接口没有给每个 action 单独 rebase PTBR 的能力，runtime 是靠 `range_base` + action-local window 来隔离的。

### 5.4 Gemmini 描述符拿到的是 alias VA

当 `spm_xlate_enable = 1` 时，`stage_tensor_exec_addr()` 会给 Gemmini 返回：

```text
action->alias_base_va
  + stage->exec_base_vpage * page_bytes
  + stage->local_spm_tensor_addr[slot]
```

也就是说，`prt_gemmini_conv_desc_t.input / weights / bias / output` 里放的不是 shared scratchpad 物理地址，而是给 xlate 看的 alias VA。

这就是 pipeline runtime 里 Gemmini 能直接吃“像普通指针一样的地址”的原因。

---

## 6. 初始化与默认策略

runtime 初始化时会做几件事：

- 如果 `spm_page_shift = 0`，根据 `page_size_bytes` 推导；
- 如果 `spm_xlate_range_size = 0`，根据 `num_cores * pages_per_acc * page_size_bytes * 8` 推导；
- 如果启用 `spm_xlate_enable`，会强制 `sync_mode = blocking_debug`；
- `blocking_debug` 会进一步强制：
  - `dma_backend = blocking_fence`
  - `gemmini_mode = blocking_fence`

直觉解释：

- xlate 页表当前是同步改写和刷新；
- 为了避免异步 DMA/Gemmini 与 PTE 更新交错，runtime 在 xlate 打开时走保守同步路径。

---

## 7. 阅读代码时的检查点

读 SPM xlate 相关代码时，建议按这个顺序问：

1. 这次 alias window 的 `range_base` 和 `range_size` 是多少？
2. `ptbr_pa` 指向的 PTE 表在哪里分配？
3. `page_shift` 是否和 runtime page size 一致？
4. 这个 tensor 的 alias VA 对应哪些 vpage？
5. PTE 写的是 shared scratchpad global address 还是普通 DRAM physical address？
6. PTE 改完后有没有 `publish` 和硬件 `flush`？
7. 当前安装到了哪些 Gemmini manager？

能把这几个问题串起来，SPM xlate 在 pipeline runtime 里的作用就清楚了。
