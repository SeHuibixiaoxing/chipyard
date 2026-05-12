# Pipebuffer 管理导读

这份文档专门解释 `pipeline runtime` 里的 `pipebuf / ringbuf` 管理。

目标不是罗列几个结构体名字，而是回答下面这些问题：

1. `buffer_id`、`slot_id` 这些编号到底是谁决定的。
2. 这些编号和 shared SPM page 分配是什么关系。
3. runtime 里的 `pipebuf`、`ringbuf`、`shared/isolate pair` 分别扮演什么角色。
4. 它们如何影响 stage 的同步、DMA、Gemmini 计算，以及 subbatch 推进。

如果你只记一句话，可以先记这个：

> `tensor_id` 说“这是谁的数据”，`buffer_id` 说“这份数据挂在哪个运行时容器上”，`slot_id` 说“这个容器当前或可切换的哪一份 backing pages”。而 `pipebuf` 正是 runtime 用来把这三层身份和实际执行流程接起来的核心对象。

---

## 1. 先分清几层对象

当前 `pipeline runtime` 里，和 buffer 相关的对象至少有四层：

1. pipeline artifact 里的 **tensor role**
   - 某个 stage 的 `entryTensorIdList` / `exportTensorIdList`
   - 这定义的是“stage 入口 / 出口语义上有哪些 tensor”
2. pipeline artifact 里的 **buffer binding**
   - `bufferBinding*List`
   - 这定义的是“这些 tensor 在 runtime 里应当挂在哪类 buffer 上，用几份 slot，每份几页”
3. runtime 里的 **pipebuf / ringbuf 对象**
   - `prt_pipebuf_t`
   - `prt_ringbuf_t`
   - 这是实际执行时被 worker 线程操作的对象
4. action 分配出来的 **page list**
   - `prt_spm_page_binding_t`
   - `prt_page_list_t`
   - 这才是真正落到 shared SPM 上的页集合

所以不要把下面几件事混为一谈：

- “一个 tensor”
- “一个 buffer binding”
- “一个 pipebuf 对象”
- “一组实际 page list”

当前实现里，这几层是明确分开的。

---

## 2. `buffer_id` 到底是谁决定的

先说结论：

- `buffer_id` 不是 runtime 现场临时发明出来的；
- 它是 **artifact exporter** 在生成 pipeline runtime YAML 时写进去的；
- runtime 只负责读取、校验、兑现，不重新编号。

### 2.1 真实编号来源

当前 canonical exporter 是：

- `conference/HybridMapper/scripts/create-pipeline-runtime-artifacts.py`

在这个脚本里，`build_segment_runtime_layout(...)` 直接做了：

- `next_buffer_id = 1`
- 然后按 segment 内部顺序递增分配

也就是说：

- `buffer_id` 的作用域是 **单个 segment**
- 每个 segment 内部从 `1` 开始编号
- 同一 segment 内不会复用同一个 `buffer_id`

这一点非常重要，因为很多人会下意识以为：

- `buffer_id` 是 runtime 根据 `pipebuf_count` 动态编号

当前代码不是这样。

### 2.2 编号顺序是什么

exporter 里的编号顺序不是随意的，而是固定的：

1. 先按 stage 顺序遍历每个 stage。
2. 对每个 stage：
   - 先给 `entryTensorIdList` 里的每个 entry tensor 分一个 `PIPE` buffer_id
   - 再给 `exportTensorIdList` 里的每个 export tensor 分一个 `PIPE` buffer_id
3. 所有 stage 的 entry/export 处理完之后：
   - 再给 `fixTensorDramBypassIdList` 里的 fixed tensor 分配 `WEIGHT` buffer_id
4. 最后：
   - 再给 segment 级 ring transport 分配 `RING` buffer_id

所以当前 `buffer_id` 的顺序，本质上是：

```text
segment 内：
  stage0 entry/export PIPE
  -> stage1 entry/export PIPE
  -> ...
  -> WEIGHT
  -> RING
```

这带来两个直接后果：

1. `buffer_id` 反映的是 exporter 生成顺序，不是 tensor_id 大小顺序。
2. 一个 tensor 即使语义上是“同一块数据”，也可能因为它在不同 stage 里扮演不同 entry/export 角色，而拿到不同 `buffer_id`。

