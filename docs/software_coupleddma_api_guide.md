# Coupled DMA 用法与接口 API 导读

这份文档解释软件如何使用 Coupled DMA，尤其是 pipeline runtime 如何把它包装成 tensor / page copy 接口。当前 P12 pair-manager 主线下，DMA 和 Gemmini 共用同一个 pair manager id，但 DMA 指令走 `custom2`。

配合阅读：

- `generators/gemmini/software/gemmini-rocc-tests/include/rerocc_coupleddma.h`
- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/include/prt_dma.h`
- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/include/prt_types.h`
- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_dma.c`
- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_schedule_action.c`
- `docs/software_rerocc_api_guide.md`
- `docs/software_spm_xlate_api_guide.md`

---

## 1. DMA 的软件心智模型

Coupled DMA 对软件暴露的是一个很小的 RoCC 指令接口：

1. 设置目的地址和完成标志地址；
2. 设置源地址和拷贝长度；
3. 等待完成；
4. 可选读取监控计数器。

在 pair-manager 模式下，还要加上一层 ReRoCC 语义：

- 先 acquire 目标 pair manager；
- 绑定 `opcode_id = 2`，也就是 `custom2`；
- 再发 DMA 指令；
- 最后 fence / release。

---

## 2. 最底层 Coupled DMA 指令 API

入口文件：

- `generators/gemmini/software/gemmini-rocc-tests/include/rerocc_coupleddma.h`

这个头文件定义：

```c
#define REROCC_COUPLED_DMA_XCUSTOM 2
```

也就是说 Coupled DMA 使用 `custom2`。

### 2.1 `set_dst`

API：

```c
void rerocc_coupleddma_set_dst(uint64_t dst_addr,
                               uint64_t completion_addr);
```

底层 funct：

```text
funct = 2
```

含义：

- `dst_addr`：DMA 写入目的地址；
- `completion_addr`：硬件完成后写 completion flag 的地址。

pipeline runtime 通常会传 completion flag 的物理地址。

### 2.2 `set_src`

API：

```c
void rerocc_coupleddma_set_src(uint64_t src_addr,
                               uint64_t num_bytes);
```

底层 funct：

```text
funct = 1
```

含义：

- `src_addr`：DMA 读取源地址；
- `num_bytes`：拷贝字节数。

在 runtime 的硬件提交路径里，通常是先发 `set_dst()`，再发 `set_src()`。`set_src()` 可以理解成真正启动传输的那一下。

### 2.3 `wait`

API：

```c
uint64_t rerocc_coupleddma_wait(void);
```

底层 funct：

```text
funct = 3
```

含义：

- 等待 DMA 完成；
- 返回硬件 status。

pipeline runtime 中对应 `hw_dma_fence()`，会在 custom 指令前后插 CPU `fence`。

### 2.4 `read_monitor`

API：

```c
uint64_t rerocc_coupleddma_read_monitor(uint64_t stat_id);
```

底层 funct：

```text
funct = 4
```

用于读取 DMA 内部监控计数器，例如命令数、请求字节数、有效传输字节数、周期数等。

---

## 3. pipeline runtime 的 DMA 请求模型

入口文件：

- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/include/prt_types.h`
- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/include/prt_dma.h`

runtime 把一次 DMA 拷贝抽象成：

```c
typedef struct {
  uint64_t src_addr;
  uint64_t dst_addr;
  uint64_t bytes;
  uint32_t src_acc;
  uint32_t dst_acc;
} prt_dma_req_t;
```

字段含义：

- `src_addr`：源地址；
- `dst_addr`：目的地址；
- `bytes`：拷贝长度；
- `src_acc` / `dst_acc`：相关 manager id。

在当前 pair-manager 主线里，一般可以把 `src_acc` 和 `dst_acc` 都理解为 pair manager id。

### 3.1 DMA token

runtime 的异步/同步完成状态放在：

```c
typedef struct {
  uint32_t id;
  uint32_t stage_idx;
  uint32_t tensor_id;
  volatile uint32_t *completion_flag;
  uint64_t debug_src_addr;
  uint64_t debug_dst_addr;
  uint64_t debug_bytes;
  uint64_t debug_done_flag_pa;
  int rr_scope_valid;
  uint32_t rr_cfg_id;
  uint32_t rr_manager_id;
  uint32_t rr_opcode_id;
  ...
} prt_dma_token_t;
```

它记录三类东西：

- 传输本身：源、目的、长度；
- 完成状态：completion flag、done、status；
- ReRoCC 绑定：`cfg_id / manager_id / opcode_id`。

---

## 4. runtime 的 DMA backend

入口文件：

- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_dma.c`

runtime 有两个 DMA backend：

| backend | 含义 |
| --- | --- |
| `PRT_DMA_BACKEND_BLOCKING_FENCE` | 提交后用硬件 fence/wait 阻塞等待 |
| `PRT_DMA_BACKEND_POLL_PROGRESS_THREAD` | 提交后由 progress thread 轮询 completion flag |

初始化入口：

```c
int prt_dma_backend_init(prt_runtime_t *rt);
```

对外 API：

```c
int prt_dma_submit(prt_runtime_t *rt,
                   const prt_dma_req_t *req,
                   prt_dma_token_t *tok);

int prt_dma_wait(prt_runtime_t *rt,
                 prt_dma_token_t *tok,
                 uint64_t timeout_ns);

int prt_dma_submit_and_wait(prt_runtime_t *rt,
                            const prt_dma_req_t *req,
                            uint64_t timeout_ns);
