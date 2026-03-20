# Pipeline Runtime Integration Plan

日期：2026-03-17

## 终极目标

在不修改模型定义文件的前提下，复用 HybridMapper 现有能力，为 Gemmini `pipeline-runtime` 生成可直接执行的新接口文件，并逐步扩展到更多模型、更多硬件目标和后续的 multi-pipeline 能力。

## 当前阶段

### Phase 1

目标：

- 先在 step1 target 上完成单模型、单 pipeline、单层 stage 的闭环
- 当前模型固定优先 `bertmini`

当前 step1 target：

- `GemminiLearningConfigSpadReRoCCGlobalNoC2C1x2G2x1x2D2x1x2CoupledDMA`
- key: `rerocc_globalnoc_coupleddma_c2_g2_d2_spad1024kb_dram19_noc64_mac1024`

Phase 1 剩余工作：

1. 把新增执行阶段等待插桩打进 Linux binary 和 FireMarshal image
2. 重新做 FireSim/FPGA replay，确认实际阻塞 phase
3. 在 bertmini 上拿到真实 FPGA correctness 结论

Phase 1 完成标准：

- `bertmini`
- methods: `ours2 / gemini2 / tangram2`
- exporter 产物来自当前新接口
- `pipeline-runtime` 读取新接口完成 entire-model 执行
- FireSim/FPGA 输出与 CPU golden 在误差范围内对齐

### Phase 2

目标：

- 把当前闭环扩展到更多模型

当前重点：

- 除 `bertmini` 外，补更多支持模型的 fresh 导出与验证
- `resnet50` 当前先不在 step1 target 上强推，因为已确认 `SPM_EXCEED`

### Phase 3

目标：

- 在更大 dummy hardware target 上验证 entire model 支持

要求：

- dummy target 不生成真实计算/存储单元
- 重点检查导出、装载、runtime 执行路径是否完整

### Phase 4

目标：

- 支持多模型并发，对齐 Mudnac 的 multi-pipeline 方向

后续再进入：

- schedule-point 划分
- 面向 schedule-point 的 pipeline mapping
- dynamic QoS

## 当前优先级

1. `bertmini` FPGA correctness 闭环
2. 更多模型的正确性扩展
3. 更大 dummy target
4. multi-pipeline

## 固定流程

- 不手工改 `sims/firesim/deploy/workloads/*.json`
- 所有 FPGA workload 变体先写 FireMarshal 源配置
- 固定顺序：
  1. `marshal build`
  2. `marshal install`
  3. `launchrunfarm`
  4. `infrasetup`
  5. `runworkload`
- run 结束后执行 `terminaterunfarm`

## 当前约束

- 不改模型定义文件
- `stage == 1 layer`
- `pipeline-runtime` 只读新接口
- 允许多个 hardware target，但要输出不同文件名
- layer mapping 必须支持单层映射到多个 Gemmini 核
- 该多核映射结果由 HybridMapper 离线生成，runtime 只消费

## 当前暂缓项

- 多层 stage 语义
- `resnet50` 在 step1 target 上的继续搜索
- SA `max_iterations=100000` 的扩大搜索

说明：

- 当前 SA 先维持能支撑开发推进的配置
- 待当前闭环稳定后，再统一提高 SA 搜索强度
