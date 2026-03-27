# Multi-Action Implementation Status

日期：2026-03-26

## 当前结论

这批 multi-action 清理工作已经从“原型脚手架”推进到“已接入主执行路径的 current-action 版本”，但还没有推进到“真正支持多个 active action 并发运行”。

## 已完成

### 1. 文档改成真实状态

`MULTI_ACTION_*` 文档已不再声称：

- “已经完成多 action 并发”
- “核心功能 100% 完成”
- “示例代码可直接运行”

### 2. 代码可编译

已验证：

```bash
make -C generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime -j4
```

当前本地构建通过。

### 3. current-action 默认启用

当前执行期默认使用：

- `prt_runtime_current_action()`
- `prt_runtime_set_thread_action()`
- `prt_runtime_clear_thread_action()`

worker 线程在启动时就会解析并绑定 action，不再只是保留原型 helper。

### 4. 所有 pipeline buffer 路径已经落到统一 action 视图

已覆盖：

- `C1`
- `C2`
- `C3`
- `C4`
- `C5`
- `C6`
- `C7`
- `C8`

说明：

- 这些 buffer 路径都在 `stage_worker_main()` 的同一执行循环中推进。
- worker 线程现在会先绑定 thread-local action。
- 后续页表、alias、SPM xlate 相关 helper 都通过 `prt_runtime_current_action()` 取上下文。

### 5. 示例代码已删除

已删除：

- `generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/examples/multi_action_example.c`

### 6. 原型残留构建问题已修复

本轮已修复的真实问题包括：

- `prt_action_queue.c` 中错误码不匹配
- `prt_action_spm_context.c` 调用不存在的 xlate/PTBR 接口
- `prt_stage_worker_multi_action.c` 在 `-Werror` 下缺少 `_GNU_SOURCE` 导致 `sched_getcpu()` 构建失败

## 当前仍未完成

### 1. action-private topology 未完成

以下对象仍是 runtime 全局单实例：

- `pipebufs`
- `ringbufs`
- `isolate_pairs`
- `shared_pairs`
- `stage_spm_shadow`
- `stage_*` runtime arrays

### 2. 多 action 主调度未完成

当前没有：

- 多 action run loop
- 多 action 同时 build topology
- 多 action sink/progress 汇聚
- 多 action 生命周期并发回收

### 3. action queue 仍只是基础设施

`action_queue` / `hart -> action` 绑定目前只是：

- action 解析辅助机制
- 为未来多 action 调度预留的接口

它还不是完整的执行调度器。

## 当前准确阶段

- `Phase 0: 约束分析` 已完成
- `Phase 1: current-action 基础设施` 已完成并接入执行主线
- `Phase 2: all-buffer current-action 统一` 已完成
- `Phase 3: action-private topology` 未开始
- `Phase 4: 真正多 action 并发调度` 未开始
- `Phase 5: 系统级验证` 未开始
