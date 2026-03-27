# Multi-Action Integration Guide

日期：2026-03-26

## 这版代码应该怎样理解

当前不需要额外开关，current-action 语义已经默认启用。

也就是说：

- 控制线程仍然负责生成并安装当前 action
- worker 线程会在启动时解析该 action
- 后续执行期 helper 都会自动读取当前线程绑定的 action

## 当前适用范围

当前这版适用于：

- 单次 `runtime_run()` 中按 segment 顺序执行
- 每次只让一套 topology/buffer 状态处于活跃状态
- 但执行期地址、页表、Gemmini xlate 已不再硬编码依赖全局 `active_action`

## 当前不适用的场景

当前还不适用于：

- 一个 runtime 实例同时推进多个 action
- 多个 action 并发拥有各自独立 topology/buffer
- 多 CPU / 多 hart 同时管理多个 active action 并真正并发提交

## 基本验证

构建：

```bash
make -C generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime -j4
```

命令行入口：

```bash
./pipeline_runtime
```

当前会输出 usage，说明主二进制已正常链接。