### 2.3 `buffer_id` 是怎么写回 stage 的

exporter 生成完 `buffer_bindings` 后，会把每个 stage 对应的：

- `entryBufferIdList`
- `exportBufferIdList`

写回 stage 描述。

所以 stage YAML 里会同时出现：

- `entryTensorIdList`
- `entryTensorTypeList`
- `entryBufferIdList`

以及：

- `exportTensorIdList`
- `exportTensorTypeList`
- `exportBufferIdList`

这说明 artifact 层已经明确把：

- “语义 tensor 列表”
和
- “运行时 buffer 编号列表”

绑定起来了。

### 2.4 runtime 读取时会不会改这些编号

不会。

`prt_yaml_loader.c` 做的事情非常直接：

- `entryTensorIdList` 读进 `prt_stage_map_t.entry[i].tensor_id`
- `entryBufferIdList` 读进 `prt_stage_map_t.entry[i].buffer_id`
- `exportTensorIdList` 读进 `prt_stage_map_t.exports[i].tensor_id`
- `exportBufferIdList` 读进 `prt_stage_map_t.exports[i].buffer_id`

segment 级的：

- `bufferBindingIdList`
- `bufferBindingTensorIdList`
- `bufferBindingStageLocalIdList`
- `bufferBindingKindList`
- `bufferBindingSlotCountList`
- `bufferBindingPagesPerSlotList`
- `bufferBindingAliasGroupIdList`

则被读进：

- `prt_segment_desc_t.buffer_bindings[]`

也就是说，runtime 这里只做两件事：

1. 把编号读进内存结构；
2. 检查这些编号和 stage / ring / tensor 合同是否自洽。

它不重新分配 `buffer_id`。

---

## 3. `slot_id` 到底是谁决定的

和 `buffer_id` 不同，`slot_id` 不是 exporter 显式写在 YAML 里的字段。

当前 `slot_id` 的来源是：

- exporter 先写一个 `slot_count`
- runtime 再按 `slot_count` 展开成
  `slot 0, slot 1, ..., slot_count-1`

所以：

- `buffer_id` 是 artifact 显式给定的“容器编号”
- `slot_id` 是 runtime 根据这个容器的 `slot_count` 派生出来的“槽位编号”

### 3.1 `slot_id` 的编号范围

对某个 buffer binding，runtime 在 `prt_action_alloc_spm()` 里会做：

- `for (slot = 0; slot < binding->slot_count; ++slot)`

然后对每个 slot 单独分 page list。

因此：

- `slot_id` 总是从 `0` 开始
- 最大到 `slot_count - 1`
- 它的作用域是 **单个 buffer_id 内部**

所以不要把 `slot_id` 理解成全局编号。

### 3.2 不同 kind 的 `slot_count` 怎么来

#### `PIPE`

`PIPE` 的 `slot_count` 主要由 exporter 根据 stage 的 tensor binding 生成：

- 如果该 tensor 开了 double buffer：
  - `slot_count = 2`
- 否则：
  - `slot_count = 1`

但如果该 tensor 是 `SHARED_SPM`，事情会再多一层：

- exporter 会按 `tensor_id` 聚合 shared group
- 对同组内多个 `PIPE` binding：
  - `slot_count` 取组内最大值
  - `pages_per_slot` 也取组内最大值

这意味着：

- 不同 `buffer_id` 的 `PIPE` binding`
- 可能共享同一个 `alias_group_id`
- 最终复用同一组 slot page 规划

#### `WEIGHT`

`WEIGHT` 的 `slot_count` 固定为 `1`。

因为 fixed tensor 不做双缓冲，也不是 ring。

所以对 `WEIGHT`：

- `slot_id` 基本总是 `0`

#### `RING`

`RING` 的 `slot_count` 来自 ring transport 配置：

- 也就是 ring 的槽数

因此：

- ring 的每个 `slot_id`
  对应的是一个 ring 槽位
- 最终会落到 `prt_ringbuf.slot_pages[slot]`

#### `ALL_RINGBUFFER`

这里最容易搞混。

`ALL_RINGBUFFER` 对应的 stage entry/export `PIPE` binding` 在 exporter 里会被写成：

- `slot_count = 0`
- `pages_per_slot = 0`

原因不是“这个 tensor 没有 slot”，而是：

