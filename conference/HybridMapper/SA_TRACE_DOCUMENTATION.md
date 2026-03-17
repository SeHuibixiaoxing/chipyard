# SA Search Tracing Feature

## 概述

本文档介绍了为 `GraphPartitionSimulatedAnnealing` 类添加的详细跟踪（tracing）功能。该功能可以记录模拟退火搜索的完整过程，并将其输出为 YAML 文件。

## 主要特性

### 1. 完整的搜索过程记录

跟踪文件包含以下信息：

- **模型参数**：每层的输入/输出/权重张量信息，包括：
  - `tensor_id`: 张量ID
  - `size`: 原始大小
  - `size_aligned`: 页对齐后的大小

- **模型拓扑**：层之间的连接关系
  - `nodes`: 所有层的索引和类型
  - `edges`: 张量流动路径（from_layer → to_layer）

- **每步搜索细节**：
  - `step`: 步数
  - `temperature`: 当前温度
  - `energy`: 当前能量
  - `acceptance_rate`: 接受率（累计）
  - `improvement_rate`: 改进率（累计）
  - `op`: 执行的算子及其参数
  - `state`: 当前映射方案的完整状态

- **最优解**：搜索结束后的最佳状态和能量

### 2. 算子变化编码

每个算子都有唯一的类型ID和详细的变化记录：

#### Type 0: move_acc (移动加速器)
```yaml
type_id: 0
type: move_acc
segment_idx: <段索引>
from_layer_idx: <源层索引>
to_layer_idx: <目标层索引>
from_acc:
  from: <原加速器数>
  to: <新加速器数>
to_acc:
  from: <原加速器数>
  to: <新加速器数>
```

#### Type 1: add_acc (增加加速器)
```yaml
type_id: 1
type: add_acc
segment_idx: <段索引>
layer_idx: <层索引>
from: <原加速器数>
to: <新加速器数>
```

#### Type 2: remove_acc (减少加速器)
```yaml
type_id: 2
type: remove_acc
segment_idx: <段索引>
layer_idx: <层索引>
from: <原加速器数>
to: <新加速器数>
```

#### Type 3: change_tensor_meta_type (改变张量元类型)
```yaml
type_id: 3
type: change_tensor_meta_type
segment_idx: <段索引>
tensor_id: <张量ID>
from_meta_type: <原元类型名称>
to_meta_type: <新元类型名称>
from_tensor_type: <原张量类型名称>
to_tensor_type: <新张量类型名称>
affected_layer_indices: [<受影响的层索引列表>]
```

#### Type 4: change_tensor_type (改变张量类型)
```yaml
type_id: 4
type: change_tensor_type
segment_idx: <段索引>
stage_idx: <阶段索引>
tensor_id: <张量ID>
from: <原类型名称>
to: <新类型名称>
propagate: <是否传播到其他层>
propagate_layers: [<传播到的层索引列表>]
```

## 使用方法

### 基本用法

```python
from HybridMapper.SASearch import GraphPartitionSimulatedAnnealing, PipelineTarget

# 1. 创建 SA 实例（与之前相同）
sa = GraphPartitionSimulatedAnnealing(
    pipeline_target=pipeline_target,
    partition_plan=partition_plan,
    acc_plan=acc_plan,
    layer_mapping=layer_mapping,
    max_iterations=10000
)

# 2. 使用 run_with_trace 方法运行并记录
trace_path = "output/sa_trace.yaml"
best_state, best_energy = sa.run_with_trace(trace_path)

# 3. 结果与 anneal() 方法相同
print(f"Best energy: {best_energy}")
```

### 在现有脚本中集成

可以在 `scripts/create-pipeline-sasearch.py` 中添加一个命令行选项：

```python
parser.add_argument("--trace", action='store_true', help='Enable SA trace logging')

# ... 在 SA 运行部分 ...
if args.trace:
    trace_path = os.path.join(output_dir, 'sa_trace.yaml')
    sa.run_with_trace(trace_path)
else:
    sa.anneal()
```

