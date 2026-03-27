# Segment 0 Layer 0 Pointwise Attempt Log

日期：2026-03-23

## Attempt 22. Runtime-specific diff replay proves the new boundary is now earlier than the old bias-`mvin` edge: it stalls after `pointwise-inner ... matmul-args`

Observed from the 2026-03-25 replay:

- result dir:
  `/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-25--13-31-38-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime`
- manager log:
  `/home/ubuntu/chipyard/sims/firesim/deploy/logs/2026-03-25--13-31-38-runworkload-FKB6U0HSDTRXDY29.log`

This replay now visibly crosses:

- Linux boot
- `/etc/init.d/S99run`
- `[bertmini] hugetlb before/after`
- `[bertmini] method=ours2 ...`
- `runtime init-step=validate-artifacts end`
- `runtime init-step=load-model-bin end`
- `runtime init-step=load-input-blob end`
- `runtime init-step=ready`
- `segment=0 begin`
- full per-page `alloc-pages`
- full per-page `exec-bind`
- `pointwise-inner ... matmul-begin`
- `pointwise-inner ... matmul-args`

and still never reaches:

- `matmul-nn-stride-auto-enter`
- `matmul-auto-enter`
- `matmul-os-enter`
- `bias-mvin0-*`

Heartbeat evidence from the same replay:

- after the last visible UART line, `heartbeat.csv` continues to advance from about:
  - `9.89B`
  to about:
  - `10.78B`
  target cycles

Interpretation:

- this is not a Linux boot hang
- this is not an outer runtime-init hang
- this is not an `alloc-pages` / `exec-bind` visibility problem
- compared to Attempt 21, the active boundary has moved earlier:
  - old replay reached the first real bias `mvin`
  - this replay stops before the first `gemmini-phase` entry marker
- therefore the latest runtime-specific delta introduced a new earlier boundary somewhere between:
  - `pointwise-inner ... matmul-args`
  - and the first line inside `tiled_matmul_nn_stride_auto()`

Immediate next step:

- split `pointwise-inner ... matmul-args` into shorter marker lines
- add explicit:
  - `matmul-call-enter`
  - `matmul-call-return`
  around `tiled_matmul_nn_stride_auto()`
- keep the new per-page `alloc-pages / exec-bind` logs unchanged so the next replay still carries the full page-order / PTE evidence

## Attempt 21. Deepest Linux/F2 replay proves the boundary is no longer in logging or parameter packing; it is at the first real bias `k_MVIN` issue

Observed from the 2026-03-25 replay:

- result dir:
  `/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-25--10-46-01-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime`
- manual capture:
  - `/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-25--10-46-01-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime/manual-capture/uartlog.live`
  - `/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-25--10-46-01-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime/manual-capture/heartbeat.live.csv`

This replay now visibly crosses:

- `matmul-os-pre-bias-mvin0`
- `matmul-os bias-mvin0-a ...`
- `matmul-os bias-mvin0-b ...`
- `bm0c0`
- `bm0ce`
- `matmul-os bias-mvin0-call-rs1 ...`
- `matmul-os bias-mvin0-call-rs2 ...`
- `bm0ca`

and still never reaches:

- `bm0cb`
- `bm0c1`
- `matmul-os-after-bias`

Heartbeat evidence from the same replay:

- by the time these logs are captured, `heartbeat.csv` continues to advance past about:
  - `9.68B`
  target cycles

Interpretation:

- this replay rules out the earlier hypotheses that the active blind spot is still:
  - the outer pointwise wrapper
  - `sp_tiled_matmul_os()` setup
  - `spaddrs`
  - `bias-config`
  - `bias-mvin0` argument logging
  - `rs1/rs2` packing for the first bias move-in
- the code now definitely reaches:
  - the deepest helper just before the first real RoCC issue
- and the current missing edge is:
  - after `bm0ca`
  - before `bm0cb`
- therefore the active boundary is now best described as:
  - the first real bias `mvin` custom instruction issue itself
  - i.e. the `.insn r CUSTOM_0 ... k_MVIN` used for moving `D` into accumulator state

Immediate next step:

- stop spending full Linux/F2 boots on narrower logging inside this same path
- use the captured Linux parameters to reproduce on baremetal:
  - first bias `mvin(D -> acc)` in isolation
  - then with the minimum prior config/state needed
- use that faster loop to determine whether:
  - the `k_MVIN` issue itself hangs
  - the accumulator destination address is problematic
  - shared-spad xlate plus `D -> acc` is problematic
  - or some earlier hidden state is required before the hang appears

Derived baremetal reproduction tuple from this replay:

- outer pointwise subcall:
  - `I=256 J=64 K=256 fallback=OS`
- first `sp_tiled_matmul_os()` local tile:
  - `I=32 J=8 K=32`
  - `pad_I=0 pad_J=0 pad_K=0`
  - `stride_A=256 stride_B=256 stride_D=256 stride_C=256`
  - `act=1(RELU)`
  - `repeating_bias=1`
  - `D_stride=0`
- first real bias move-in:
  - `bias_row=0`
  - `dram=0x3fa9042000`
  - `sp=0x80000000`
  - `blocks=2`
  - `cols=16`
  - `rows=8`
  - `rs2=0x8001080000000`

## Attempt 20. Split-spaddrs replay crosses `spaddrs` and bias-config, then stalls inside the first `bias-mvin0` phase log itself

Observed from the 2026-03-25 replay:

- result dir:
  `/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-25--08-48-52-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime`
- manual capture:
  - `/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-25--08-48-52-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime/manual-capture/uartlog.live`
  - `/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-25--08-48-52-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime/manual-capture/heartbeat.live.csv`

This replay now visibly crosses:

- `matmul-os-pre-spaddrs-ab`
- `matmul-os spaddrs-ab ...`
- `matmul-os-post-spaddrs-ab`
- `matmul-os-pre-spaddrs-dc`
- `matmul-os spaddrs-dc ...`
- `matmul-os-post-spaddrs-dc`
- `matmul-os-pre-bias-config`
- `matmul-os bias-config ...`
- `matmul-os-post-bias-config`
- `matmul-os bias-loop ...`
- `matmul-os-pre-bias-mvin0`