- 它不走普通本地 pipe slot 分配
- 真正的数据槽位由独立的 `RING` binding` 提供

所以对 `ALL_RINGBUFFER`：

- stage 侧 `PIPE` binding` 本身不拥有自己的本地 slot pages
- 它会在运行时借用 ring 的 slot pages

---

## 4. `alias_group_id` 和 `buffer_id` 的关系

这个点和 `SHARED_SPM` 尤其相关。

当前 exporter 的策略是：

- 对同一个 shared tensor，组内多个 `PIPE` binding`
  虽然各自拿到不同 `buffer_id`
- 但它们会共享同一个 `alias_group_id`

这意味着：

- `buffer_id` 仍然区分不同 stage / 不同 entry/export 角色
- `alias_group_id` 额外说明：
  “这些不同的 buffer，底层其实应该共用同一组 physical pages”

到了 runtime 的 `prt_action_alloc_spm()`：

- 如果 `alias_group_id != 0`
- runtime 会先为这个 group 分配一套 `alias_plan->slot_pages[slot]`
- 之后同组其它 binding 不再重新分配，而是复用这组 pages

因此对 shared tensor：

- `buffer_id` 不是“物理页唯一标识”
- `buffer_id` 更像“逻辑容器实例 id”
- 真正决定它们是否共享底层页的是 `alias_group_id`

---

## 5. runtime 里到底有哪些 buffer 相关对象

这一部分非常重要，因为很多编号在不同对象里作用不同。

### 5.1 `prt_buffer_binding_t`：静态合同

它来自 pipeline YAML，描述的是：

- 这个 binding 服务哪个 tensor
- 属于哪个 stage
- 是 entry 还是 export
- 是 `PIPE` / `WEIGHT` / `RING`
- 有几个 slot
- 每个 slot 几页
- 是否属于某个 alias group

可以把它理解成：

> **artifact 对 runtime 说：“我需要你准备这样一种 buffer 容器。”**

它还不是执行期对象，只是静态合同。

### 5.2 `prt_pipebuf_t`：stage 执行期容器

这是 worker 线程真正操作的 buffer 对象。

每个 stage entry/export tensor，`build_topology_from_pipeline()` 都会创建一个 `prt_pipebuf_t`。

它保存的关键信息包括：

- `buffer_id`
- `tensor_id`
- `stage_idx`
- `is_entry`
- `kind`
- `with_double_buffer`
- `slot_pages[2]`
- `dram_base_addr[2]`
- `full[2]`
- `cmd_running[2]`
- `in_use_idx / no_use_idx`
- `subbatch_offset`
- `ring`

所以 `prt_pipebuf_t` 不是“纯 page list”。
它是：

- 页的持有者
- 传输状态机
- 和 stage worker 同步的对象

### 5.3 `prt_ringbuf_t`：segment 级页环

`prt_ringbuf_t` 描述的是：

- 一个 segment 级 ring transport

它保存的是：

- `buffer_id`
- `tensor_id`
- `size`
- `head`
- `tail`
- `out_degree`
- `use_count`
- `slot_pages[]`

所以 ring 不是“一个地址队列”，而是：

> **一组真正的 shared SPM page slots 组成的环。**

### 5.4 `prt_spm_page_binding_t`：action 分配结果

这是真正的：

- `(buffer_id, tensor_id, stage_id, slot_id) -> pages`

映射。

它是 action 分配 SPM 后的结果，不是 artifact 原始输入。

当前 runtime 里：

- `weight_pages`
- `in_stage_pages`
- `ring_pages`

本质上都是 `prt_spm_page_binding_t` 数组。

这一步之后，page list 才真正和某个 runtime buffer 身份绑定起来。

---

## 6. 存储管理：pipebuffer 是怎样接上 shared SPM 的

这一部分回答：

> 一个 `buffer_id` 最后是如何变成 `pipebuf.slot_pages[]` 或 `ringbuf.slot_pages[]` 的？

### 6.1 第一步：先建立执行期拓扑对象

`build_topology_from_pipeline()` 做的不是分配页，而是先建骨架：

1. 根据 segment 的 `ring_cfgs` 建 `ringbuf`
2. 根据每个 stage 的 entry/export 建 `pipebuf`
3. 每个 `pipebuf` 直接从 stage 的 tensor binding 里继承：
   - `tensor_id`
   - `buffer_id`
   - `double_buffer`
4. 根据 `tensor_type + has_ring` 分类成 `C1..C8`
5. 再识别跨 stage 的：
   - `isolate_pairs`
   - `shared_pairs`

这一阶段可以理解成：

> **runtime 先把“有哪些容器、这些容器怎么连接”建出来，但此时容器还没真正挂上 pages。**

### 6.2 `C1..C8` 是怎么分类出来的

当前分类规则很简单：

- `DRAM` / `DRAM_DEPEN`
  - entry -> `C1`
  - export -> `C2`
- `ISOLATE_SPM`
  - 无 ring -> `C3`
  - entry + ring -> `C5`
  - export + ring -> `C6`
- `SHARED_SPM`
  - `C4`
- `ALL_RINGBUFFER`
  - entry -> `C7`
  - export -> `C8`

这里的含义不是“八种不同 buffer 结构体”。

它的真实含义是：

- 同一个 `prt_pipebuf_t`
- 根据 tensor 类型和是否带 ring
- 走不同的 transport / publish / wait 语义

### 6.3 第二步：action 分配真实 SPM 页

`prt_action_alloc_spm()` 会遍历 `seg->buffer_bindings[]`。

对每条 binding，它按 kind 做不同处理。

#### `RING`

对每个 ring slot：

1. 分配 `pages_per_slot` 个 page
2. 生成一个 `prt_page_list_t`
3. 记录成：
   - `buffer_id = 这个 ring buffer`
   - `slot_id = 当前 ring 槽号`

最后这些结果进入：

- `action->spm_source.ring_pages`

#### `WEIGHT`

对每个 fixed tensor：

1. 只分配一组 pages
2. 记录成 `slot_id = 0`

最后进入：

- `action->spm_source.weight_pages`

#### `PIPE`

对普通 `PIPE` binding：

1. 按 `slot_count` 循环
2. 每个 slot 单独分配一组 pages
3. 记录成：
   - `buffer_id = 当前 pipe binding`
   - `slot_id = 0/1/...`

最后进入：

- `action->spm_source.in_stage_pages`

#### `PIPE + alias_group`

如果这个 `PIPE` binding` 带 `alias_group_id`：