```

### 4.1 当前 xlate 主线为什么常走 blocking

`spm_xlate_enable = 1` 时，runtime 会强制：

- `sync_mode = blocking_debug`
- `dma_backend = blocking_fence`
- `gemmini_mode = blocking_fence`

原因是当前 PTE / alias view 的更新是同步语义，runtime 需要避免 DMA/Gemmini 异步执行和 PTE 切换交错。

---

## 5. 一次硬件 DMA 提交实际做什么

核心提交函数在 `prt_dma.c` 的 `dma_blocking_submit()`。

一次提交的大致顺序是：

1. 初始化 token；
2. acquire ReRoCC scope：
   - `manager_id = req->dst_acc`
   - `opcode_id = 2`
3. 准备 completion flag；
4. 发 CPU memory fence；
5. 发 `custom2 funct=2`：
   - `set_dst(dst_addr, done_flag_pa)`
6. 发 `custom2 funct=1`：
   - `set_src(src_addr, bytes)`
7. 返回 token。

等待路径 `dma_blocking_wait()` 做：

1. 刷新 completion flag；
2. 调 `hw_dma_fence()`：
   - CPU fence；
   - `custom2 funct=3`；
   - CPU fence；
3. 再读 completion flag；
4. 释放 ReRoCC scope；
5. 返回状态。

所以底层指令顺序可以记成：

```text
ReRoCC acquire custom2
  set_dst(dst, done_flag_pa)
  set_src(src, bytes)
  wait/fence
ReRoCC release
```

---

## 6. page / tensor 级拷贝 API

pipeline runtime 很少直接手写 `prt_dma_req_t`，更多使用 page-level helper。

入口文件：

- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/include/prt_dma.h`

### 6.1 SPM 到 SPM

```c
int prt_dma_copy_spm_pages(prt_runtime_t *rt,
                           const prt_page_list_t *dst_pages,
                           const prt_page_list_t *src_pages,
                           uint32_t manager_id,
                           uint32_t stage_idx,
                           uint32_t tensor_id,
                           uint64_t timeout_ns);
```

用途：

- 把一组 SPM pages 拷贝到另一组 SPM pages；
- 每页生成一次或多次 DMA 请求；
- `manager_id` 是执行这次 DMA 的 pair manager / DMA manager。

### 6.2 DRAM 到 SPM

```c
int prt_dma_copy_dram_to_spm_pages(prt_runtime_t *rt,
                                   const prt_page_list_t *dst_pages,
                                   uint64_t src_dram_addr,
                                   uint32_t manager_id,
                                   uint32_t stage_idx,
                                   uint32_t tensor_id,
                                   uint64_t timeout_ns);
```

用途：

- 把 host/model/input 数据搬进 shared scratchpad pages。

在 Linux RISC-V 下，runtime 会进入专门的 host-to-SPM 路径，用 pagemap / physical address 信息处理 host buffer 到 SPM 的拷贝。

### 6.3 SPM 到 DRAM

```c
int prt_dma_copy_spm_pages_to_dram(prt_runtime_t *rt,
                                   uint64_t dst_dram_addr,
                                   const prt_page_list_t *src_pages,
                                   uint32_t manager_id,
                                   uint32_t stage_idx,
                                   uint32_t tensor_id,
                                   uint64_t timeout_ns);
```

用途：

- 把 stage 输出或 export tensor 从 SPM 拷回 DRAM / host-visible buffer。

### 6.4 alias VA 拷贝

```c
int prt_dma_copy_spm_va(prt_runtime_t *rt,
                        uint64_t dst_va,
                        uint64_t src_va,
                        uint64_t bytes,
                        uint32_t manager_id,
                        uint32_t stage_idx,
                        uint32_t tensor_id,
                        uint64_t timeout_ns);
```

用途：

- 输入是 SPM xlate alias VA；
- runtime 先把 alias VA 翻译成物理 segment；
- 再按 segment 发 DMA。

这个 API 是 SPM xlate 和 DMA 的交界点。

---

## 7. 和 action / stage 的关系

在 pipeline runtime 里，manager 分配发生在 action 阶段：

- `prt_action_alloc_acc()` 给每个 stage 分配 Gemmini manager id；
- 同时给每个 stage 分配 DMA manager id；
- pair-manager mode 下二者会映射到同一个 pair id。

关键规则在 `assign_stage_manager_slot()`：

```text
gm_id = prt_cfg_gemmini_manager_id(cfg, gm_local)
dm_id = prt_cfg_dma_manager_id(cfg, gm_local)
```

pair-manager mode 下：

```text
gm_id == dm_id
```

stage 执行时，runtime 会用 `exec->stage_dma_ids[stage_id]` 作为 DMA manager id，把固定输入、ring buffer、export 等数据搬到正确位置。

---

## 8. 阅读代码时的检查点

读 DMA 路径时建议按这个顺序检查：

1. 这次 DMA 的 `src_addr / dst_addr / bytes` 是什么？
2. 地址是 DRAM physical、SPM global physical，还是 SPM alias VA？
3. `manager_id` 是 pair id 还是 separate DMA id？
4. 是否 acquire 了 `opcode_id = 2`？
5. completion flag 的物理地址是否准备好了？
6. 发指令顺序是不是 `set_dst` 再 `set_src`？
7. 等待结束后是否 release ReRoCC scope？

在 P12 pair-manager 主线里，最重要的是第 3 和第 4 点：同一个 pair manager id 下，DMA 依靠 `custom2` 区分出来。
