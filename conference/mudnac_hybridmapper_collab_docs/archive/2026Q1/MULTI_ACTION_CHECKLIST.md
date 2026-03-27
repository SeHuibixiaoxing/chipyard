# Multi-Action Checklist

日期：2026-03-26

## 已完成

- [x] 文档改成真实状态
- [x] `pipeline-runtime` 本地构建通过
- [x] worker 线程接入 `prt_stage_worker_get_action()`
- [x] thread-local current-action 默认启用
- [x] `prt_runtime.c` 执行期 helper 改为读取 current-action
- [x] `prt_page_table.c` 改为读取 current-action
- [x] `prt_gemmini_adapter.c` 改为读取 current-action
- [x] `C1..C8` 全部 buffer 路径统一到同一套 current-action 视图
- [x] 删除无效示例代码

## 未完成

- [ ] action-private `pipebuf`
- [ ] action-private `ringbuf`
- [ ] action-private `shared/isolate pair`
- [ ] action-private `stage_spm_shadow`
- [ ] 一个 runtime 实例同时执行多个 active action
- [ ] 多 action sink/progress/watchdog 聚合
- [ ] 多 action 生命周期并发管理