The last visible line is now:

- `[gemmini-phase] matmul-os bias-mvin0 bias_row=0 dram=0x3f9c10e00`

and it still never reaches:

- `matmul-os-post-bias-mvin0`
- `matmul-os-after-bias`
- `matmul-os-after-b`
- `pointwise-inner ... matmul-end`

Heartbeat evidence from the same replay:

- by the time these logs are captured, `heartbeat.csv` continues to advance past about:
  - `9.27B`
  target cycles

Interpretation:

- this replay closes the previous `spaddrs` blind spot:
  - the code is no longer stopping at `spaddrs-ab`
  - and it is no longer stopping at `spaddrs-dc`
- it also proves the code crosses:
  - bias-config
  - bias-loop setup
  - the raw guard immediately before the first bias move-in detail log
- but the new boundary again appears inside a long `gemmini-phase` print itself:
  - specifically the first `bias-mvin0` parameter line
- therefore the current evidence still does **not** say the hang is already in:
  - `gemmini_extended_mvin(...)`
- it says the active blind spot is still earlier:
  - during emission of that detailed log line

Immediate next step:

- split `bias-mvin0` into smaller logs, just like the earlier `spaddrs` split
- add raw guards around the smaller pieces
- then distinguish:
  - stall in the first half of `bias-mvin0`
  - stall in the second half of `bias-mvin0`
  - or true progress into `post-bias-mvin0` / `after-bias`

## Attempt 19. High-detail Linux/F2 replay narrows the live boundary into `sp_tiled_matmul_os()` entry, right around the `spaddrs` print

Observed from the 2026-03-25 replay:

- result dir:
  `/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-25--08-04-25-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime`
- manual capture:
  - `/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-25--08-04-25-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime/manual-capture/uartlog.live`
  - `/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-25--08-04-25-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime/manual-capture/heartbeat.live.csv`

This replay now visibly crosses the full chain:

- `pointwise-inner ... matmul-begin`
- `[gemmini-phase] matmul-auto ...`
- `[graw] auto-call-tiled`
- `[graw] tiled-enter`
- `[gemmini-phase] matmul-outer begin ...`
- `[gemmini-phase] matmul-inner begin ...`
- `[gemmini-phase] matmul-inner ptrs ...`
- `[gemmini-phase] matmul-inner shape ...`
- `[gemmini-phase] matmul-inner flags ...`
- `[graw] tiled-pre-innercall`
- `[prt-raw] matmul-os-enter`
- `[gemmini-phase] matmul-os ptrs ...`
- `[gemmini-phase] matmul-os shape ...`
- `[gemmini-phase] matmul-os flags ...`

The last visible line is:

- `[gemmini-phase] matmul-os spaddrs A=0x0 B=0x7800 D=0x8000`

and it still never reaches:

- `[prt-raw] matmul-os-after-bias`
- `[prt-raw] matmul-os-after-b`
- `[prt-raw] matmul-os-after-a`
- `[prt-raw] matmul-os-after-compute`
- `pointwise-inner ... matmul-end`

Heartbeat evidence from the same replay:

- after the visible `matmul-os spaddrs` line, `heartbeat.csv` still advances to about:
  - `11.82B`
  target cycles

Interpretation:

- the active blind spot is no longer at:
  - Linux boot
  - artifact load
  - action bind/xlate install
  - outer pointwise wrapper
  - `tiled_matmul_auto(...)`
  - `tiled_matmul_outer(...)`
  - `matmul-inner begin`
- it is now specifically inside the very early entry path of:
  - `sp_tiled_matmul_os()`
- more concretely, the current evidence places the boundary:
  - at or immediately around the `spaddrs` parameter logging
  - and still before bias move-in finishes

Immediate next step:

- split the old one-line `spaddrs` print into two smaller phase logs:
  - `spaddrs-ab`
  - `spaddrs-dc`
- add raw guards before and after each one:
  - `matmul-os-pre-spaddrs-ab`
  - `matmul-os-post-spaddrs-ab`
  - `matmul-os-pre-spaddrs-dc`
  - `matmul-os-post-spaddrs-dc`
- rerun Linux/F2 and classify:
  - no `post-spaddrs-ab`: stall is in the AB print itself
  - `post-spaddrs-ab` but no `post-spaddrs-dc`: stall is in the DC print
  - `post-spaddrs-dc` but no `after-bias`: move focus to D bias move-in
  - `after-bias` appears: move focus to later B/A move-in or compute

## Attempt 18. Another replay proves the same `matmul-begin` boundary, so move phase markers off `stdout` and add a `tiled_matmul_nn_stride_auto()` entry marker

Observed from the 2026-03-25 replay:

- result dir:
  `/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-25--05-53-06-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime`
- manual capture:
  - `/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-25--05-53-06-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime/manual-capture/uartlog.live`
  - `/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-25--05-53-06-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime/manual-capture/heartbeat.live.csv`

This replay again reaches:

- `runtime init-step=load-model-bin ...`
- `segment=0 begin`
- `action=1 segment=0 bind-plan ...`
- `action=1 segment=0 xlate-install ...`
- `action=1 segment=0 bind-topology ...`
- `pointwise-chunk ... subcall-begin`
- `pointwise-inner ... matmul-begin I=256 J=64 K=256 fallback=OS`

and it still never reaches:

- `pointwise-inner ... matmul-end`
- any `[gemmini-phase] ...`
- `subcall-return`
- `chunk-drain`
- `chunk-end`
- `gemm-exit`

Heartbeat evidence from the same replay:

- after `matmul-begin`, `heartbeat.csv` continues to advance from about:
  - `11.12B`
  - to `11.50B`
  target cycles
- with no later completion marker and still no first Gemmini phase marker

Interpretation:

- the runtime has already crossed:
  - model/pipeline artifact init
  - segment resource binding
  - per-manager shared-spad xlate installation
- so the blind spot is still the same narrow region:
  - at or before entry to `tiled_matmul_nn_stride_auto(...)`
  - and still before the first visible Gemmini phase marker
- however, the current Gemmini phase logs were going through `stdout`,
  while the stable marker chain goes through `stderr`
- therefore the absence of `[gemmini-phase]` still mixes together two possibilities:
  - the code never entered the function
  - the first phase log itself is taking the problematic path

