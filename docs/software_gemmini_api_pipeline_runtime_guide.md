# Gemmini 用法与 pipeline runtime 接口导读

这份文档解释软件如何使用 Gemmini，重点不是完整复述 `gemmini.h` 的所有算子，而是说明 pipeline runtime 如何把模型层、SPM xlate alias、ReRoCC scope 和 Gemmini C API 串起来。

配合阅读：

- `generators/gemmini/software/gemmini-rocc-tests/include/gemmini.h`
- `generators/gemmini/software/gemmini-rocc-tests/include/gemmini_nn.h`
- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/include/prt_gemmini_adapter.h`
- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_gemmini_adapter.c`
- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_runtime.c`
- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/include/prt_types.h`
- `docs/software_rerocc_api_guide.md`
- `docs/software_spm_xlate_api_guide.md`

---

## 1. Gemmini 的软件层次

Gemmini 的软件接口可以分成四层：

1. **RoCC 指令层**
   - `custom3` + `funct`。
   - 直接发 `mvin/mvout/compute/config/preload/flush` 等指令。
2. **`gemmini.h` 宏层**
   - 把 RoCC 指令编码成 C 宏，例如 `gemmini_extended_mvin()`。
3. **`gemmini_nn.h` / 高层 tiling 层**
   - 提供 `tiled_conv_stride_auto()`、`tiled_matmul_nn_stride_auto()`、`tiled_resadd_auto()` 等高层调用。
4. **pipeline runtime adapter 层**
   - 把 runtime 的 stage / tensor / manager 分配翻译成 Gemmini 高层 API 调用。

pipeline runtime 主要使用第 4 层，最终落到第 3 层；只有在调试和 fence 时显式碰到部分第 2 层宏。

---

## 2. 最底层 Gemmini 指令 API

入口文件：

- `generators/gemmini/software/gemmini-rocc-tests/include/gemmini.h`

Gemmini 使用：

```text
XCUSTOM_ACC = 3
```

也就是 `custom3`。

### 2.1 关键 funct 编号

`gemmini.h` 里定义的核心 funct：

| funct 名 | 编号 | 含义 |
| --- | ---: | --- |
| `k_CONFIG` | `0` | 配置 load/store/execute/norm |
| `k_MVIN2` | `1` | mvin 通道 2 |
| `k_MVIN` | `2` | mvin 通道 0 |
| `k_MVOUT` | `3` | 从 Gemmini 写回内存 |
| `k_COMPUTE_PRELOADED` | `4` | 使用已 preload 的 B/D 执行 compute |
| `k_COMPUTE_ACCUMULATE` | `5` | 累加 compute |
| `k_PRELOAD` | `6` | preload B/D 和输出位置 |
| `k_FLUSH` | `7` | flush Gemmini |
| `k_LOOP_WS` | `8` | CISC weight-stationary matmul loop |
| `k_MVIN3` | `14` | mvin 通道 3，常用于 bias / D |
| `k_LOOP_CONV_WS` | `15` | CISC weight-stationary conv loop |

### 2.2 数据搬运宏

常用宏：

```c
gemmini_extended_mvin(dram_addr, spad_addr, cols, rows);
gemmini_extended_mvin2(dram_addr, spad_addr, cols, rows);
gemmini_extended_mvin3(dram_addr, spad_addr, cols, rows);
gemmini_extended_mvout(dram_addr, spad_addr, cols, rows);
```

直觉：

- `mvin` 把外部内存搬进 Gemmini scratchpad / accumulator；
- `mvout` 把 Gemmini 结果搬回外部内存；
- `mvin2/mvin3` 是额外 load 通道，常用来重叠 A/B/D 或 bias。

### 2.3 compute / preload 宏

常用宏：

```c
gemmini_extended_preload(BD, C, BD_cols, BD_rows, C_cols, C_rows);
gemmini_extended_compute_preloaded(A, BD, A_cols, A_rows, BD_cols, BD_rows);
gemmini_extended_compute_accumulated(A, BD, A_cols, A_rows, BD_cols, BD_rows);
```

直觉：

- `preload` 指定 B/D 以及输出 C 的位置；
- `compute_preloaded` 使用 preload 好的 B/D；
- `compute_accumulated` 在 accumulator 上继续累加。

### 2.4 配置宏

常用宏：

```c
gemmini_extended_config_ex(...);
gemmini_extended3_config_ld(...);
gemmini_extended_config_st(...);
gemmini_config_norm(...);
```

它们都通过 `k_CONFIG` 发给 Gemmini，但 `CONFIG_EX / CONFIG_LD / CONFIG_ST / CONFIG_BERT` 子类型不同：

