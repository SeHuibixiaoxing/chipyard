# MudnacSim × HybridMapper 协同文档计划（冻结版）

- 版本：v1
- 日期：2026-03-05
- 目标：交付一版“HybridMapper（编译侧）+ MudnacSim（执行侧）+ 脚本链路”的协同机制文档，覆盖模块组成、划分机制、pipeline 机制、buffer 机制、SPM 管理、多模型调度、协同契约、关键代码定位与现状风险。

## 交付目录

- `tmp/mudnac_hybridmapper_collab_docs/PLAN.md`
- `tmp/mudnac_hybridmapper_collab_docs/PROCESS.md`
- `tmp/mudnac_hybridmapper_collab_docs/v1/协同机制文档_v1.md`
- `tmp/mudnac_hybridmapper_collab_docs/v1/代码索引_v1.md`
- `tmp/mudnac_hybridmapper_collab_docs/v1/反馈清单_v1.md`

## 范围与默认

- 范围采用：`HybridMapper + MudnacSim + 脚本链路`。
- 文档深度采用：`架构 + 关键函数`（到文件/函数/关键字段级）。
- 首版以文字流程和表格表达为主，不引入额外图文件。

## v1 文档结构

1. 总体协同架构
2. 模块组成
3. 模型划分机制
4. 网络层划分机制
5. 单模型 pipeline 机制
6. pipeline buffer 机制
7. SPAD(SPM) 管理机制
8. 多模型调度机制
9. 编译测-执行测协同协议
10. 关键代码定位
11. 已知风险与实现现状

## 验收标准

- 覆盖性：8 个机制点均有独立章节。
- 可追溯性：每节至少 2-3 处关键代码锚点。
- 契约一致性：编译产物字段与执行 parser 字段逐项对齐。
- 路径有效性：文档中的路径在仓库中可定位。
- 现状说明：明确列出已识别风险（至少含 CMake 引用缺失、Target 匹配风险）。

## 非目标

- 不修改运行时/编译器代码逻辑。
- 不新增公共 API。
- 不做性能结论复现实验，仅做机制文档化与代码映射。
