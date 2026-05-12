# 软件视角的 ReRoCC 用法

这份文档只从软件角度解释 ReRoCC：软件如何选择一个 manager、如何把某个 custom opcode 绑定到它、如何发指令、如何等待、如何释放。硬件实现细节只在必要时点到为止。

配合阅读：

- `generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests/rerocc_control.h`
- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/include/prt_rerocc.h`
- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_rerocc.c`
- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/include/prt_types.h`
- `docs/software_spm_xlate_api_guide.md`
- `docs/software_coupleddma_api_guide.md`
- `docs/software_gemmini_api_pipeline_runtime_guide.md`

---

## 1. 先建立软件心智模型

ReRoCC 对软件来说不是一个普通函数库，而是一个**动态路由层**：

- 软件先拿到一个 `manager_id`，表示目标硬件 endpoint。
- 软件再拿到一个临时 `cfg_id`，表示当前 hart/线程绑定到哪个 manager。
- 软件把某个 RISC-V custom opcode 绑定到这个 `cfg_id`。
- 之后发出的 `custom2` / `custom3` 指令，就会被 ReRoCC 路由到对应 manager。
- 用完后，软件 fence 并 release 这个 `cfg_id`。

不要把这几个概念混在一起：

- `manager_id`：硬件里某个 ReRoCC manager 的全局编号。
- `cfg_id`：软件侧临时占用的配置槽，最多 `RR_MAX_CFGS = 16` 个。
- `opcode_id`：custom opcode 编号；当前主线最重要的是 `2` 和 `3`。
- `funct`：custom 指令内部的小功能号，例如 DMA 的 `set_src`、Gemmini 的 `k_MVIN`。

一句话总结：

> ReRoCC 先用 CSR 把 `custom opcode -> cfg slot -> manager` 这条路由设好，然后普通 RoCC 指令才会被送到正确的 manager。

---

## 2. 最底层 API：`rerocc_control.h`

入口文件：

- `generators/gemmini/software/gemmini-rocc-tests/rerocc-linux-tests/rerocc_control.h`

### 2.1 CSR 资源

这个文件定义了两类 CSR：

- `CSR_RROPC0` 到 `CSR_RROPC3`
  - opcode 绑定表。
  - 软件用它说明：`custom0/1/2/3` 当前应该走哪个 `cfg_id`。
- `CSR_RRCFG0` 到 `CSR_RRCFG15`
  - manager acquire / release 配置槽。
  - 每个槽可以临时绑定一个 `manager_id`。

相关宏：

- `RR_CFG_ACQ_MASK = 0x100`
  - 写入时表示 acquire 请求。
  - 读回时用来判断是否 acquire 成功。
- `RR_CFG_MGR_MASK = 0x0ff`
  - manager id 低 8 位。
- `RR_MAX_CFGS = 16`
  - runtime 最多同时管理 16 个配置槽。

### 2.2 基本操作

最底层读写 CSR：

- `rr_read_csr(csr_id)`
- `rr_write_csr(csr_id, wdata)`
- `rr_swap_csr(csr_id, wdata)`

普通代码一般不直接用它们，而是用下面几个封装。

### 2.3 acquire manager

API：

```c
bool rr_acquire_cfg(uint32_t cfg_id, uint64_t manager_id);
```

行为：

1. 选择 `CSR_RRCFG0 + cfg_id`；
2. 写入 `RR_CFG_ACQ_MASK | manager_id`；
3. 再读回这个 CSR；
4. 如果读回值里 `RR_CFG_ACQ_MASK` 还在，就表示 acquire 成功。

这里要注意：

- acquire 的目标是 `manager_id`。
- acquire 成功后占用的是 `cfg_id`。
- `cfg_id` 不是 manager id，只是本 hart 当前使用的路由槽。

### 2.4 bind opcode

API：

```c
void rr_set_opc(uint8_t opcode_id, uint32_t cfg_id);
```

行为：

- 写 `CSR_RROPC0 + opcode_id = cfg_id`。

举例：

- `rr_set_opc(2, cfg_id)`：把 `custom2` 绑定到这个 cfg。
- `rr_set_opc(3, cfg_id)`：把 `custom3` 绑定到这个 cfg。

在当前 P12 pair-manager 主线里：

- DMA 使用 `opcode_id = 2`，也就是 `custom2`。
- Gemmini 和 SPM xlate 使用 `opcode_id = 3`，也就是 `custom3`。

### 2.5 fence 和 release

API：

```c
void rr_fence(uint32_t cfg_id);
void rr_release(uint32_t cfg_id);
void rr_release_all(size_t n_cfgs);
```

语义：

- `rr_fence(cfg_id)`：
  - 写 `CSR_RRBAR = cfg_id`；
  - 再执行 CPU `fence`；
  - 用来等待这个 cfg 路由下的请求完成。
- `rr_release(cfg_id)`：
  - 写 `CSR_RRCFGx = 0`；
  - 表示释放这个 cfg slot。

典型顺序是：

1. acquire cfg；
2. bind opcode；
3. 发 custom 指令；
4. fence；
5. release。

---

## 3. pipeline runtime 的 ReRoCC 封装

入口文件：

- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/include/prt_rerocc.h`
- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_rerocc.c`

runtime 不直接到处调用 `rr_acquire_cfg()`，而是把一次 ReRoCC 使用封装成 `prt_rr_scope_t`。

### 3.1 `prt_rr_scope_t`

结构：

```c
typedef struct {
  int valid;
  uint32_t cfg_id;
  uint32_t stage_id;
  uint32_t manager_id;
  uint32_t opcode_id;
} prt_rr_scope_t;
```

它记录一次已建立的 ReRoCC 路由：

- `cfg_id`：占到的 ReRoCC 配置槽；
- `stage_id`：哪个 pipeline stage 在使用；
- `manager_id`：目标 manager；
- `opcode_id`：这次绑定的是 `custom2` 还是 `custom3`；
- `valid`：这个 scope 是否还活着。

### 3.2 scope API

主要 API：

```c
int prt_rr_acquire_scope(prt_runtime_t *rt,
                         uint32_t stage_id,
                         uint32_t manager_id,
                         uint32_t opcode_id,
                         prt_rr_scope_t *scope);