1. runtime 先查这个 group 是否已经分配过 slot pages
2. 如果没有：
   - 先按 group 的 `slot_count/pages_per_slot` 分一整套
3. 如果已经有：
   - 当前 binding 直接复用前面那套 slot pages

所以：

- 不同 `buffer_id`
- 可能对应相同底层 `ppn` 集合

这是 shared tensor 的核心。

### 6.4 第三步：为何分配时还要看 preferred manager

`prt_action_alloc_spm()` 在每条 binding 分配之前，还会决定：

- 这些 pages 优先放在哪些 Gemmini manager 的 shared SPM 上

当前规则大致分两类：

1. 普通 stage-local binding
   - 优先用这个 stage 已分到的 manager 集合
2. `RING` 或带 `alias_group` 的 binding
   - 优先范围扩大到 action 的所有 manager

这背后的含义是：

- 普通 stage-local pages 更偏向局部性
- cross-stage 共享 / ring pages 不再只偏向单个 stage

因此：

- `buffer_id` 决定逻辑容器是谁
- `preferred managers` 决定这组容器的页更倾向落在哪些 manager 的 SPM 里

### 6.5 第四步：把 page list 重新挂回 pipebuf / ringbuf

`prt_action_bind_topology()` 才是把 action 分好的页真正挂回执行期对象的步骤。

这里的逻辑非常关键：

- `ringbuf` 绑定 pages 时，按 `(buffer_id, slot_id)` 去 `ring_pages` 里找
- `pipebuf` 绑定 pages 时，按 `(buffer_id, slot_id)` 去 `in_stage_pages` 里找

也就是说，在“拓扑绑定”这一步里：

- 主键是 `buffer_id + slot_id`
- 不是 `tensor_id`

这也是为什么前面说：

- `tensor_id` 是语义身份
- `buffer_id` 才是容器身份

### 6.6 `stage_tensor_current_pages()` 又为什么改回按 `tensor_id` 查

因为这时问题已经变了。

在绑定拓扑之前，runtime 的问题是：

> “这组 page list 应该挂回哪个 pipebuf/ringbuf？”