Immediate next step:

- move `PRT_GEMMINI_PHASE_LOG(...)` from `stdout` to `stderr`
- add:
  - `[gemmini-phase] matmul-nn-stride-auto-enter ...`
    at the top of `tiled_matmul_nn_stride_auto(...)`
- rebuild / reinstall / rerun
- then distinguish:
  - no `matmul-nn-stride-auto-enter`: stall before the wrapper call
  - `matmul-nn-stride-auto-enter` but no `matmul-auto-enter`: stall between wrapper entry and `tiled_matmul_auto(...)`
  - both markers present: stall deeper in Gemmini matmul setup

## Attempt 16. The live boundary has moved from outer chunk begin into inner `matmul-begin`

Observed from the 2026-03-25 replay:

- result dir:
  `/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-25--04-53-51-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime`
- manual capture:
  - `/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-25--04-53-51-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime/manual-capture/uartlog.live`
  - `/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-25--04-53-51-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime/manual-capture/heartbeat.live.csv`

This replay again reaches:

- Linux boot
- `/etc/init.d/S99run`
- `[bertmini] hugetlb before/after`
- `[bertmini] method=ours2 ...`
- `runtime-init`
- artifact validation
- `segment=0`

But the stable runtime boundary is now strictly deeper than before:

- `pointwise-chunk ... chunk-begin oc_beg=0 oc_tile=64`
- `pointwise-chunk ... subcall-begin oc_beg=0 oc_tile=64`
- `pointwise-inner ... matmul-begin I=256 J=64 K=256 fallback=OS`

and it still never reaches:

- `pointwise-inner ... matmul-end`
- `pointwise-chunk ... subcall-return`
- `pointwise-chunk ... chunk-drain`
- `pointwise-chunk ... chunk-end`
- `gemm-exit`

Heartbeat evidence from the same replay:

- after `matmul-begin`, `heartbeat.csv` continues to advance from about:
  - `8.70B`
  - to `9.49B`
  target cycles
- with no later inner/outer completion markers

Interpretation:

- the current active stall is no longer best described as:
  - outer chunk-loop entry
  - wrapper call boundary
- it is now much more credibly inside:
  - `tiled_matmul_nn_stride_auto(...)`
  - or a deeper Gemmini matmul sub-phase issued from that call

Immediate next step:

- stop adding outer runtime logs
- instead enable the existing fine-grained Gemmini phase logs for the next rerun:
  - `PIPELINE_RUNTIME_GEMMINI_PHASE=1`
- use that rerun to determine whether the stall is at:
  - `tiled_matmul_auto(...)`
  - `tiled_matmul_outer(...)`
  - `matmul-inner`
  - or a deeper `loop_ws` / command-issue phase

## Attempt 17. The replay reproduces the same `matmul-begin` stall, and still never reaches the first `[gemmini-phase]`

Observed from the 2026-03-25 replay:

- result dir:
  `/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-25--05-22-18-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime`
- manual capture:
  - `/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-25--05-22-18-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime/manual-capture/uartlog.live`
  - `/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-25--05-22-18-rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-f2-rerocc-linux-bertmini-pipeline-runtime/manual-capture/heartbeat.live.csv`

This replay again reaches:

- Linux boot
- `/etc/init.d/S99run`
- `[bertmini] hugetlb before/after`
- `[bertmini] method=ours2 ...`
- `main runtime-init-begin/end`
- `runtime begin`
- `segment=0 begin`
- `action=1 segment=0 alloc-acc`
- `action=1 segment=0 xlate-range-begin/end`
- `action=1 segment=0 alloc-spm`
- `segment=0 workers-launched=1`
- `pointwise-chunk ... subcall-begin`
- `pointwise-inner ... matmul-begin I=256 J=64 K=256 fallback=OS`

and it still never reaches:

- `pointwise-inner ... matmul-end`
- any `[gemmini-phase] ...`
- `subcall-return`
- `chunk-drain`
- `chunk-end`
- `gemm-exit`

Heartbeat evidence from the same replay:

- after `matmul-begin`, `heartbeat.csv` continues to advance from about:
  - `9.48B`
  - to `10.46B`
  target cycles
- with no later inner completion marker and still no first Gemmini phase marker

Interpretation:

- the current blind spot is narrower than "inside tiled matmul" in the generic sense
- it is now specifically at or before:
  - the entry of `tiled_matmul_nn_stride_auto(...)` / `tiled_matmul_auto(...)`
  - and before the first existing `[gemmini-phase] matmul-auto ...` log site
- so the next useful logging is not more outer runtime wrapper logging
- it is:
  - runtime init-step markers between `runtime begin` and `segment=0 begin`
  - and a new `[gemmini-phase] matmul-auto-enter ...` marker at the top of `tiled_matmul_auto(...)`

Immediate next step:

- rebuild the guest with:
  - runtime init-step markers
  - `matmul-auto-enter`
- rerun `infrasetup -> runworkload`
- then check whether the stall is:
  - before `matmul-auto-enter`
  - between `matmul-auto-enter` and the old `matmul-auto`
  - or deeper in the existing Gemmini phase chain

## Attempt 15. Marker-only chunk replay still stops after the first `chunk-begin`

Observed from the 2026-03-25 refined replay:

- the guest now reliably gets through:
  - Linux boot
  - `/etc/init.d/S99run`
  - `[bertmini] hugetlb before/after`
  - `runtime-init`
  - artifact validation
  - `segment=0`
- the last stable runtime boundary is:
  - `worker stage=0 subbatch=0 gemm-enter`
  - `conv-sync ... dispatch=pointwise`
  - `pointwise ... chunked`
  - `pointwise-chunk ... begin`
  - `pointwise-chunk ... chunk-begin oc_beg=0 oc_tile=64`

Important new evidence:

- before this replay, the ordinary `PRT_PROGRESS_LOG` calls inside
  `prt_run_pointwise_matmul_fallback_chunked_oc(...)` had been removed
  for:
  - chunked `begin`
  - `chunk-begin`
  - `chunk-drain`
  - `chunk-end`
  - chunked `end`
- despite that reduction, the run still never reached:
  - `chunk-drain`
  - `chunk-end`
  - `gemm-exit`