- `CONFIG_EX`：数据流、activation、transpose、execute 参数；
- `CONFIG_LD`：load stride、scale、load 通道 id；
- `CONFIG_ST`：store stride、activation、pooling、scale；
- `CONFIG_BERT`：BERT 相关 norm / softmax / igelu 参数。

### 2.5 loop 宏

`gemmini_loop_ws()` 和 `gemmini_loop_conv_ws()` 是更高层的 CISC 指令序列：

- 先发多条 config 指令；
- 最后发一条 run 指令；
- 硬件内部展开循环。

pipeline runtime 当前对 canonical pointwise conv 比较谨慎：某些 1x1 conv 会避开 `loop_ws`，改走 matmul fallback 路线。

### 2.6 fence / flush

常用宏：

```c
gemmini_flush(0);
gemmini_fence();
```

注意区别：

- `gemmini_flush(0)` 是发给 Gemmini 的 custom3 指令；
- `gemmini_fence()` 是 CPU `fence`；
- pipeline runtime 通常还会配合 `prt_rr_fence_scope()` 等待 ReRoCC scope。

---

## 3. pipeline runtime 给 Gemmini 的抽象

入口文件：

- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/include/prt_gemmini_adapter.h`

runtime 不直接把 YAML layer 传给 Gemmini，而是先构造 descriptor。

### 3.1 Conv descriptor

```c
typedef struct {
  int batch_size;
  int in_row_dim;
  int in_col_dim;
  int in_channels;
  int out_channels;
  int out_row_dim;
  int out_col_dim;
  int in_stride;
  int weight_stride;
  int out_stride;
  int groups;
  int stride;
  int input_dilation;
  int kernel_dilation;
  int padding;
  int kernel_dim;
  const void *input;
  const void *weights;
  const void *bias;
  void *output;
  int act;
  float output_scale;
  int pool_size;
  int pool_stride;
  int pool_padding;
  int tiled_type;
} prt_gemmini_conv_desc_t;
```

你可以把它看成 runtime 对 Gemmini conv API 的参数包。

最关键字段：

- `input / weights / bias / output`
  - 可能是 DRAM 地址；
  - 也可能是 SPM xlate alias VA。
- `tiled_type`
  - `0 = OS`
  - `1 = WS`
  - `2 = CPU`

当前 runtime 默认 conv 用 `tiled_type = 1`，也就是 WS。

### 3.2 Resadd descriptor

```c
typedef struct {
  size_t I;
  size_t J;
  float A_scale;
  float B_scale;
  float C_scale;
  size_t stride;
  const void *A;
  const void *B;
  void *C;
  int relu;
  int tiled_type;
} prt_gemmini_resadd_desc_t;
```

runtime 把 resadd 看成一个矩阵形状的元素级加法。

在 RISC-V 目标上，当前代码明确不走历史 CPU fallback，而是保持 Gemmini 路径。

### 3.3 Task descriptor

`prt_conv_task_t` 是 runtime 调度层给 adapter 的任务：

```c
typedef struct {
  uint32_t stage_id;
  uint32_t acc_id;
  uint32_t num_managers;
  uint32_t manager_ids[PRT_MAX_CORES];
  uint32_t tile_count;
  prt_layer_split_t split_kind;
  prt_stage_op_t op_kind;
  void *opaque_task;
} prt_conv_task_t;
```

它回答：

- 哪个 stage；
- 使用哪些 Gemmini manager；
- 是否切分；
- 是 conv 还是 resadd；
- 真正参数包在哪里。

---

## 4. runtime 如何构造 Gemmini descriptor

入口文件：

- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_runtime.c`

### 4.1 `build_stage_conv_desc()`

这个函数做的是：

1. 找到当前 stage 对应的 model layer；
2. 从 `layer->param[]` 里读出 `N / IC / OC / OH / OW / KH / KW / G / stride`；
3. 推导 input height、input width、padding；
4. 调 `stage_prepare_exec_views()` 准备 stage 的执行视图；
5. 调 `stage_tensor_exec_addr()` 得到 bias、weight、input、output 的执行地址；
6. 填充 `prt_gemmini_conv_desc_t`。

最关键的是第 4 和第 5 步。

如果 `spm_xlate_enable = 1`，`stage_tensor_exec_addr()` 返回的是 alias VA：

```text
action->alias_base_va
  + stage->exec_base_vpage * page_bytes
  + stage->local_spm_tensor_addr[slot]
```

所以 Gemmini 最后拿到的是“像 DRAM 指针一样”的地址，但硬件侧会经 SPM xlate 翻译到 shared scratchpad。

### 4.2 `build_stage_resadd_desc()`