这时主键当然是：

- `buffer_id + slot_id`

而到了 stage 真正执行时，问题变成：

> “当前 stage 正在处理 tensor X，它现在应该看到哪组页？”

这时代码自然改成按：

- `stage_id + tensor_id`

去找：

1. fixed tensor
   - `runtime_find_weight_pages(stage_id, tensor_id)`
2. entry pipe
   - `find_stage_pipebuf(stage_id, tensor_id, is_entry=1)`
3. export pipe
   - `find_stage_pipebuf(stage_id, tensor_id, is_entry=0)`

拿到 pipebuf 后，默认返回：

- `buf->slot_pages[buf->in_use_idx]`

而对 `C7/C8` 这种 pure ring 路线，如果本地 slot 为空，会退回：

- `buf->ring->slot_pages[subbatch_offset % ring->size]`

所以可以这样理解：

- **绑定阶段**：按 `buffer_id + slot_id`
- **执行阶段**：按 `stage_id + tensor_id`

两者并不冲突，只是问题不同。

### 6.7 stage 运行前为什么还要“重绑一次视图”

`stage_prepare_exec_views()` 会在每次 stage 运行前，把“当前应看到的 pages”重新绑定到当前 stage 的执行视图。

它做的事情是：

1. 对 fixed tensor，必要时先把 host 数据 DMA 到当前 pages
2. 对每个 tensor 调 `stage_tensor_current_pages()`
3. 按 stage 的：
   - `exec_base_vpage`
   - `local_spm_first_vpage`
   - `local_spm_page_count`
4. 把当前 pages 绑定到 action 的 SPM xlate PTE 上
5. 最后 flush 这次 stage 的 xlate 视图

这说明：

- `pipebuf.slot_pages[]` 只是“候选 backing pages”
- 真正给 Gemmini 看的，是“这一次 stage 执行前被绑定到 alias window 的当前那组页”

---

## 7. 同步管理：pipebuffer 如何影响运行时计算

这一部分是 pipebuffer 真正的核心。

如果只从“存储容器”理解 `pipebuf`，你会漏掉最重要的一层：

> `pipebuf` 同时还是 stage 之间的同步对象。

### 7.1 `prt_pipebuf_t` 里最重要的状态字段

#### `full[idx]`

表示：

- 这个 slot 当前是否已经有一份完整数据可供消费或发送

可以把它理解成：

- `1`：数据就绪
- `0`：这个 slot 还空着

#### `cmd_running[idx]`

表示：

- 这个 slot 当前是否有 fetch / flush / send / publish 之类命令在执行

它不是“是否有数据”，而是“是否还有命令在飞”。

#### `cmd_count[idx]`

是 `cmd_running` 的计数版。

当前大多数路径只把它当：

- 是否还有未完成 transport 的辅助状态

#### `in_use_idx / no_use_idx`

这两个字段只在双缓冲下真正关键。

- `in_use_idx`
  - 当前 stage 计算实际使用哪一份 slot pages
- `no_use_idx`
  - 当前没被计算占用、可用于预取下一批数据的 slot

所以双缓冲的核心不是“多一份内存”，而是：

- 一份给本轮 compute 用
- 另一份允许 DMA/transport 提前准备下一轮数据

#### `subbatch_offset`

表示：

- 这个 buffer 当前推进到了第几个 subbatch

这个字段几乎贯穿所有 transport 逻辑：

- ring ready / idle 判断
- C1/C2/C5/C6 的 ring 槽位选择
- C7/C8 的纯 ring 发布/消费
- shared / isolate pair 的进度对齐

它可以理解成：

> “这个 buffer 认为自己当前正在处理哪一拍 subbatch。”

#### `fanout_total / fanout_pending`

这两个字段主要用于 `C3 isolate` export。

因为一个 export 可能要发给多个后继 entry。

所以：

- `fanout_total`
  - 这份数据理论上要发给多少个后继
- `fanout_pending`
  - 当前这一轮还有多少个后继没消费完

只有所有后继都拿走后，producer export slot 才会真正清空。

#### `tag` / `shared_tag`

这两个字段主要服务：

- `C7/C8` 的 ring page metadata 绑定
- `C4 shared` 的无拷贝交接

它们本质上是“阶段切换的 ownership bit”，不是数据内容。