int prt_rr_fence_scope(prt_rr_scope_t *scope);
int prt_rr_release_scope(prt_rr_scope_t *scope);
```

对应关系：

- `prt_rr_acquire_scope()`
  - 选一个 `cfg_id`；
  - acquire 目标 `manager_id`；
  - 调 `rr_set_opc(opcode_id, cfg_id)`；
  - 把结果填进 `scope`。
- `prt_rr_fence_scope()`
  - 对 `scope->cfg_id` 做 `rr_fence()`。
- `prt_rr_release_scope()`
  - 对 `scope->cfg_id` 做 `rr_release()`；
  - 清掉 `scope->valid`。

### 3.3 runtime 如何选择 `cfg_id`

`prt_rerocc.c` 里有一个关键规则：

```c
lane = opcode_id == 2 ? 0 : 1;
cfg_id = ((stage_id * 2) + lane) % RR_MAX_CFGS;
```

直观理解：

- 每个 stage 默认有两条 ReRoCC lane。
- lane 0 给 `custom2`，也就是 DMA。
- lane 1 给 `custom3`，也就是 Gemmini / SPM xlate。
- 最后对 `RR_MAX_CFGS = 16` 取模。

这就是为什么 runtime 可以让一个 stage 同时使用 DMA 和 Gemmini：

- DMA scope：`opcode_id = 2`
- Gemmini scope：`opcode_id = 3`

它们可以落在不同 `cfg_id` 上。

---

## 4. P12 pair-manager 模式下怎么理解

当前重点配置是：

```text
GemminiLearningConfigSpadReRoCCGlobalNoC4C2x2P12x4x3CoupledDMAPairManagerDummy16x16Sbus128
```

在这条主线里，最容易误解的是 manager 编号。

### 4.1 pair-manager 的软件语义

pair-manager 模式下：

- `num_gemmini_mgrs = 12`
- `num_dma_mgrs = 12`
- `gemmini_mgr_base_id = dma_mgr_base_id`
- `pair_manager_mode = 1`

但这不表示硬件里有两组独立 manager。软件应理解为：

- local id `i` 对应第 `i` 个 pair manager；
- Gemmini 指令和 DMA 指令都发给同一个 `manager_id`；
- 区分 Gemmini / DMA 的不是 manager id，而是 incoming opcode：
  - `custom3` 走 Gemmini；
  - `custom2` 走 Coupled DMA。

runtime 中这个规则体现在：

- `prt_cfg_dma_mgr_count(cfg)`
  - pair mode 下返回 `num_gemmini_mgrs`；
- `prt_cfg_dma_manager_id(cfg, local_idx)`
  - pair mode 下返回 `gemmini_mgr_base_id + local_idx`；
- `prt_cfg_gemmini_manager_id(cfg, local_idx)`
  - 返回 `gemmini_mgr_base_id + local_idx`。

因此 pair mode 下：

```text
gemmini manager id == dma manager id == pair manager id
```

### 4.2 为什么 opcode 很重要

pair manager 内部同时支持：

- `custom2`：Coupled DMA；
- `custom3`：Gemmini / SPM xlate。

所以软件必须保证：

- 发 DMA 前绑定 `opcode_id = 2`；
- 发 Gemmini 前绑定 `opcode_id = 3`；
- 不要把二者当成两个 manager id 空间。

这也解释了硬件配置里为什么需要保留 incoming opcode：如果 ReRoCC manager 在入口把 opcode 改写掉，pair wrapper 内部就无法区分 `custom2` 和 `custom3`。

---

## 5. 典型调用模式

### 5.1 DMA 调用模式

伪代码：

```c
prt_rr_scope_t scope;