- `heartbeat.csv` continued to advance from roughly:
  - `9.09B`
  - to `11.83B`
  target cycles after `chunk-begin`

Interpretation:

- this replay removes the strongest remaining argument that the old
  chunked ordinary progress lines were themselves the active blocker
- the current live boundary is now more credibly inside or immediately around
  the first subchunk's real matmul call
- the next useful markers are no longer:
  - outer chunk `begin/end`
- they are:
  - the call into `prt_run_pointwise_matmul_fallback_strided_impl(...)`
  - and the first `tiled_matmul_nn_stride_auto(...)` begin/end inside that call

## Attempt 14. `r9` closes the large-`J=128` pointwise line: all six formerly failing cases now PASS

Observed from `r9`:

- all formerly failing large-`J=128` pointwise cases now PASS:
  - `pointwise_matmul_os_128_cross_1kb_contiguous`
  - `pointwise_matmul_os_128_cross_1kb_interleaved`
  - `pointwise_matmul_os_128_tjcap_cross_1kb_contiguous`
  - `pointwise_matmul_os_128_tjcap_cross_1kb_interleaved`
  - `pointwise_matmul_os_chunked_128_to_64_cross_1kb_contiguous`
  - `pointwise_matmul_os_chunked_128_to_64_cross_1kb_interleaved`
- the small diagnostic controls also stay PASS:
  - `pointwise_matmul_os_biasonly_smallik8_128_cross_1kb_contiguous`
  - `pointwise_matmul_os_biasonly_smallik8_128_cross_1kb_interleaved`

What this proves:

- the earlier large-`J=128` failure matrix was not a deeper Gemmini hardware limit
- it was not evidence that:
  - `tile_J > MAX_BLOCK_LEN` is inherently illegal
  - interleaved large-`J` is broken
  - the external chunk helper is the only workable path
- the fix that actually mattered was the chunk-bias VA-layout repair:
  - move chunk-bias vpages out of `C`'s VA span
  - validate overlaps only inside each installed xlate set

Refined conclusion:

- for this baremetal gate, the large-`J=128` root cause was a software page-table / VA-layout bug in the harness
- after removing that overwrite, the whole large-`J=128` matrix recovers without any RTL change

What remains after `r9`:

- the overall baremetal gate still ends in `ALL_TESTS_FAIL`
- but the remaining fails are now the older independent ones:
  - `copy_explicit_cross_1kb_interleaved_b_mvin2`
  - `resadd_explicit_cross_1kb_interleaved`
- so the active problem after this point is no longer pointwise `J=128`

## Attempt 12. `r7` proves the stronger bug is case-local virtual-page overlap, not a generic `J=128` hardware failure

Observed from `r7`:

- newly added small diagnostics both PASS:
  - `pointwise_matmul_os_biasonly_smallik8_128_cross_1kb_contiguous`
  - `pointwise_matmul_os_biasonly_smallik8_128_cross_1kb_interleaved`
- but all large `J=128` cases still FAIL with the same first-element signature:
  - `idx=0 exp=127 act=-128`

New code-level finding:

- the original virtual-page constants were:
  - `PW_C_VPAGE = 0x130`
  - `PW_CHUNK_BIAS_VPAGE = 0x140`
  - `PW_CHUNK_BIAS_CONTIG_VPAGE = 0x150`
- with:
  - `PW_BYTES_C = 65536`
  - page size `1KB`
  - page offset `768B`
- `C` therefore spans `65` virtual pages
- that means both old chunk-bias vpage bases fall inside the virtual-page span consumed by `C`

Why this matters:

- the case install order is:
  - `A -> B -> BIAS -> C`
- so when `C` is installed into the shared-spad xlate table, it overwrites the previously installed chunk-bias PTEs
- this directly explains the matrix:
  - `J=64` PASS because it uses `PW_BIAS_VPAGE = 0x120`, outside `C`
  - `J=128/chunked` FAIL because they use the chunk-bias vpages later overwritten by `C`
  - `smallik8` PASS because the diagnostic bias / `C` regions use separate VA ranges

Interpretation:

- this is a software harness / VA-layout bug
- it is not evidence that:
  - shared-spad cross-tile access is broken
  - Gemmini cannot use the page table correctly
  - the hardware rejects `J=128`

Fix applied:

- move chunk-bias vpages out of `C`'s VA range:
  - `PW_CHUNK_BIAS_VPAGE: 0x140 -> 0x171`
  - `PW_CHUNK_BIAS_CONTIG_VPAGE: 0x150 -> 0x174`
- keep the earlier physical-page avoidance fix:
  - `PW_CHUNK_CONTIG_BIAS_LOCAL_PAGE = 416`

## Attempt 13. The first validation patch was too broad; refine it to per-case xlate sets and replay `r9`

First validation attempt:

- add a global virtual-overlap check in `validate_alias_layout()`

Observed from `r8`:

- guest failed immediately with:

```text
ALL_TESTS_FAIL reason=region_vpage_overlap lhs=pw_contig_a rhs=pw_interleaved_a
```

Why that check was wrong:

- the baremetal file intentionally reuses the same VA ranges for:
  - contig fallback regions
  - interleaved fallback regions
- they are alternative mappings, not simultaneously installed entries
- so a global “all regions must have disjoint VA ranges” rule is stricter than the runtime contract

Refined fix:

- restore `validate_alias_layout()` to check only physical overlap
- add `alias_regions_virtual_page_overlap(...)`
- add `validate_case_xlate_layout(...)`
- call the new validator only on the exact region set installed for each test case

Current replay state:

- local baremetal compile passes with the refined fix
- `r9` packaging, launchrunfarm, and infrasetup all succeeded
- current live run:
  - instance `i-0e4b36e90a059599b`
  - private IP `192.168.1.63`
- live evidence so far:
  - `heartbeat.csv` has already advanced into the multi-billion-cycle range
  - `uartlog` has entered the baremetal binary and printed:
    - `[resadd-explicit-baremetal] gemmini_mgr=0 local_gemmini=0 num_gemmini=2`

Immediate next-use value:

- wait for `r9` to finish before making further semantic changes
- the first questions `r9` must answer are now:
  - is the old early `region_vpage_overlap lhs=pw_contig_a rhs=pw_interleaved_a` failure gone?
  - do the large `J=128` cases recover after the chunk-bias VA fix?

## Purpose

This note only records the concrete attempts around the current `segment=0 / layer=0` pointwise stall boundary.
It is intentionally narrower than `STATUS.md` and `SEG0_LAYER0_STALL.md`.

## Attempt 1. Remove the alias special-case and re-enable canonical pointwise fallback

Code change:

- remove the shared-SPM alias special-case that had prevented canonical `1x1` conv from taking the pointwise matmul fallback
- keep the pipeline-buffer exec-address contract
- do not reintroduce `host_addr` special-casing

Result:

- the live run moved beyond the old `tiled-conv-safe-args ...` stall
- the new live boundary became the first OC-split pointwise tile launch

Observed UART tail:

```text
[prt-progress] worker stage=0 subbatch=0 begin op=1 acc=0 dma=2 tiles=2
[prt-progress] oc-split-pointwise stage=0 tile=0/2 mgr=0 oc_beg=0 oc_tile=128 in_stride=256 weig
```

Interpretation:

- this patch was effective in moving the stall boundary forward
- the old “it never reaches pointwise OC-split” reading is no longer true

## Attempt 2. Switch canonical pointwise fallback from `WS` to `OS`

Code change:

- in the narrow canonical pointwise fallback, if requested type is `WS`, use `OS` instead

Reason:

- earlier live evidence showed the older fallback could enter `gemmini_loop_ws`
- the purpose was to avoid the suspected loop-WS path without degrading to CPU

Result:

- rerun still stalled
- but the new stall boundary is later than before, so this change did not regress progress

Current status:

- not yet enough evidence to claim `OS` matmul itself is the bug
- because the logging around the chunked fallback path was incomplete

## Attempt 3. Add ordinary logs around `oc-split-pointwise`

Code change:

- add ordinary `PRT_PROGRESS_LOG` at `oc-split-pointwise begin/end`

Result:

- begin marker became visible in live UART
- end marker never appeared

Interpretation:

- manager 0 tile launch is definitely reached
- hang is at or after the first tile enters the sync-strided pointwise path

## What Was Misread

Earlier reading:

- no visible `pointwise-matmul-fallback ...` lines
- therefore maybe the code never entered the fallback

Why that reading is wrong:

- `PRT_GEMMINI_SAFE_POINTWISE_OC_CHUNK = 64`
- current tile is `oc_tile=128`
- therefore the code can go directly into `pointwise-matmul-fallback-chunked`
- and that path previously emitted only `HOT_LOG`
- current workload build does not print hot logs

So:

- absence of `pointwise-matmul-fallback ...` in UART is not diagnostic
- it does not distinguish “never entered fallback” from “entered chunked fallback but only hot logs existed”

## Current Diagnostic Patch

Current pending rerun includes ordinary logs for:

- `conv-sync-strided begin`
- `conv-sync-strided acquired`
- `conv-sync-strided flushed`
- `conv-sync-strided dispatch=pointwise`
- `pointwise-matmul-fallback ... chunked`
- `pointwise-matmul-fallback-chunked begin`
- `pointwise-matmul-fallback-chunked chunk-begin/chunk-end`
- `conv-sync-strided fence-begin/fence-end`

Expected next-use value:

- if UART stops before `acquired`, suspect `rr_acquire_scope`
- if it stops before `dispatch=pointwise`, suspect `gemmini_flush(0)` or nearby pre-dispatch code
- if it reaches `chunk-begin` and then stops, suspect the chunked matmul helper itself

## Attempt 4. Refine the boundary inside chunked pointwise fallback

Observed from the latest rerun:

- artifact validation finished
- runtime entered `segment=0`
- logs advanced through:
  - `worker stage=0 ready`
  - `worker stage=0 subbatch=0 begin`
  - `oc-split-pointwise ... begin`
  - `conv-sync-strided ... begin`
  - `conv-sync-strided ... acquired`
  - `conv-sync-strided ... flushed use_pointwise=1`
  - `conv-sync-strided ... dispatch=pointwise`
  - `pointwise-matmul-fallback ... chunked`
- UART then stopped with an unterminated final line:

```text
[prt-progress] pointwise-matmul-fallback-chunked stage=0 mgr=0 begin I=256 J=128 K=256 oc_chunk=64 in_stride=256 weight_
```

Important runtime facts:

- `wc -l /home/ubuntu/sim_slot_0/uartlog` stayed at `830`
- `heartbeat.csv` kept increasing well past that point
- so Linux boot and artifact setup are no longer the active blocker

Interpretation:

- the stall boundary moved again, from `oc-split-pointwise begin` to the entrance of `pointwise-matmul-fallback-chunked`
- the hang is now at one of these narrower points:
  - inside the long `fprintf` for the chunked `begin` log
  - immediately after that log and before `chunk-begin`
  - inside the first inner OS matmul call

Additional code finding:

- the current canonical `oc-split-pointwise` fast path is not actually concurrent
- it iterates tiles and calls `conv_call_for_manager_sync_strided(...)` sequentially
- so even apart from the live stall, this path still violates the required 2-manager parallel execution contract

Follow-up patch now added:

- raw progress markers around `prt_run_pointwise_matmul_fallback_chunked_oc(...)`
- raw progress markers around Gemmini `OS` matmul internals:
  - `matmul-outer-configured`
  - `matmul-os-enter`
  - `matmul-os-after-bias`
  - `matmul-os-after-b`
  - `matmul-os-after-a`
  - `matmul-os-after-compute`
  - `matmul-os-after-mvout`
- coupled-DMA workload build path now propagates `PIPELINE_RUNTIME_PROGRESS_RAW`

Expected next-use value:

- if UART shows `pw-chunked-after-begin-log` but not `pw-chunked-before-inner`, the hang is still outside the inner matmul
- if it reaches `matmul-os-enter` and then stops before `matmul-os-after-b`, suspect the first Gemmini load/config side of OS matmul
- if it reaches `matmul-os-after-a` but not `matmul-os-after-compute`, suspect compute/preload issue inside the first OS tile

## Attempt 5. Fix raw-log propagation through FireMarshal tmux wrapper