这个函数做的是：

1. 找到 resadd layer；
2. 准备 stage 执行视图；
3. 解析 A/B/C 三个 tensor 的执行地址；
4. 推导 `I / J / stride`；
5. 填充 `prt_gemmini_resadd_desc_t`。

### 4.3 `build_stage_task_desc()`

这个函数把 layer type 分发成 runtime task：

- `type == "conv"`：
  - 构造 `prt_gemmini_conv_desc_t`
  - `task->op_kind = PRT_STAGE_OP_CONV`
  - `task->opaque_task = conv_desc`
- `type == "resadd"`：
  - 构造 `prt_gemmini_resadd_desc_t`
  - `task->op_kind = PRT_STAGE_OP_RESADD`
  - `task->opaque_task = resadd_desc`

---

## 5. stage worker 如何调用 Gemmini

runtime 有 host serial 和 worker 两条执行路径，但核心一样：

1. 填 `prt_conv_task_t`；
2. 填 `manager_ids[]`；
3. 调 `build_stage_task_desc()`；
4. 调 `prt_gemm_conv_run()`；
5. 调 `prt_gemm_fence()`；
6. export / sync 输出。

worker 路径里，关键调用是：

```c
rc = prt_gemm_conv_run(rt, &task, timeout_ns);
```

这里名字叫 `conv_run`，但实际可以处理：

- conv；
- resadd；
- CPU backend fallback 场景。

---

## 6. Gemmini backend 模式

入口文件：

- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_gemmini_adapter.c`

初始化：

```c
int prt_gemmini_backend_init(prt_runtime_t *rt);
```

可选模式：

| 模式 | 含义 |
| --- | --- |
| `PRT_GEMMINI_MODE_BLOCKING_FENCE` | issue 后立即走 fence |
| `PRT_GEMMINI_MODE_ASYNC_EXPERIMENTAL` | issue 与 fence 分离，实验性 |

注意：

- `main.c` 默认填的是 `async_experimental`；
- 但 `spm_xlate_enable = 1` 时，`prt_runtime_init()` 会强制切到 `blocking_fence`；
- 当前 P12 pairdummy/sbus128 主线通常应按保守 blocking 路线理解。

---

## 7. adapter 如何真正调用 Gemmini

核心入口：

```c
int prt_gemm_conv_run(prt_runtime_t *rt,
                      const prt_conv_task_t *task,
                      uint64_t timeout_ns);
```

调用链：

```text
prt_gemm_conv_run()
  -> rt->gemm_ops.conv_run()
  -> gemm_blocking_conv_run() 或 gemm_async_conv_run()
  -> gemm_issue_task()
  -> gemm_issue_grouped_conv_task()
  -> gemm_issue_conv_task()
  -> conv_call_for_manager_nb/sync()
```

### 7.1 `gemm_issue_task()`

这个函数按 `task->op_kind` 分发：

- `PRT_STAGE_OP_CONV`
  - 走 conv 路线；
- `PRT_STAGE_OP_RESADD`
  - 走 resadd 路线。

它还会检查：

- `task->num_managers != 0`
- RISC-V 上是否必须保持 Gemmini 路线
- CPU backend 是否走 host reference。

### 7.2 `gemm_issue_conv_task()`

这个函数按 split kind 分发：

- `PRT_LAYER_SPLIT_SINGLE`
  - 只用一个 manager；
- `PRT_LAYER_SPLIT_OC`
  - 按输出通道切分；
- `PRT_LAYER_SPLIT_SPATIAL`
  - 按输出空间区域切分；
- `PRT_LAYER_SPLIT_UNSPEC`
  - 根据 tile 数和 out_channels 自动选 single / OC / spatial。

### 7.3 每个 manager 的调用

真正发 Gemmini 指令前会先 acquire ReRoCC scope：

```c
prt_rr_acquire_scope(NULL, stage_id, manager_id, 3, &scope);
```

这里 `opcode_id = 3`，说明后续 `custom3` 都路由到这个 manager。

之后 runtime 选择实际 Gemmini路径：

- canonical pointwise conv：
  - 走 `tiled_matmul_nn_stride_auto()` fallback；
- 其他 conv：
  - 走 `tiled_conv_stride_auto()`；
  - 或 `tiled_conv_stride_auto_capped_kchs()`；
- resadd：
  - 走 resadd 相关 Gemmini helper。

完成后 release scope。

---

## 8. pipeline runtime 为什么特别处理 pointwise conv

在 adapter 中，canonical pointwise conv 的判断大致是：

- `kernel_dim == 1`
- `stride == 1`
- `padding == 0`
- 无 pooling / dilation / transpose / rotation
- input/output 空间维度一致

这类 conv 本质上可以看成矩阵乘：

```text
I = batch_size * out_row_dim * out_col_dim
J = out_channels
K = in_channels
```

runtime 会优先走：

```c
tiled_matmul_nn_stride_auto(...)
```

而不是直接走 `gemmini_loop_ws()`。

代码里写明了这个设计动机：

- live FireSim 运行中，canonical 1x1 conv fallback 进入 `gemmini_loop_ws` 后曾在 LOOP_WS run command 上 stall；
- 因此这条窄路径保持在 Gemmini 上，但从 WS loop 切到 OS matmul fallback；
- CPU fallback 在 bertmini live 目标上不是默认允许路线。

这也是为什么读 Gemmini runtime 时不能只看 `tiled_conv_stride_auto()`：当前最重要的一批 pointwise conv 很可能走的是 matmul fallback。

---

## 9. Gemmini 与 SPM xlate 的关系

Gemmini 本身不知道 runtime 的 tensor / page allocator。它只看到指针。

当 `spm_xlate_enable = 1` 时，runtime 保证：

1. action 已经安装 xlate context；
2. 当前 stage 的 PTE 已经绑定到正确 SPM pages；
3. stage 关联的 Gemmini managers 已经 flush xlate；
4. `conv_desc.input / weights / bias / output` 是 alias VA；
5. Gemmini 发 mvin/mvout 时，硬件 xlate 把 alias VA 翻译到 shared scratchpad。

这条链路可以记成：

```text
model layer tensor id
  -> runtime page allocation
  -> SPM xlate PTE
  -> stage_tensor_exec_addr() alias VA
  -> prt_gemmini_conv_desc_t
  -> tiled_conv/tiled_matmul API
  -> custom3 Gemmini instructions