prt_rr_acquire_scope(rt, stage_id, pair_manager_id, 2, &scope);

// 发 custom2 指令：
//   set_dst(dst_addr, completion_flag_pa)
//   set_src(src_addr, bytes)
//   wait/fence

prt_rr_fence_scope(&scope);
prt_rr_release_scope(&scope);
```

实际封装见：

- `docs/software_coupleddma_api_guide.md`

### 5.2 Gemmini 调用模式

伪代码：

```c
prt_rr_scope_t scope;

prt_rr_acquire_scope(rt, stage_id, pair_manager_id, 3, &scope);

// 发 custom3 指令：
//   gemmini_flush()
//   tiled_conv_stride_auto()
//   tiled_matmul_nn_stride_auto()
//   tiled_resadd_auto()

prt_rr_fence_scope(&scope);
gemmini_fence();
prt_rr_release_scope(&scope);
```

实际封装见：

- `docs/software_gemmini_api_pipeline_runtime_guide.md`

### 5.3 SPM xlate 调用模式

SPM xlate 也是 `custom3`，但这里最容易误解的点是：

- SPM xlate 不是“另一条独立于 Gemmini 的 opcode 通道”；
- 它和 Gemmini 共享的是 **同一个 `custom3` 入口**；
- 二者的区别不是靠 opcode，而是靠 **进入 Gemmini 之后的 `funct` 编码**。

你可以把这件事拆成 3 层来看。

#### 第 1 层：ReRoCC / pair wrapper 这一层，只看 opcode

对 pair wrapper 来说：

- `custom2` → DMA
- `custom3` → Gemmini 这一侧

也就是说，wrapper 根本不区分“这是 Gemmini compute”还是“这是 SPM xlate control”。
只要是 `custom3`，都会先被送进 Gemmini 这一侧。

所以从 wrapper 视角看：

```text
custom2 -> DMA
custom3 -> Gemmini-side
```

而不是：

```text
custom2 -> DMA
custom3 -> Gemmini
custom4 -> SPM xlate
```

后面这种想象是错的；SPM xlate 没有自己独立的 opcode lane。

#### 第 2 层：Gemmini controller 这一层，再看 `funct`

进入 Gemmini 侧以后，控制器再根据 `inst.funct` 继续细分：

- 普通 Gemmini flush / compute / counter / config 走各自已有 `funct`
- SPM xlate 使用专门的 `funct`
  - `23` = `cfg`
  - `24` = `range`
  - `25` = `flush`
  - `26` = `fault`

所以真正的层次关系是：

```text
先看 opcode:
  custom3 -> 进入 Gemmini side

再看 funct:
  funct=23/24/25/26 -> SPM xlate 控制
  其它 Gemmini funct -> 正常 Gemmini 指令