Observed contradiction:

- code already had:
  - `PIPELINE_RUNTIME_PROGRESS_RAW` plumbed into
    `rerocc-linux-tests-coupleddma/workload/host-init.sh`
  - `PIPELINE_RUNTIME_PROGRESS_RAW` consumed by
    `rerocc-linux-tests/Makefile`
  - raw markers present in source:
    - `pw-chunked-*`
    - `matmul-outer-configured`
    - `matmul-os-*`
- but the live workload binary still contained no `[prt-raw]` strings

Direct evidence:

- FireMarshal build log for the stalled `r3` rerun showed:
  - `PIPELINE_RUNTIME_PROGRESS_RAW=0`
  - `-DPRT_ENABLE_PROGRESS_RAW_LOG=0`
- local `strings` on the built `rerocc_pipeline_runtime-linux` also showed no raw markers

Root cause:

- the problem was not the runtime build system
- it was the tmux launch wrapper:
  - `scripts/firemarshal-tmux-run.sh`
- calling:

```bash
env PIPELINE_RUNTIME_PROGRESS_RAW=1 ./scripts/firemarshal-tmux-run.sh ...
```

  did not make that variable visible inside the tmux session
- tmux only carried its own server environment, so custom one-shot variables were silently dropped

Fix:

- update `scripts/firemarshal-tmux-run.sh`
- explicitly bake selected pipeline-runtime environment variables into the generated tmux command script
- verified generated command script now contains:

```bash
export PIPELINE_RUNTIME_PROGRESS=1
export PIPELINE_RUNTIME_PROGRESS_RAW=1
```

Verification after fix:

- FireMarshal `r5` build log now shows:
  - `PIPELINE_RUNTIME_PROGRESS_RAW=1`
  - `-DPRT_ENABLE_PROGRESS_RAW_LOG=1`
- rebuilt `rerocc_pipeline_runtime-linux` now contains:
  - `[prt-raw] pw-chunked-enter`
  - `[prt-raw] pw-chunked-before-inner`
  - `[prt-raw] matmul-outer-configured`
  - `[prt-raw] matmul-os-enter`
  - `[prt-raw] matmul-os-after-b`
  - `[prt-raw] matmul-os-after-a`
  - `[prt-raw] matmul-os-after-compute`
  - `[prt-raw] matmul-os-after-mvout`

Updated interpretation:

- earlier reruns without raw markers are no longer useful for micro-localization inside the pointwise matmul path
- they only establish the coarse live stall boundary
- the next rerun with the rebuilt workload is the first one that can truly distinguish:
  - stall before first chunk dispatch
  - stall during OS matmul load/config
  - stall during OS matmul compute
  - stall during OS matmul mvout

## Attempt 6. `r5` raw-marker replay proves the stall is still before inner matmul

Observed from the `r5` live run:

- Linux boot completed normally
- workload reached `segment=0`
- runtime progressed through:
  - `worker stage=0 subbatch=0 begin`
  - `oc-split-pointwise ... begin`
  - `conv-sync-strided ... begin`
  - `conv-sync-strided ... acquired`
  - `conv-sync-strided ... flushed use_pointwise=1`
  - `conv-sync-strided ... dispatch=pointwise`
  - `pointwise-matmul-fallback ... chunked`
  - `[prt-raw] pw-chunked-enter`
  - `[prt-raw] pw-chunked-after-begin-log`
  - `[prt-raw] pw-chunked-before-chunk-log`

The last visible ordinary progress line is still truncated:

```text
[prt-progress] pointwise-matmul-fallback-chunked stage=0 mgr=0 chunk-begin oc_beg
```

Direct remote `grep` confirms the following markers are absent after long heartbeat growth:

- `[prt-raw] pw-chunked-before-inner`
- `[prt-raw] matmul-os-enter`
- `pointwise-matmul-fallback-chunked ... chunk-end`

Interpretation:

- this replay rules out the previous hypothesis that the stall is inside inner `OS` matmul
- the live boundary is now narrowed to the first chunk's ordinary `PRT_PROGRESS_LOG`
  or immediately before control reaches `pw-chunked-before-inner`
- since heartbeat continued to grow from roughly `950` to `1019` without any new marker,
  this is not just a short UART delay

Follow-up software action:

- remove the ordinary `chunk-begin/chunk-end` progress logs from
  `prt_run_pointwise_matmul_fallback_chunked_oc(...)`
- keep the existing raw markers and hot logs
- rerun to check whether execution can now reach:
  - `pw-chunked-before-inner`
  - `matmul-os-enter`
  - later inner `matmul-os-*` markers

## Attempt 7. `r6b` proves the new blocker is the logging path itself

Observed from the new rerun:

- FireMarshal rebuild/install completed with the patched workload image
- new FireSim run completed Linux boot and entered the runtime normally
- execution again reached:
  - `worker stage=0 subbatch=0 begin`
  - `oc-split-pointwise ... begin`
  - `conv-sync-strided ... dispatch=pointwise`
  - `pointwise-matmul-fallback ... chunked`
  - `[prt-raw] pw-chunked-enter`
  - `[prt-raw] pw-chunked-after-begin-log`
  - `[prt-raw] pw-chunked-before-chunk-log`

But the run still did not reach:

- `[prt-raw] pw-chunked-before-inner`
- any `matmul-os-*`
- any `chunk-end`

New decisive evidence:

- `tail -n 20 /home/ubuntu/sim_slot_0/uartlog` ended with a literal half-written line:

```text
[prt-raw]
```

- byte-level inspection of the last 96 bytes confirmed the file really ends with:

```text
... [prt-raw] pw-chunked-before-chunk-log\r\r\n[prt-raw]
```

- during that time:
  - `heartbeat.csv` kept increasing from roughly `1129` to `1246`
  - `uartlog` stayed fixed at:
    - `102081` bytes
    - `1478` lines

Interpretation:

- this run moved the live boundary forward again:
  - the previous ordinary `chunk-begin` progress line is no longer the active blocker
- the new active blocker is the next logging operation itself
- the most likely location is the next raw marker write, i.e. the attempted emission of:
  - `[prt-raw] pw-chunked-before-inner`
