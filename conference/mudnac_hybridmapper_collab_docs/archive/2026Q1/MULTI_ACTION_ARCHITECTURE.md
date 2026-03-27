# Multi-Action Architecture - Real Current State

日期：2026-03-26

## 当前已经落地的架构

当前 `pipeline-runtime` 已经完成的是“default-enabled current-action 执行语义”，不是“完整多 action 并发 runtime”。

现状如下：

1. 控制线程仍然按 segment 生成一个 `action`
   - `prt_runtime_run()` 中仍然是逐 segment：
     - `prt_action_generate()`
     - `prt_action_alloc_acc()`
     - `prt_action_alloc_spm()`
     - `prt_action_bind_topology()`
   - 这一阶段仍通过 `rt->active_action` 挂住当前 segment 的 action。

2. 执行线程已经切到 thread-local current-action
   - `stage_worker_main()` 在线程启动时调用 `prt_stage_worker_get_action()`
   - 该 helper 会按如下顺序解析 action：
     - 优先使用 `ctx->action`
     - 否则尝试 `hart -> action` 队列绑定
     - 最后退回 `rt->active_action`
   - 一旦解析成功，就调用 `prt_runtime_set_thread_action()` 把 action 写入 TLS。

3. 执行期 helper 已经统一改为读取 `prt_runtime_current_action()`
   - `prt_runtime.c`
     - stage manager 选择
     - stage exec view 准备
     - exec alias 地址计算
     - SPM xlate flush
     - host serial 路径的 stage runtime state
   - `prt_page_table.c`
     - 当前 xlate context 选择
     - alias base 选择
     - tensor 映射 / range translate
   - `prt_gemmini_adapter.c`
     - Gemmini SPM xlate cache 装载

4. 所有 pipeline buffer 类型现在都共享这一套 current-action 语义
   - 这里的“支持所有 buffer”是指：
     - `C1`
     - `C2`
     - `C3`
     - `C4`
     - `C5`
     - `C6`
     - `C7`
     - `C8`
   - 这些路径都在 `stage_worker_main()` 中运行，而 worker 已经在进入主循环前绑定 thread-local action。
   - 因此它们依赖的页表、alias window、Gemmini xlate cache，都会读到同一个线程私有 current-action。

5. 示例代码已经删除
   - `examples/multi_action_example.c` 已移除。
   - 原因是它不是可运行样例，而且会误导成“runtime 已完成多 action 集成”。

## 当前仍然不存在的架构能力

以下能力当前仍然没有完成：

1. action-private topology
   - `rt->pipebufs`
   - `rt->ringbufs`
   - `rt->isolate_pairs`
   - `rt->shared_pairs`
   - `rt->stage_spm_shadow`
   - `rt->stage_*` 运行时数组
   这些仍然都是 runtime 全局单实例。

2. 多 action 同时 active 的执行主线
   - 当前没有“一个 runtime 实例同时推进多个 action”的 run loop。
   - `action_queue` / `hart binding` 只是基础设施，不是完整调度器。

3. action-private 生命周期汇聚
   - 当前没有 per-action sink 追踪
   - 没有多 action watchdog / completion 聚合
   - 没有 action 级别的独立 topology build / teardown 并发管理

## 因此应如何准确表述

当前准确表述应为：

- 已完成：
  - default-enabled current-action 基础设施
  - 执行期页表 / alias / Gemmini xlate 的 action 语义统一
  - `C1..C8` 全部 buffer 路径接入同一套 current-action 视图
  - 原型代码清理到可编译状态
- 未完成：
  - action-private topology
  - 真正的多 action 并发执行
  - 多 CPU / 多 hart 并发管理多个 active action 的主线调度

## 下一阶段真正要做的事

下一阶段如果要做“真实 multi-action runtime”，重点不是再补几个 helper，而是重构以下对象为 per-action 私有：

1. topology/buffer 状态
2. stage runtime state
3. sink/progress/watchdog
4. 生命周期与 teardown
5. 多 action 调度入口