```

这就是为什么说：

- **Gemmini 和 SPM xlate 共用 `custom3`**
- 但它们并不会互相“混淆成同一条指令”

因为二者是在不同层次解码的：

- wrapper 用 opcode 选“走哪一边”
- Gemmini controller 用 funct 选“在 Gemmini 这边具体做什么”

#### 第 3 层：ReRoCC 软件绑定这一层，为什么还要“临时借用” `opcode_id = 3`

虽然 Gemmini 和 SPM xlate 都走 `custom3`，但 ReRoCC 的软件绑定模型仍然是：

- 一个 `cfg slot`
- 绑定一个 `opcode_id`
- 指向一个 `manager_id`

所以 runtime 在某个时刻如果要给 manager 7 安装 xlate，它仍然需要先建立：

- “`custom3` 现在绑定到 manager 7”的一段有效 scope

这就是 `prt_spm_xlate_acquire_scope()` / `prt_gemmini_spm_xlate_program()` 那套逻辑在做的事情。

runtime 为了避免破坏已有 Gemmini 绑定，会：

1. 保存旧的 `CSR_RROPC3`
2. 用专用 cfg slot acquire 目标 manager
3. 调 `rr_set_opc(3, cfg_id)`，让当前 `custom3` scope 临时指向这个 manager
4. 连续发 `custom3 + funct=23/24/25(/26)` 的 xlate 指令
5. `fence / release`
6. 恢复旧的 `CSR_RROPC3`

所以“SPM xlate 和 Gemmini 共用 `custom3`”的准确理解是：

- **硬件入口层面**：它们确实共享同一个 opcode
- **Gemmini 内部功能层面**：靠 `funct` 区分
- **ReRoCC 绑定层面**：runtime 需要暂时把 `custom3` 绑定到目标 manager，再发那几条 xlate 指令

#### 这 3 个动作在代码里到底怎么做

1. **保存旧的 `CSR_RROPC3`**
   - `prt_spm_xlate_acquire_scope()` 会先读 `rr_read_opcode_binding(3U)`
   - 这个 helper 本质上就是读 `CSR_RROPC3`
   - 旧值保存在 `prev_binding`

2. **“专用 cfg slot” acquire**
   - 普通 stage 路径走 `prt_rr_acquire_scope()`
   - 它会按 `rr_cfg_id_for_stage(stage_id, opcode_id)` 计算 `cfg_id`
   - 但 SPM xlate 不走这条公式，而是直接调用：
     - `prt_rr_acquire_scope_cfg(NULL, PRT_RR_SPM_XLATE_CFG_ID, UINT32_MAX, manager_id, 3U, scope)`
   - 当前实现里：
     - `RR_MAX_CFGS = 16`
     - `PRT_RR_SPM_XLATE_CFG_ID = RR_MAX_CFGS - 1 = 15`
   - 所以所谓“专用 cfg slot”，当前实际上就是 **xlate helper 固定直接使用 `cfg15`**

3. **恢复旧的 `CSR_RROPC3`**
   - `prt_spm_xlate_release_scope()` 的顺序是：
     - `rr_fence(scope->cfg_id)`
     - `prt_rr_release_scope(scope)`
     - `rr_restore_opcode_binding(3U, prev_binding)`
   - 最后这一步就是把旧的 `cfg_id` 写回 `CSR_RROPC3`

可以把它压缩成下面这条真实执行链：

```text
old_cfg = read CSR_RROPC3
acquire cfg15 -> manager_id
write CSR_RROPC3 = 15
emit custom3 + funct=23/24/25(/26)
fence cfg15
release cfg15
write CSR_RROPC3 = old_cfg
```

#### 当前实现风险：`cfg15` 并没有被真正保留出来

这里要特别小心，当前代码虽然把 `cfg15` 当成 xlate helper 的固定 slot，但**普通 stage 路径并没有把 `cfg15` 从映射空间里剔除**。

普通路径的映射公式是：

```text
rr_cfg_id_for_stage(stage_id, opcode_id)
  = ((stage_id * 2) + lane) % RR_MAX_CFGS

lane = 0  for opcode_id = 2
lane = 1  for opcode_id = 3
```

所以对普通 `custom3` stage 而言：

```text
stage_id = 7
opcode_id = 3
=> cfg_id = ((7 * 2) + 1) % 16 = 15
```

也就是说：

- xlate helper 固定使用 `cfg15`
- 普通 Gemmini/custom3 scope 也可能映射到 `cfg15`

因此当前“专用 cfg slot”的准确含义其实是：

- **xlate helper 约定自己总是去用 `cfg15`**
- 但**并没有一个真正的 allocator / 保留机制保证别人绝不会用 `cfg15`**

这应被视为一个实现层面的真实风险，而不是已经解决的约束。

#### 一个最实用的阅读图

```text
ReRoCC scope:
  acquire(manager=7, opcode=3)
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

#### 这也解释了一个常见误解

`preserveIncomingOpcode = true` 解决的是：

- wrapper 必须分清 `custom2` 和 `custom3`

它**不是**用来区分：

- Gemmini compute 和 SPM xlate

因为 Gemmini compute 和 SPM xlate 本来就故意共用 `custom3`；
它们在 wrapper 之后，靠 `funct` 再区分。

实际封装见：

- `docs/software_spm_xlate_api_guide.md`

---

## 6. 阅读代码时最重要的检查点

读 ReRoCC 软件栈时，建议每次都问 5 个问题：

1. 这次操作的 `manager_id` 是多少？
2. 当前是不是 pair-manager mode？
3. 这次绑定的是 `opcode_id = 2` 还是 `opcode_id = 3`？
4. `cfg_id` 是临时 slot，还是被误当成 manager id 了？
5. 发完指令后有没有 fence / release？

只要这 5 个问题能答上，ReRoCC 的软件用法就基本读顺了。