- therefore the current live stall is still not evidence of a Gemmini inner-matmul failure
- it is stronger evidence that the UART / stdio logging path is blocking or being partially drained in this hot path under FireSim Linux

Follow-up action:

- stop using blocking progress logging as the diagnostic mechanism in this hot path
- change progress logging to best-effort nonblocking writes
- rebuild / reinstall / rerun
- only after that judge whether execution truly reaches `pw-chunked-before-inner` and `matmul-os-*`

## Attempt 8. Move the diagnosis to a baremetal differential gate

Reason:

- the live Linux stop point had become too entangled with UART / stdio behavior
- we needed a faster discriminator that preserves:
  - rerocc manager mode
  - shared-spad alias / xlate
  - explicit `mvin/mvin2/mvout`
  - standard Gemmini pointwise matmul calls

Action:

- add and replay:
  - `rerocc_lc_resadd_explicit_interleaved.c`
- run it on FireSim F2 as the dedicated baremetal gate

Key results from `r2` / `r3`:

- PASS:
  - `pointwise_matmul_os_cross_1kb_contiguous`
  - `pointwise_matmul_ws_cross_1kb_contiguous`
  - `pointwise_matmul_os_cross_1kb_interleaved`
  - `pointwise_matmul_ws_cross_1kb_interleaved`
- FAIL:
  - `pointwise_matmul_os_128_cross_1kb_contiguous`
  - `pointwise_matmul_os_128_cross_1kb_interleaved`
  - `pointwise_matmul_os_chunked_128_to_64_cross_1kb_contiguous`
  - `pointwise_matmul_os_chunked_128_to_64_cross_1kb_interleaved`

Interpretation:

- the active pointwise failure is not “Linux logging only”
- it is also not “only interleaved aliasing”
- it is also not “only the chunk helper”
- the boundary is a more general `OS + J=128` shape problem, with chunk-to-chunk drain still a separate secondary issue

## Attempt 9. Re-read Gemmini tile limits and add an explicit `tileJ` control

Reason:

- current active parameters are:
  - `DIM=8`
  - `MAX_BYTES=64`
  - `MAX_BLOCK_LEN=8`
- with the current auto tiler:
  - `J=64` uses `tileJ=8`
  - `J=128` uses `tileJ=16`
- this exactly matches the observed pass/fail boundary

Action:

- add baremetal controls:
  - `pointwise_matmul_os_128_tjcap_cross_1kb_contiguous`
  - `pointwise_matmul_os_128_tjcap_cross_1kb_interleaved`
- these force explicit OS tiling with `tileJ = MAX_BLOCK_LEN`
- in parallel, add a runtime patch so chunked canonical pointwise fallback does a scope-aware drain between chunks

Current status:

- both code changes are already in the tree
- the updated baremetal target rebuilt locally
- the next required step is a fresh FireMarshal build/install and FireSim replay to see whether `tjcap` fixes `J=128`

Correction added after re-reading the hardware:

- `MAX_BLOCK_LEN` is generated from the Gemmini config as:
  - `MAX_BYTES / (DIM * elem_bytes)`
- on this target that is exactly:
  - `64 / (8 * 1) = 8`
- this is a per-DMA-transaction block-width bound, not automatically a proof that
  `tile_J > MAX_BLOCK_LEN` is hardware-illegal
- the loop load/store engines already clamp each individual transfer to
  `min(req.max_j, max_block_len)` and iterate when `req.max_j` is larger
- therefore the refined interpretation is:
- `tjcap` is testing whether forcing `J` back into a single-transaction-friendly range
  avoids the bug
- it is not proving that the hardware specification forbids `tile_J > MAX_BLOCK_LEN`

## Attempt 10. `r4` shows the current baremetal `J=128` gate is polluted by alias-page overlap

Observed from `r4`:

- both new controls still fail:
  - `pointwise_matmul_os_128_tjcap_cross_1kb_contiguous`
  - `pointwise_matmul_os_128_tjcap_cross_1kb_interleaved`
- the mismatch signature is unchanged:
  - `idx=0 exp=127 act=-128`

New code-level finding:

- for the `J=128` controls, the chunk-bias buffer is `512B`
- with `1KB` pages and `768B` page offset, that alias region spans `2` pages
- but the current local-page assignments are:
  - contiguous:
    - `C` starts at page `336`
    - `chunk-bias` starts at page `352`
  - interleaved:
    - `C` starts at page `656`
    - `chunk-bias` starts at page `704`
- `C` itself is `65536B`, so it occupies `65` pages
- a first coarse range calculation suggested:
  - contiguous:
    - `C = [336, 400]`
    - `chunk-bias = [352, 353]`
  - interleaved:
    - `C = [656, 720]`
    - `chunk-bias = [704, 705]`
- but after recalculating with the real `alias_region_build_interleaved()`
  mapping rule:
  - contiguous `chunk-bias` / `C` physically overlap
  - interleaved `chunk-bias` / `C` do not physically overlap

Interpretation:

- this is a stronger and more direct explanation than the earlier `MAX_BLOCK_LEN` suspicion
- at least the contiguous `J=128` baremetal failures are not yet clean evidence about:
  - auto-tiler `tile_J`
  - multi-segment `j` DMA behavior
  - chunk-drain semantics
- first fix the confirmed contiguous alias-page overlap so bias and `C` are disjoint
- then rerun the same control matrix
- after that, check whether interleaved `J=128` still fails independently

## Attempt 11. `r6` on-demand replay is alive, but the baremetal gate front-loads a very long CPU-only phase

Infrastructure state:

- user asked to revert temporary F2 spot-instance experiments
- runtime configs are back to `run_instance_market: ondemand`
- an old leaked on-demand runfarm instance was reused:
  - `i-03f210861ef3784c0`
  - private IP `192.168.1.168`
- completed sessions:
  - `baremetal-chunk-r6-infrasetup`
  - `baremetal-chunk-r6-runworkload` is still running

Important live observation from `r6`:

- guest `uartlog` prints only:
  - `[resadd-explicit-baremetal] gemmini_mgr=0 local_gemmini=0 num_gemmini=2`
- but `heartbeat.csv` keeps advancing steadily
- manager status also keeps reporting:
  - `1/1 simulations are still running`
