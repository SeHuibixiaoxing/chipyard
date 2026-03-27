# Multi-Action Performance Notes

日期：2026-03-26

## 当前没有性能结论

本轮工作是结构清理和执行语义修正，不是性能优化轮次。

因此当前不应声称：

- 有 multi-action 并发吞吐提升
- 有多 hart 并发调度增益
- 有 buffer 路径性能提升数据

## 当前实际影响

本轮引入的额外开销主要是：

1. worker 启动时解析一次 current-action
2. 执行期 helper 通过 `prt_runtime_current_action()` 取上下文

这部分属于轻量开销，理论上远小于 DMA/Gemmini 本体开销，但当前没有做系统级 benchmark。

## 当前真正的收益

当前收益主要是正确性和可维护性：

1. buffer 路径不再分裂出“部分路径读全局 action、部分路径读私有 action”的状态
2. 后续如果要做真正 multi-action，可以继续沿 current-action 语义扩展
3. 当前代码已经从“不可编译原型”收敛到“可编译、可继续集成的基线”