### 7.2 `prt_ringbuf_t` 里最重要的状态字段

#### `head / tail`

当前 ring 不是简单数组，而是带发布/消费边界的页环。

- `tail`
  - 已经发布完成的最末端后一位
- `head`
  - 最老、且已经没有消费者持有的那一位

因此：

- `head <= offset < tail`
  才表示某个 subbatch 对应的 ring slot 已 ready

#### `out_degree`

表示：

- 一个 ring slot 发布后，需要被消费多少次才算真正释放

这在多消费者场景下很关键。

#### `use_count`

这是 ring 的“引用计数表”。

某个 subbatch 发布到 ring 后：

- `ring_fill_locked()` 会给它挂一个 use_count
- 值初始化为 `out_degree`

每消费一次：

- `ring_use_locked()` 递减一次

只有减到 `0`，这个 slot 才会真的让 `head` 前进。

所以 ring 不是“写完就能覆盖”，而是：

- 发布
- 等所有消费者都确认消费
- 才能复用该 slot

---

## 8. worker 线程如何围绕 pipebuffer 推进一次 subbatch

这部分最值得认真看，因为它解释了：

> pipebuffer 不只是 transport 前后挂一下页，而是直接决定 stage 什么时候能算、什么时候必须等。

### 8.1 大致顺序

每个 stage worker 的主循环，顺序可以概括成：

1. 先处理所有 entry buffer
   - 必要时从 DRAM / ring / 前驱 SPM 搬运
2. 等所有 entry 的当前 `in_use_idx` 变成 `full`
3. 再确保 export 侧已经 ready
   - 例如旧数据已经 flush 完
   - ring 有空位
   - shared/export 状态已经排空
4. 执行 Gemmini 计算
5. 计算结束后：
   - 把 entry 当前 slot 清空
   - 把 export 当前 slot 标成 full
6. 再按 export kind 做真正的 publish / flush / forward
7. 若成功，必要时旋转双缓冲

所以 compute 根本不是“看到数据就直接算”。
它前后都被 pipebuffer 状态严格包住。

### 8.2 计算前为什么要等 entry `full`

worker 在真正调用 Gemmini 前，会对每个 entry buffer 调：

- `prt_pipebuf_wait_full(b, in_use_idx, timeout)`

这意味着：

- 只要当前 `in_use_idx` 还没变成 full
- 这个 stage 就不能进入 compute

所以 entry path 的 transport 失败、过慢、或状态没切好，都会直接阻塞计算。

### 8.3 计算前为什么还要等 export `ready`

这点容易被忽略。

当前 worker 在 compute 前还会先跑：

- `stage_wait_exports_ready(...)`

它的作用不是“等新的 export 数据 ready”，而是：

- 确保 export 当前要写的那个 slot 现在是可写的

例如：

- `C2/C6` 要确认旧 flush 已退役，slot 已 empty
- `C8` 要先确认 ring 有空位，必要时先 prime 下一个 ring slot
- `C4` 要确认 shared 这一路已经完成上一轮所有权切换

所以 export path 也会反向阻塞 compute。

换句话说：

> 一个 stage 是否能开始算，不只取决于输入 ready，也取决于输出容器是否已经准备好承接这一轮结果。

---

## 9. `C1..C8` 各类 pipebuffer 的真实行为

这一节是最核心的运行语义总结。

### 9.1 `C1`：entry from DRAM / DRAM_DEPEN

语义：

- 把数据装进本地 entry pipebuf 的当前 slot

来源有两种：

1. `dram_base_addr != 0`
   - 直接从 host/DRAM DMA 到 `slot_pages[idx]`
2. `dram_base_addr == 0` 且带 ring
   - 不是从 DRAM 拿
   - 而是从 `ring->slot_pages[subbatch_offset % ring->size]`
     搬到本地 `slot_pages[idx]`

完成后：

- `full[idx] = 1`
- `subbatch_offset += 1`

如果它是消费 ring 的路径，还会对 ring 做一次 use。

### 9.2 `C2`：export to DRAM / DRAM_DEPEN

语义：

- 把本地 export slot 的结果导出到 DRAM，或者导出到 ring

来源是本地：

- `slot_pages[idx]`

目标有两种：

1. `dram_base_addr != 0`
   - flush 到 DRAM