- remote `FireSim-f2` stays at ~100% CPU

New code-level interpretation:

- this does **not** currently look like a dead simulator or a stuck manager
- in [rerocc_lc_resadd_explicit_interleaved.c](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/bareMetalC/learn-gemmini/rerocc_lc_resadd_explicit_interleaved.c), the first banner is followed by a long guest-side preprocessing phase before any `CASE_START` is printed:
  - `pointwise_matmul_reference()`
  - `pointwise_matmul_reference_dim_j(..., 128)`
  - `pointwise_matmul_chunked_reference()`
- those three routines alone perform:
  - `4,194,304 + 8,388,608 + 8,388,608 = 20,971,520` MACs
- the access pattern to `B` is column-wise:
  - `b[k * PW_B_STRIDE + j]`
  - with `PW_B_STRIDE = 256`
- on Rocket this is cache-unfriendly and can realistically expand into multi-billion target-cycle runtime before the first real Gemmini case begins

Why this matters:

- a long silent `uartlog` here is not enough evidence of a hang
- this baremetal regression is still useful for correctness, but it is **not** as quick as initially assumed
- future engineering cleanup should move or precompute the CPU golden/reference work so the first `CASE_START` appears much earlier

## Attempt 12. `r7` init markers prove the pre-case path is alive, but dense logs distort timing

Replay:

- results dir:
  [2026-03-25--11-49-09-rerocc-lc-baremetal-coupleddma-explicit-interleaved-small-f2-rerocc-baremetal-explicit-interleaved-small](/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-25--11-49-09-rerocc-lc-baremetal-coupleddma-explicit-interleaved-small-f2-rerocc-baremetal-explicit-interleaved-small)
- runfarm instance:
  - `i-0b29fa7121e664f54`
  - private IP `192.168.1.200`

Instrumentation added:

- `thread_entry()` phase markers before and after:
  - `init_resadd_a`
  - `init_resadd_b`
  - `resadd_reference`
  - `init_pw_a`
- per-function markers for:
  - `init_random_elem()`
  - `init_random_acc()`
  - `resadd_reference()`
  - `pointwise_matmul_reference*()`

Observed live boundary before terminating the run:

- guest printed:
  - `INIT_PHASE phase=thread_entry_after_banner`
  - `INIT_PHASE phase=before_init_resadd_a`
  - `INIT_FN_END fn=init_random_elem ...`
  - `INIT_PHASE phase=after_init_resadd_a`
  - `INIT_PHASE phase=before_init_resadd_b`
  - `INIT_FN_END fn=init_random_elem ...`
  - `INIT_PHASE phase=after_init_resadd_b`
  - `INIT_PHASE phase=before_resadd_reference`
  - `INIT_FN_END fn=resadd_reference ...`
  - `INIT_PHASE phase=after_resadd_reference`
  - `INIT_PHASE phase=before_init_pw_a`

Interpretation update:

- the old symptom `banner only` was at least partly an observability problem
- the guest was making forward progress through CPU-side fixture generation before the first real Gemmini case
- therefore the current baremetal boundary is no longer “before any useful work”
- it is already inside the pointwise fixture path

Second finding from the same replay:

- quarter-step `printf` in tiny loops is too invasive for FireSim F2
- even `1536`-element init/reference loops blew up into hundreds of millions of target cycles once every quarter-step was logged
- that level of logging is not suitable for the next narrowing pass

Action taken after `r7`:

- terminate the runfarm promptly
- keep the phase-level markers
- reduce `init_random_*` / `resadd_reference` to `start/end + delta`
- keep only sparse progress in the truly large pointwise reference path for the next replay

## Attempt 13. `r8` sparse logging shows the first full pointwise CPU reference completes, and it is expensive

Replay:

- results dir:
  [2026-03-25--12-03-47-rerocc-lc-baremetal-coupleddma-explicit-interleaved-small-f2-rerocc-baremetal-explicit-interleaved-small](/home/ubuntu/chipyard/sims/firesim/deploy/results-workload/2026-03-25--12-03-47-rerocc-lc-baremetal-coupleddma-explicit-interleaved-small-f2-rerocc-baremetal-explicit-interleaved-small)
- runfarm instance:
  - `i-09e4b14b91d5474cd`
  - private IP `192.168.1.202`

Instrumentation policy in this round:

- keep:
  - phase markers
  - `start/end + delta` for small init/reference helpers
  - sparse row checkpoints for `pointwise_matmul_reference()`
- drop:
  - dense quarter-step prints from tiny loops

Observed boundary:

- guest crossed all preprocessing before the first big pointwise golden:
  - `after_init_pw_chunk_bias`
  - `after_memset_pw_init_c`
  - `after_copy_pw_golds`
  - `before_pointwise_reference_full`
- then entered:
  - `INIT_FN_START fn=pointwise_matmul_reference ... I=256 J=64 K=256`
- and successfully advanced through sparse row checkpoints:
  - `row=0`
  - `row=63`
  - `row=127`
  - `row=191`
  - `row=255`
- finally completed:
  - `INIT_FN_END fn=pointwise_matmul_reference ... delta=1220901896`
- after that, guest continued onward into:
  - `INIT_PHASE phase=after_pointwise_reference_full`
  - `INIT_PHASE phase=before_pointwise_reference_dim_j`
  - `INIT_FN_START fn=pointwise_matmul_reference_dim_j ... J=128`

Updated interpretation:

- the front-loaded baremetal delay is real CPU work, not a dead simulator
- even with sparse logging, the first `256 x 64 x 256` pointwise golden reference alone costs about:
  - `1.22B` target cycles
- so a replay that is killed after only a couple of billion cycles can easily die before any `CASE_START`
- the next heavy stages are even larger:
  - `pointwise_matmul_reference_dim_j(..., 128)`
  - `pointwise_matmul_chunked_reference()`

Practical conclusion:

- for this baremetal gate, “no `CASE_START` yet” is no longer a useful stop criterion by itself
- future stop criteria should key off:
  - lack of forward movement in `INIT_PHASE/INIT_FN_*`
  - stagnant `heartbeat.csv`
  - or a much larger cycle/time budget than earlier probes used
- this run was terminated after the key conclusion to avoid paying for the remaining long CPU-only golden path