```

---

## 10. Gemmini 与 ReRoCC 的关系

在 P12 pair-manager 模式下：

- Gemmini manager id 是 pair manager id；
- DMA manager id 也是同一个 pair manager id；
- Gemmini 和 DMA 靠 opcode 区分。

Gemmini 路径必须：

```text
acquire manager_id with opcode_id = 3
emit custom3 Gemmini instructions
fence / flush
release
```

DMA 路径则是：

```text
acquire same manager_id with opcode_id = 2
emit custom2 DMA instructions
wait / fence
release
```

这就是为什么 pair-manager 主线下不能只看 manager id，还必须看 `opcode_id`。

---

## 11. 推荐阅读顺序

如果你要精读 pipeline runtime 怎么用 Gemmini，建议按下面顺序：

1. `prt_gemmini_adapter.h`
   - 先看 `prt_gemmini_conv_desc_t`、`prt_gemmini_resadd_desc_t`、`prt_gemmini_ops_t`。
2. `prt_runtime.c`
   - 看 `build_stage_conv_desc()`；
   - 再看 `build_stage_resadd_desc()`；
   - 再看 worker 里怎么填 `prt_conv_task_t`。
3. `prt_gemmini_adapter.c`
   - 看 `prt_gemmini_backend_init()`；
   - 看 `prt_gemm_conv_run()`；
   - 看 `gemm_issue_task()`；
   - 看 `gemm_issue_conv_task()`；
   - 看 `conv_call_for_manager_nb()`。
4. `gemmini.h`
   - 只看 funct 定义；
   - 再看 `mvin/mvout/config/preload/compute/fence` 宏；
   - 最后看 `gemmini_loop_ws()` 和 `gemmini_loop_conv_ws()`。
5. `gemmini_nn.h`
   - 顺着 adapter 调用去看 `tiled_conv_stride_auto()`、`tiled_matmul_nn_stride_auto()`、`tiled_resadd_auto()`。

---

## 12. 阅读代码时的检查点

读 Gemmini 调用路径时，建议每次都问：

1. 当前 stage 的 `task.op_kind` 是 conv 还是 resadd？
2. `task.manager_ids[]` 是哪些 pair manager？
3. 是否已经 acquire `opcode_id = 3`？
4. descriptor 里的地址是 DRAM 地址还是 SPM alias VA？
5. 如果是 SPM alias VA，PTE 是否已经绑定并 flush？
6. 当前 conv 是普通 conv，还是 canonical pointwise conv？
7. dispatch 走的是 `tiled_conv_stride_auto()` 还是 `tiled_matmul_nn_stride_auto()`？
8. issue 后有没有 `prt_rr_fence_scope()`、`gemmini_flush()`、`gemmini_fence()`？

能按这 8 个问题走一遍，pipeline runtime 如何使用 Gemmini 接口就基本清楚了。