2. `dram_base_addr == 0` 且带 ring
   - 写到 `ring->slot_pages[subbatch_offset % ring->size]`

完成后：

- 本地 `full[idx] = 0`
- `subbatch_offset += 1`

如果写的是 ring，还会推进 ring 的 `tail/use_count`。

### 9.3 `C3`：无 ring 的 `ISOLATE_SPM`

语义：

- 前驱 export slot 和后继 entry slot 之间做一次显式 SPM->SPM DMA

它要求：

- 前驱 full
- 后继 empty
- 两边 `subbatch_offset` 一致

完成后：

- 后继 `full = 1`
- 后继 `subbatch_offset += 1`

对前驱则不是立刻简单清空，而是要看 fanout：

- 如果还有其它后继没拿到
  - 只减少 `fanout_pending`
- 如果这是最后一个后继
  - 才清空 producer slot
  - 再推进 producer `subbatch_offset`

所以 `C3` 是显式“拷贝 + fanout 计数”模型。

### 9.4 `C4`：`SHARED_SPM`

这是最容易误读的一类。

它的核心不是“把数据从前驱拷到后继”，而是：

- 前驱和后继本来就共享同一组 backing pages
- runtime 只负责切换这组页的可见 ownership

因此 `C4` 没有真正 DMA copy。

当前实现是一个两相切换：

1. 如果 producer 当前 full，且所有 consumer 当前都 empty，且 `tag == 0`
   - 把所有 consumer 标成 full
   - 表示“这份共享页现在可以被下游当作 ready 输入”
2. 下一轮再进入同样条件，且 `tag == 1`
   - 清 producer 的 full
   - 表示“上游这边对这一拍的拥有权已经结束”

所以 `C4` 的本质是：

- **无拷贝传播**
- 靠 `full + tag` 在共享页的多个视角之间切所有权

### 9.5 `C5/C6`：带 ring 的 `ISOLATE_SPM`

它们是 `C3` 的 ring 版。

#### `C5`

- 从 ring slot 搬到本地 entry slot

#### `C6`

- 从本地 export slot 搬到 ring slot

和 `C1/C2` 相比，它们两端都是 SPM page list，不经过 DRAM base address。

### 9.6 `C7`：entry `ALL_RINGBUFFER`

这是“纯 ring entry”。

关键点是：

- 它不把数据真正从 ring DMA 到自己的本地 slot
- 它做的是 **page list metadata 绑定**

流程大致是：

1. 等 ring ready
2. 把当前 ring slot 的 page list 复制到 `buf->slot_pages[0]`
   - 注意：这里只是 page descriptor copy，不是 payload copy
3. `full[0] = 1`
4. compute 消费完后，当 `full[0]` 被清空
5. 再对 ring 做 `use`
6. 释放当前 subbatch 的 ring 持有

所以 `C7` 的含义是：

- stage 直接在 ring slot 对应的页上算
- 不是“ring -> local scratch slot”的二次搬运

### 9.7 `C8`：export `ALL_RINGBUFFER`

这是“纯 ring export”。

它的工作更像“预占 ring slot 并把当前 export view 指过去”。

流程是两段：

1. compute 前的 `stage_wait_exports_ready()`
   - 先看 ring 是否有空位
   - 如果有，就把下一拍 ring slot 的 page list 复制进 `buf->slot_pages[0]`
   - 相当于先把 export view prime 到 ring slot 上
2. compute 后
   - worker 把 `full[0] = 1`
   - `prt_process_c8()` 再把这个 slot 正式 publish 到 ring
   - 推进 `tail`
   - 然后如果 ring 还有空位，还会顺手再把下一拍 slot 预绑进来

所以 `C8` 的核心不是“算完再拷到 ring”，而是：

- **先把 export view 对准 ring slot**
- 然后让 compute 直接把结果写进 ring slot 对应的页

---

## 10. 双缓冲到底怎么生效

双缓冲不是自动发生的，它要同时满足两个条件：

1. artifact 已把这个 buffer 标成 `double_buffer`
2. 这条路径的 transport 逻辑支持对 `no_use_idx` 做提前预取或下一拍切换

### 10.1 初始状态

`pipebuf_init()` 初始化时：

- `in_use_idx = 0`
- `no_use_idx = 1`（如果开双缓冲）

所以第一拍总是先用 slot 0。

### 10.2 什么时候旋转