## 实现细节

### 与 simanneal 库的集成

本实现**完全复用** simanneal 库的模拟退火算法，而不是重新实现：

1. **继承 Annealer 类**：`GraphPartitionSimulatedAnnealing` 继承自 `simanneal.Annealer`

2. **使用标准接口**：
   - `move()`: 生成新状态
   - `energy()`: 评估当前状态
   - `anneal()`: 执行标准 SA 流程

3. **钩子方法**：通过重写 `update()` 方法捕获每步信息
   ```python
   def update(self, step, T, E, acceptance, improvement):
       # 自定义日志记录
       if self._enable_trace:
           self._trace_steps.append({...})
       
       # 调用父类方法保持标准行为
       super().update(step, T, E, acceptance, improvement)
   ```

4. **trace 包装器**：`run_with_trace()` 方法：
   - 启用跟踪标志
   - 调用标准 `anneal()` 方法
   - 收集并输出跟踪数据

### 温度调度

使用 simanneal 的指数冷却调度：

```
T(step) = Tmax * exp(ln(Tmin/Tmax) * step / steps)
```

默认参数：
- `Tmax = 25000.0`
- `Tmin = 2.5`
- `steps = max_iterations`

### 接受概率

遵循标准 Metropolis 准则：
- 如果 ΔE < 0：接受概率 = 1.0
- 如果 ΔE ≥ 0：接受概率 = exp(-ΔE / T)

## 输出格式

### YAML 文件结构

```yaml
meta:
  created_at: "2025-12-02 10:30:00"
  Tmax: 25000.0
  Tmin: 2.5
  steps: 10000
  target:
    numArrays: 22
    spmKB: 44032
    # ... 其他硬件参数

model_params:
  layers:
    - layer_idx: 0
      type: conv
      inputs:
        - tensor_id: 0
          size: 1024
          size_aligned: 2048
      outputs: [...]
      weights: [...]

model_topology:
  nodes:
    - layer_idx: 0
      type: conv
  edges:
    - tensor_id: 1
      from_layer: 0
      to_layer: 1

initial_state:
  partition_plan: [...]
  acc_plan: [...]
  tensor_type: [...]
  tensor_meta_type: [...]

steps:
  - step: 1
    temperature: 24950.0
    energy: 123456.0
    acceptance_rate: 0.85
    improvement_rate: 0.45
    op:
      type_id: 0
      type: move_acc
      # ... 算子详情
    state: {...}

best:
  energy: 98765.0
  state: {...}
```

## 测试

运行测试脚本：

```bash
python test_sa_trace.py
```

这将：
1. 加载 ResNet18 模型
2. 运行 100 步 SA（用于快速测试）
3. 生成跟踪文件到 `output/pipeline/traces/resnet18/test_trace.yaml`

## 性能考虑

- **开销**：跟踪功能会增加约 10-20% 的运行时间（主要来自状态深拷贝和序列化）
- **内存**：跟踪数据在内存中累积，长时间运行（>100k 步）可能需要较大内存
- **可选性**：仅在调用 `run_with_trace()` 时启用，不影响标准 `anneal()` 方法

## 向后兼容性

- 现有代码完全兼容，无需修改
- 标准 `anneal()` 方法行为不变
- `run_with_trace()` 是可选的额外功能

## 故障排查

### 问题：YAML 文件过大
- **原因**：steps 太多或状态太复杂
- **解决**：减少 max_iterations 或实现采样（每 N 步记录一次）

### 问题：内存不足
- **原因**：trace_steps 列表过大
- **解决**：流式写入 YAML（修改为边运行边写入）

### 问题：枚举值未正确序列化
- **原因**：TensorType/TensorMetaType 是枚举类型
- **解决**：已通过 `_state_to_serializable()` 转换为字符串名称
