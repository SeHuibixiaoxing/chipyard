# Segment 0 Layer 0 Stall Notes

## Scope

This note isolates the current FireSim stall point for the bertmini pipeline-runtime run on:

- target: `rerocc_globalnoc_coupleddma_c2_g2_d2_spad1024kb_dram19_noc64_mac1024`
- workload: `rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime`
- mapping: `ours2`

The goal here is not to restate the whole project status, but only to record the work done around the current stall point.

## Current Symptom

The live guest reaches runtime init, completes:

- artifact cache validation
- model blob load
- input blob load
- segment 0 topology build
- stage-local page binding
- SPM xlate flush

Then it enters the first real compute stage and stops producing new runtime progress beyond:

```text
[prt-progress] worker stage=0 subbatch=0 begin op=1 acc=0 dma=2 tiles=2
[prt-progress] tiled-conv-safe-args batches=1 porows=93 ...
```

Meanwhile, FireSim `heartbeat.csv` continues to advance steadily, so this is not a Linux boot hang. It is a live simulation hang or long wait inside the first conv path.

## Exact Stage Being Run

Layer 0 in the exported model is:

- file: [model.layers.yaml](/home/ubuntu/chipyard/conference/HybridMapper/output/pipeline_runtime/bertmini/model.layers.yaml#L3)
- params: `param: [1, 256, 256, 256, 1, 1, 1, 1, 1, 1]`
- strides: `tensorStride: [1, 256, 256, 256]`
- type: `conv`

Interpreted by runtime, this is:

- `N=1`
- `IC=256`
- `OC=256`
- `OH=256`
- `OW=1`
- `KH=KW=1`
- `G=1`
- `stride=1`

So this is a canonical 1x1 pointwise conv, not a grouped conv.

Segment 0 mapping is:

- file: [pipeline_mapping...ours2.yaml](/home/ubuntu/chipyard/conference/HybridMapper/output/pipeline_runtime/bertmini/pipeline_mapping.rerocc_globalnoc_coupleddma_c2_g2_d2_spad1024kb_dram19_noc64_mac1024.ours2.yaml#L16)
- `acc_util: 2`
- `splitKind: oc`
- `vAccIdxList: [0, 1]`

So the first layer is explicitly mapped as a 2-Gemmini output-channel split.

## Important Clarification: What "pointwise" Means Here

In the runtime adapter, "pointwise" means:

- canonical 1x1 conv
- `kernel=1`, `stride=1`, `padding=0`
- no pooling
- no dilation
- no tensor transpose flags
- input spatial shape equals output spatial shape

See:

- [prt_gemmini_adapter.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_gemmini_adapter.c#L174)

This is treated as a conv which is mathematically equivalent to a matrix multiplication:

- `I = batch * out_row_dim * out_col_dim`
- `J = out_channels`
- `K = in_channels`

See:

- [prt_gemmini_adapter.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_gemmini_adapter.c#L193)

This is separate from grouped conv. Grouped conv is handled by a different path which splits over `groups` first:

- [prt_gemmini_adapter.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_gemmini_adapter.c#L2240)

For this stuck layer, `G=1`, so grouped-conv handling is not involved.

## What We Already Fixed Before Reaching This Stall

These are upstream software issues already resolved and not believed to be the present stall:

1. Explicit stride semantics were added end-to-end.
2. Conv/resadd layout validation and pad inference were added.
3. Silent descriptor fallback to `OP_NONE` was removed.
4. Grouped conv support was added.
5. `ALL_RINGBUFFER` host-serial empty-page fallback was fixed.

These fixes were validated on host and got the run past earlier failures into real on-device execution.

## Runtime Facts Confirmed For This Stall

### 1. This is not a Linux boot stall

Observed in live `uartlog`:

- `/etc/init.d/S99run`
- FireMarshal workload launch
- runtime init progress
- segment 0 begin

Observed in live `heartbeat.csv`:

- heartbeat kept growing even while UART stopped advancing

So the stall is inside the compute path, not boot.

### 2. This stage is using two Gemmini managers

Observed in UART:

```text
[prt-progress] action=1 stage=0 layer=0 acc_util=2 split=2 gm0=0 dm0=2 explicit=0
[prt-progress] worker stage=0 ready ... acc=0 dma=2 tiles=2
```

`split=2` corresponds to `PRT_LAYER_SPLIT_OC`.

### 3. This stage is not a pure-DRAM conv

Observed in UART:

```text
[prt-progress] topology-local-pages stage=0 kind=entry tensor=0 slots=1 pages_per_slot=64 preferred_cnt=2 pref0=0 pref1=1
[prt-progress] topology-local-pages stage=0 kind=export tensor=2 slots=1 pages_per_slot=64 preferred_cnt=2 pref0=0 pref1=1
```

So entry/output tensors are staged onto local shared SPM pages before compute. This matches the intended pipeline model: DMA first, then Gemmini compute directly from shared scratchpad aliases.

### 4. SPM translation is enabled in this run

Observed in UART:

```text
[prt-progress] init begin backend=0 cores=2 gemmini=2 dma=2 pages_per_acc=1024 page_bytes=1024 spm_xlate=1 range_base=0x0 range_size=16777216
[prt-progress] init page-table end ptbr=...
[prt-progress] init spm-xlate begin
[prt-progress] init spm-xlate end
```

The init log prints the pre-page-table config. Runtime later allocates the alias region during page-table init on Linux.

## Important Correction About Earlier "Missing Raw Logs"

Earlier interpretation:

- raw diagnostic markers were added
- but none appeared in live UART
- so maybe the runtime never reached those code points

That interpretation was incomplete.

What was later proven:

- the FireMarshal tmux wrapper had not forwarded custom environment variables like
  `PIPELINE_RUNTIME_PROGRESS_RAW=1` into the tmux session
- as a result, the workload image used for those reruns was rebuilt with:
  - `PIPELINE_RUNTIME_PROGRESS_RAW=0`
  - `-DPRT_ENABLE_PROGRESS_RAW_LOG=0`

Direct evidence:

- `r3` FireMarshal build log explicitly showed `PIPELINE_RUNTIME_PROGRESS_RAW=0`
- `strings` on the built `rerocc_pipeline_runtime-linux` showed no `[prt-raw]` markers

After fixing `scripts/firemarshal-tmux-run.sh` to explicitly export the pipeline-runtime env vars into the generated tmux command script:

- FireMarshal `r5` build log shows:
  - `PIPELINE_RUNTIME_PROGRESS_RAW=1`
  - `-DPRT_ENABLE_PROGRESS_RAW_LOG=1`
- rebuilt binary now contains:
  - `pw-chunked-*`
  - `matmul-outer-configured`
  - `matmul-os-*`

So:

- previous runs are still valid evidence for the coarse stall boundary
- but they are **not** valid evidence for the absence of inner raw markers
- the next rerun with the rebuilt image is the first decisive run for micro-localization

## Attempts Made Around This Stall

### Attempt A: Fix stride semantics and stop guessing stride

Reason:

- Earlier code inferred or assumed stride from channel counts in several places.
- This could make split-OC sub-convs issue wrong address arithmetic.

Change:

- Exported `tensorStride` from HybridMapper and consumed it in runtime.
- Conv/resadd descriptors now carry explicit `in_stride`, `weight_stride`, `out_stride`.

Result:

- Host closure passed.
- This was necessary, but it did not remove the on-device stall at segment 0 layer 0.

Conclusion:

- The current stall is after the stride cleanup, so the bug is deeper than "stride not exported".

### Attempt B: Keep conv/resadd on the pipeline-buffer exec-address contract

Reason:

- User explicitly required no `host_addr` special casing.
- Gemmini should decide whether the address targets DRAM or shared SPM via runtime views and page tables.

Change:

- Descriptor builders now use `stage_tensor_exec_addr(...)`.

Result:

- Runtime now really exercises the intended shared-SPM alias model.
- Segment 0 confirms this by binding entry/export pages and then stalling in the compute path.

Conclusion:

- This was the correct architectural direction.
- It also exposed that the first real compute path is not robust under this alias model.

### Attempt C: Fix grouped-conv support and ringbuffer page staging

Reason:

- The run originally failed before reaching this stall.

Result:

- Run now enters real compute.

Conclusion:

- Not related to the current segment-0 stall, but important context: the current problem is later and narrower.

### Attempt D: Inspect whether the first layer should take the pointwise matmul fallback

Reason:

- Layer 0 is a canonical 1x1 conv with `G=1`.
- In adapter semantics, that should qualify for the pointwise matmul fallback path.

Relevant code:

- canonical pointwise check:
  - [prt_gemmini_adapter.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_gemmini_adapter.c#L174)
- OC-split pointwise-direct-strided path:
  - [prt_gemmini_adapter.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_gemmini_adapter.c#L1687)

Observation:

- The live UART does not show pointwise fallback progress.
- Instead it shows `tiled-conv-safe-args`, which is emitted only by the loop-conv path.

Conclusion:

- In the current binary, layer 0 is not actually taking the pointwise fallback at runtime.
- It is going into the `tiled_conv` / `gemmini_loop_conv_ws` path.

### Attempt E: Check whether this is actually a grouped-conv case

Result:

- No.
- Layer 0 is `G=1`, so this stall is unrelated to the new grouped-conv support.

Conclusion:

- The stuck path is canonical 1x1 OC-split conv, not grouped conv.

### Attempt F: Compare with baremetal resadd regressions

Reason:

- User asked whether shared-SPM interleaved-page handling was fundamentally broken.

Result:

- Explicit interleaved-page resadd baremetal regressions showed:
  - the handwritten explicit workaround was the buggy path
  - canonical Gemmini resadd path can work on interleaved shared-SPM aliases

Conclusion:

- Shared-SPM aliasing itself is not proven broken globally.
- The present evidence points more specifically at the conv path, especially `loop_conv_ws`-style execution on alias-backed tensors.

## Current Best Hypothesis

The most likely current root cause is:

1. Segment 0 layer 0 is a canonical 1x1 conv mapped as 2-way OC split.
2. Its tensors are resident on shared-SPM alias pages, not plain DRAM.
3. The runtime ends up in the `tiled_conv` path instead of the pointwise matmul fallback.
4. That means execution likely reaches `gemmini_loop_conv_ws` on alias-backed tensors.
5. This is the strongest current suspect for the hang.

Why this is plausible:

- The stall boundary is exactly at the loop-conv path marker.
- Resadd on alias-backed tensors has already been shown to work.
- Earlier comments in the adapter already note loop-based pointwise/strided paths can stall on FPGA.

## Important Architectural Mismatch Still Present

Even aside from the immediate stall, the current OC-split implementation is not yet truly concurrent across the two Gemmini managers.

In the pointwise-direct-strided path:

- it loops over tiles
- issues one manager
- waits
- then issues the next manager

See:

- [prt_gemmini_adapter.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_gemmini_adapter.c#L1703)

So it uses two managers, but serially, not simultaneously.

This does not explain the current hang by itself, but it is a known semantic mismatch with the intended multi-manager execution model.

## Next Software Tries To Make

### Try 1

Re-enable the canonical pointwise matmul fallback for shared-SPM alias-backed tensors, instead of force-falling back to `tiled_conv`.

Why:

- The current alias-specific disable pushes the first layer back into the loop-conv path.
- For canonical 1x1 conv, OS matmul fallback is the cleaner candidate path.

### Try 2

Add non-HOT progress logs around pointwise fallback and OC-split tile launch/end.

Why:

- Current UART only exposes normal progress logs.
- Without ordinary logs, it is too hard to distinguish:
  - pointwise fallback
  - loop-conv path
  - per-manager tile boundaries

### Try 3

After the above patch, rebuild/install workload, rerun FireSim, and check whether segment 0 moves past layer 0.

Success criterion:

- Any log beyond the current `tiled-conv-safe-args` boundary
- or ideally progress into `segment=1`

### Try 4

If the pointwise fallback still stalls, inspect the Gemmini matmul path under alias-backed addresses separately from conv loop control.

That would narrow the problem to:

- alias translation for generic load/store/execute
- or manager-specific execution/fencing semantics

## Bottom Line

For this stall point, the strongest current reading is:

- this is not grouped conv
- this is not Linux boot
- this is not the earlier stride-export bug
- this is not the earlier grouped-conv/ringbuffer bug
- this is the first canonical 1x1 OC-split conv
- it is running on shared-SPM alias-backed tensors
- and it appears to be entering the loop-conv path where it likely hangs

That is the working target for the next patch and rerun.

## Update After The Pointwise Rerun Patch

The runtime adapter has now been patched again and the workload was rebuilt and reinstalled.

Key software change:

- the shared-SPM alias special-case which previously prevented canonical 1x1 conv from using the pointwise matmul fallback was removed
- the canonical pointwise fallback now switches this narrow path from `WS` to `OS`
- ordinary `PRT_PROGRESS_LOG` markers were added around:
  - `pointwise-matmul-fallback begin/end`
  - `oc-split-pointwise begin/end`

Relevant file:

- [prt_gemmini_adapter.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_gemmini_adapter.c)

Current FireSim status for this rerun:

- `launchrunfarm`: succeeded
- instance: `i-0e3564c17a0b152a7`
- `infrasetup`: succeeded
- `runworkload`: currently running

So the current live check is no longer testing the old binary which definitely fell back into `loop_conv_ws`.
It is testing the new binary which should expose whether alias-backed canonical 1x1 OC-split conv can survive on the pointwise matmul path.

## Additional Code Facts Confirmed Since This Note Was Started

### 1. Runtime is now consuming exported tensor stride metadata

This is no longer just a guessed channel-based stride in the conv descriptor builder.

See:

- [Model.py](/home/ubuntu/chipyard/conference/HybridMapper/HybridMapper/Model.py#L161)
- [prt_runtime.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_runtime.c#L892)
- [prt_runtime.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_runtime.c#L2957)

For conv, HybridMapper exports:

- bias stride: `1`
- weight stride: `OC`
- input stride: `G * IC`
- output stride: `G * OC`

So for this layer, stride metadata is present and matches the layer layout.

### 2. Padding is still inferred, not explicitly exported

The runtime currently infers input height/width and pad from tensor size plus:

- `OH`
- `OW`
- `KH`
- `KW`
- `sH`
- `sW`
- `in_stride`

See:

- [prt_runtime.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_runtime.c#L949)
- [prt_runtime.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_runtime.c#L982)

For this specific layer, the inferred result is still the expected canonical case:

- `padding=0`
- `IH=256`
- `IW=1`

So pad inference is still a general semantic risk for future layers, but it is not the strongest suspect for this exact layer-0 stall.

### 3. DMA-side shared-SPM VA translation already handles 1KB page crossings explicitly

The runtime-side DMA helper does not assume a single contiguous physical region for shared-SPM alias VAs.
It explicitly:

- translates the source VA range into physical segments
- translates the destination VA range into physical segments
- issues DMA in chunks bounded by each translated segment

See:

- [prt_page_table.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_page_table.c#L873)
- [prt_dma.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_dma.c#L759)

So the software DMA path already has explicit cross-1KB-page splitting logic.
That makes a pure "runtime DMA forgot interleaved-page boundaries" explanation less likely for the current conv hang.

### 4. Existing baremetal interleaved regressions already cover standard Gemmini resadd on alias-backed pages

There is already a dedicated baremetal test:

- [rerocc_lc_resadd_explicit_interleaved.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/bareMetalC/learn-gemmini/rerocc_lc_resadd_explicit_interleaved.c)

This test covers:

- contiguous alias pages
- interleaved alias pages
- explicit handwritten `mvin/mvin2/mvout`
- standard `tiled_resadd_stride_auto(..., WS)`

So for the current investigation, this file is important not because it proves all shared-SPM software is correct, but because it narrows the remaining suspect area:

- shared-SPM alias translation is not obviously incompatible with standard Gemmini resadd
- the current bertmini stall is therefore more specific than "all alias-backed Gemmini traffic is broken"

### 5. Standard Gemmini high-level matmul/conv APIs still fundamentally treat external tensor pointers as memory-side addresses

In Gemmini ISA terms:

- `mvin` / `mvin2` / `mvout` take memory-side addresses in `rs1`
- execute/preload use local scratchpad or accumulator row addresses

See:

- [README.md](/home/ubuntu/chipyard/generators/gemmini/README.md#L418)
- [README.md](/home/ubuntu/chipyard/generators/gemmini/README.md#L447)
- [README.md](/home/ubuntu/chipyard/generators/gemmini/README.md#L396)
- [gemmini.h](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/include/gemmini.h#L212)

That means the alias mechanism only matters on the memory-facing `load/store` side.
The compute engine itself still runs on local scratchpad or accumulator row addresses after the data-movement phase is issued.

This is consistent with the intended model:

- pipeline DMA stages materialize tensors into shared-SPM alias space
- Gemmini load/store commands can then use alias VAs as their memory-side addresses
- Gemmini execute uses local row addresses internally

So the remaining semantic question is not whether execute directly consumes virtual alias addresses.
It is whether the load/store path used by the chosen Gemmini library path is the one that correctly honors the alias translation scheme.

## Latest Live Outcome On 2026-03-23

The latest rerun no longer stalls at the old `tiled-conv-safe-args ...` boundary.

Live UART reached:

```text
[prt-progress] worker stage=0 subbatch=0 begin op=1 acc=0 dma=2 tiles=2
[prt-progress] oc-split-pointwise stage=0 tile=0/2 mgr=0 oc_beg=0 oc_tile=128 in_stride=256 weig
```

Important facts from that run:

- `uartlog` stopped growing at `2026-03-23 13:38:01 UTC`
- `heartbeat.csv` kept growing past `2026-03-23 13:46:01 UTC`
- the run farm was then explicitly terminated and the instance entered `shutting-down`

So the stall boundary really moved forward:

- old boundary: first `tiled_conv` / loop-conv path marker
- new boundary: first OC-split pointwise tile launch for manager 0

## Important Correction To An Earlier Reading

The absence of visible `pointwise-matmul-fallback ...` lines in that rerun is **not** proof that the code failed to enter the pointwise fallback.

Reason:

- `PRT_GEMMINI_SAFE_POINTWISE_OC_CHUNK` is `64`
- the first split tile is `oc_tile=128`
- so the code can immediately redirect from:
  - `prt_run_pointwise_matmul_fallback_strided_impl(...)`
  - into `prt_run_pointwise_matmul_fallback_chunked_oc(...)`
- before emitting the non-chunked fallback `begin/end` logs
- and the chunked path previously emitted only `PRT_PROGRESS_HOT_LOG`, which is disabled in the current workload build

That means the correct current interpretation is narrower:

- the rerun definitely reaches the OC-split pointwise path
- it may already be entering the chunked pointwise matmul fallback
- but the previous logging was insufficient to prove exactly whether it hangs:
  - before `rr_acquire_scope`
  - after acquire / flush
  - or inside the chunked matmul helper

## Current Follow-Up Patch

The current code now adds ordinary `PRT_PROGRESS_LOG` markers around:

- `conv-sync-strided begin/acquired/flushed/dispatch/fence`
- `pointwise-matmul-fallback ... chunked`
- `pointwise-matmul-fallback-chunked begin/chunk-begin/chunk-end/end`

Relevant file:

- [prt_gemmini_adapter.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/src/prt_gemmini_adapter.c)

This next rerun should answer whether the remaining blocker is:

- manager acquire / flush
- entry into chunked pointwise fallback
- a specific chunk inside the pointwise matmul helper

## New Boundary After The Latest Rerun

The latest live rerun advanced past all of the above pre-dispatch uncertainty.

Live UART reached:

```text
[prt-progress] worker stage=0 subbatch=0 begin op=1 acc=0 dma=2 tiles=2
[prt-progress] oc-split-pointwise stage=0 tile=0/2 mgr=0 oc_beg=0 oc_tile=128 in_stride=256 weight_stride=256 out_stride=256 begin
[prt-progress] conv-sync-strided stage=0 mgr=0 begin oc=128 out_dim=256x1 in_dim=256x1 in_stride=256 weight_stride=256 out_stride=256
[prt-progress] conv-sync-strided stage=0 mgr=0 acquired
[prt-progress] conv-sync-strided stage=0 mgr=0 flushed use_pointwise=1
[prt-progress] conv-sync-strided stage=0 mgr=0 dispatch=pointwise
[prt-progress] pointwise-matmul-fallback stage=0 mgr=0 chunked I=256 J=128 K=256 oc_chunk=64 in_stride=256 weight_stride=256 out_stride=256 requested_type=WS fallback_type=OS
[prt-progress] pointwise-matmul-fallback-chunked stage=0 mgr=0 begin I=256 J=128 K=256 oc_chunk=64 in_stride=256 weight_
```

Important facts:

- `uartlog` stopped at `830` newline-terminated lines, with the final line left unterminated
- `heartbeat.csv` continued increasing after the UART stopped
- therefore the current live stall is no longer at:
  - Linux boot
  - artifact validation
  - `rr_acquire_scope`
  - `gemmini_flush(0)`
  - pointwise dispatch selection

The narrowest current reading is:

- manager 0 definitely enters the canonical pointwise fallback
- it definitely redirects into the chunked `OS` fallback
- the live hang is now at or immediately after the chunked fallback `begin` boundary

## Additional Structural Finding

While inspecting the current adapter code, one more software issue became explicit:

- the canonical `oc-split-pointwise` direct-strided path is still sequential
- it loops over tiles and calls `conv_call_for_manager_sync_strided(...)` one tile at a time
- so it is not yet the required “split a single layer across 2 Gemmini managers and execute them concurrently” implementation

This does not by itself explain the present manager-0 live stall, but it is a real semantic mismatch which must be fixed after the primitive stall is localized.

## Current Next Patch

To disambiguate “stuck in `fprintf`” from “stuck in first OS matmul primitive”, the code now adds raw fixed-string markers in:

- `prt_run_pointwise_matmul_fallback_chunked_oc(...)`
- Gemmini `tiled_matmul_outer(...)`
- Gemmini `sp_tiled_matmul_os(...)`

The next rerun should show whether the hang is:

- before the first chunk actually launches
- inside OS matmul configuration / first `mvin`
- or later in compute / `mvout`