`rotate_buf_if_needed()` 只在以下场景发生：

- entry 侧：
  - `C1`
  - `C5`
- export 侧：
  - `C2`
  - `C6`

而且是这轮 transport 成功之后才旋转：

- `in_use_idx = (in_use_idx + 1) & 1`
- `no_use_idx = (no_use_idx + 1) & 1`

所以：

- 双缓冲不是 compute 一结束就转
- 而是要等这一轮 transport / publish 真正确认完成

### 10.3 `no_use_idx` 什么时候会被提前用到

在 async gemmini overlap 模式下，runtime 还会尝试：

- 对 entry 双缓冲 slot 的 `no_use_idx`
  进行 prefetch

前提是：

- `no_use_idx` 当前 empty
- 且没有命令在飞

这样 compute 在用 `in_use_idx` 的同时，下一拍的数据可以往 `no_use_idx` 提前搬。

所以双缓冲真正带来的收益不是“多一份存储”，而是：

- **计算和下一拍 transport 的时间重叠可能性**

---

## 11. pipebuffer 如何影响 Gemmini 实际看到的地址

对 stage compute 来说，最关键的不是 `buffer_id` 本身，而是：

- 当前 `in_use_idx` 对应的 `slot_pages`

`stage_tensor_current_pages()` 会从当前 pipebuf 里取出：

- `slot_pages[in_use_idx]`

然后 `stage_prepare_exec_views()` 再把这组 pages 绑定到当前 stage 的 alias VA window 上。

最终：

- Gemmini 看到的是 alias VA
- alias VA 对应哪组真实 SPM pages
- 取决于这次 stage 运行前，当前 pipebuf/ringbuf 的状态

所以 `pipebuf` 不是“只是给 DMA 用的”。
它实际上决定了：

- 本轮 Gemmini 的输入 tensor 映到哪组页
- 本轮 Gemmini 的输出 tensor 写到哪组页

---

## 12. 一份最实用的心智模型

如果你想不看代码也先把整个逻辑抓住，可以记成下面四句话：

1. `buffer_id` 是 exporter 在 segment 内顺序编号出来的运行时容器 id，runtime 不重排；`slot_id` 则是 runtime 按 `slot_count` 在容器内部展开出来的槽位号。
2. `prt_action_alloc_spm()` 负责把 `(buffer_id, slot_id)` 变成真实 page list；`prt_action_bind_topology()` 再把这些 page list 挂回 `pipebuf.slot_pages[]` 和 `ringbuf.slot_pages[]`。
3. 运行时真正做计算时，查页不再主要看 `buffer_id`，而是看“当前 stage 的当前 tensor 对应哪个 pipebuf，以及这个 pipebuf 当前的 `in_use_idx` 是多少”。
4. `pipebuf` 不只是存储对象，还是同步对象：`full / empty / cmd_running / subbatch_offset / ring head-tail / fanout / tag` 一起决定了这个 stage 什么时候能开始算、什么时候必须等、什么时候能把结果发给下游。

---

## 13. 建议阅读顺序

如果你后面要对照源码读，建议按这个顺序：

1. 先读 exporter 里的编号生成
   - `conference/HybridMapper/scripts/create-pipeline-runtime-artifacts.py`
   - 重点看 `build_segment_runtime_layout(...)`
2. 再读运行时静态合同
   - `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/include/prt_types.h`
3. 再读 action 分配与 topology 绑定
   - `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_schedule_action.c`
4. 再读执行期对象创建与当前页查询
   - `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_runtime.c`
5. 最后读 `C1..C8` 的具体 transport / 同步逻辑
   - `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_scheduler.c`

按这个顺序，你会先搞清楚“编号和对象关系”，再去看“状态机怎么跑”，不容易在 `C1..C8` 细节里迷路。

---

## 14. 相关源码位置

- exporter 编号与 binding 生成
  - `conference/HybridMapper/scripts/create-pipeline-runtime-artifacts.py`
- YAML 读入
  - `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_yaml_loader.c`
- 核心类型
  - `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/include/prt_types.h`
- action 分配 / alias group / page binding
  - `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_schedule_action.c`
- topology 构建 / pair 建立 / 当前页查询 / stage worker 主循环
  - `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_runtime.c`
- transport / ring / C1..C8 执行
  - `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_scheduler.c`
