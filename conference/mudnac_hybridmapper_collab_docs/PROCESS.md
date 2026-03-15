# 过程记录（PROCESS）

- 版本：v1
- 日期：2026-03-05
- 目标：记录本次文档编写的事实依据、代码勘查路径与结论。

## T1：环境与目录核对

- 确认工程根目录：`/home/wzy/proj/wp2/chipyard`。
- 确认文档工作目录已创建：`tmp/mudnac_hybridmapper_collab_docs/`。
- 确认关键协同路径：`tmp/MudnacSim/models` 为符号链接，指向 `../HybridMapper/output/`。

## T2：MudnacSim 执行侧主链路勘查

- Model 与 layer mapping 装载：`tmp/MudnacSim/src/runtime/model_loader.cpp:30`。
- pipeline 入口解析与组装：`tmp/MudnacSim/src/runtime/runtime.cpp:2466`。
- runtime 调度初始化与 profiling 装载：`tmp/MudnacSim/src/runtime/runtime.cpp:753`、`tmp/MudnacSim/src/runtime/runtime.cpp:2072`。
- segment 资源申请/释放：`tmp/MudnacSim/src/runtime/runtime.cpp:815`、`tmp/MudnacSim/src/runtime/runtime.cpp:962`、`tmp/MudnacSim/src/runtime/runtime.cpp:1111`。
- pipeline 与 stage 状态机：`tmp/MudnacSim/src/runtime/pipeline.cpp:27`、`tmp/MudnacSim/src/runtime/pipeline_stage.cpp:195`。
- ring buffer 语义：`tmp/MudnacSim/src/runtime/pipeline_buffer.cpp:52`。
- 单模型与 QoS 执行入口：`tmp/MudnacSim/expr/expr_single_pipeline.cpp:1`、`tmp/MudnacSim/expr/expr_qos_pipeline.cpp:1`。

## T3：HybridMapper 编译侧主链路勘查

- 图划分核心：`tmp/HybridMapper/HybridMapper/GraphPartition.py:133`、`:175`、`:278`。
- SA 解空间与段评估：`tmp/HybridMapper/HybridMapper/SASearch.py:305`、`:383`、`:398`。
- 解记录导出工厂：`tmp/HybridMapper/HybridMapper/SASearch.py:1686`。
- stage 运行时类型映射：`tmp/HybridMapper/HybridMapper/StageRecord.py:198`。
- 层映射记录格式：`tmp/HybridMapper/HybridMapper/MTMRecord.py:7`。
- 脚本编排入口：`tmp/HybridMapper/scripts/create-pipeline-models.py:31`。
- 单模型 SA 输出：`tmp/HybridMapper/scripts/create-pipeline-sasearch-single-model.py:220`。
- 调度点 SA 输出：`tmp/HybridMapper/scripts/create-pipeline-schedule-sasearch.py:282`。
- profiling 聚合：`tmp/HybridMapper/scripts/create-schedule-point-profiling-file.py:1`。

## T4：关键契约核对

- 单模型映射命名：`entire_model/{acc}_{spm}_{batch}_{dram}_{noc}_{method}.yaml`。
- 调度点 profiling：`schedule_point_profiling_<method>.yaml`，字段包含 `target`、`cost`、`mapping_path`。
- runtime 解析字段包含：`mappingForPipeline`、`spmPageUtil`、`spmTensorPageCount`、`firstTensorPageNum`、`entry/exportTensorTypeList`、`ring_buffer_count` 等。

## T5：已识别风险（用于文档“现状风险”章节）

- CMake 引用了仓库中不存在的源文件：`tmp/MudnacSim/CMakeLists.txt` 包含 `qos_scheduler_new.cpp`、`thread_qos_pipeline_new.cpp` 等。
- `PipelineProfilingResult::Target::operator==` 未比较 `model_idx`，但哈希包含 `model_idx`，存在 key 等价与哈希一致性风险：`tmp/MudnacSim/src/runtime/runtime.cpp:2036`、`:2060`。
- `GenerateTarget` 中 `dram_bw` 目前写死为 `19`，可能导致与 profiling target 不一致：`tmp/MudnacSim/src/runtime/runtime.cpp:943`。
- `create-pipeline-schedule-point.py` 中 `Target(numArrays=512, ...)` 与候选 ACC 集可能不一致，存在语义偏移风险：`tmp/HybridMapper/scripts/create-pipeline-schedule-point.py:1`。

## 待你反馈的点（下一轮可改）

- 是否需要补“字段级 YAML schema（强约束/弱约束）”独立附录。
- 是否需要补“单 subgraph 执行时序图（文本版/mermaid）”。
- 是否需要在 v2 增加“建议修复项（不改代码，仅给 patch 草案）”。

## T6：v1 批注吸收与 v2 增补

- 批注结论：保留 v1 主体结构，不增加通用流程图，不增加“建议修复项”与“最小复现实验步骤”。
- v2 新增内容：
  - 实验脚本参数矩阵与推荐运行方式。
  - 严格版 YAML schema（按 parser 强约束整理）。
  - 单 subgraph 文本时序图。
  - 风险章节改为“问题-影响-建议”结构。
- v2 新文件：
  - `tmp/mudnac_hybridmapper_collab_docs/v2/协同机制文档_v2.md`
  - `tmp/mudnac_hybridmapper_collab_docs/v2/代码索引_v2.md`
  - `tmp/mudnac_hybridmapper_collab_docs/v2/反馈清单_v2.md`

## T7：v2 批注吸收与 v3 增量策略

- 批注结论：`v2` 结构整体认可，不回改 `v2` 主体，而是用 `v3` 增量补充文档承接新的细化要求。
- `v3` 重点新增：
  - 更详细解释各种 pipeline buffer 的作用、状态和页来源。
  - 更详细解释“开启松耦合 / 不开启松耦合”时的页面容量计算差异。
  - 把 `stages[i][0]` 这一隐式约定单独提到显眼位置。
  - 增加新的风险项，包括：隐式 stage 嵌套契约、方法名命名空间分裂、QoS 可执行入口默认未开启。
- `v3` 新文件：
  - `tmp/mudnac_hybridmapper_collab_docs/v3/协同机制文档_v3.md`
  - `tmp/mudnac_hybridmapper_collab_docs/v3/代码索引_v3.md`
  - `tmp/mudnac_hybridmapper_collab_docs/v3/反馈清单_v3.md`
