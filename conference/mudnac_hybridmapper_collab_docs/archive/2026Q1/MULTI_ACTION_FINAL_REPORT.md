# Multi-Action Cleanup Report

日期：2026-03-26

## 本轮交付结果

本轮不是“完成 multi-action runtime”，而是完成了以下清理与集成：

1. 把文档改成真实状态
2. 把 current-action 真正接入主执行路径
3. 让所有 pipeline buffer 类型共享同一套 action 视图
4. 删除无效示例代码
5. 修复构建，使 `pipeline-runtime` 可编译

## 已落地的核心变化

### 1. worker 线程现在真正解析 action

`stage_worker_main()` 已经接入：

- `prt_stage_worker_get_action()`
- `prt_runtime_set_thread_action()`
- `prt_runtime_clear_thread_action()`

这意味着 worker 不再只是依赖全局 `rt->active_action`。

### 2. 执行期地址与页表 helper 已统一到 current-action

已经改到：

- `prt_runtime.c`
- `prt_page_table.c`
- `prt_gemmini_adapter.c`

因此 worker 执行过程中看到的：

- alias base
- SPM xlate context
- Gemmini xlate cache

都来自当前线程绑定的 action。

### 3. 所有 pipeline buffer 类型都走同一套 action 语义

本轮“扩展到所有 pipeline buffer 支持”的真实含义是：

- `C1..C8` 所有 buffer 类型在执行时都处于 thread-local current-action 之下
- 不再只有部分路径能读到正确 action

### 4. 无效示例已删除

已删除：

- `multi_action_example.c`

## 已验证结果

已验证本地构建：

```bash
make -C generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime -j4
```

构建通过。

## 仍然必须明确的限制

以下能力当前仍然没有：

1. action-private topology
2. 多 active action 同时运行
3. 多 action sink/progress/watchdog 聚合
4. 多 action 生命周期并发管理

所以当前不能再声称：

- “已经支持完整 multi-action concurrent execution”
- “一个 runtime 实例已经能并发推进多个 action”
- “已经完成面向多 CPU / 多 hart 的 action 并发调度”
