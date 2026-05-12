# FireSim F2 6p2c AGFI Smoke and Root-Cause Boundary

Date: 2026-05-12

## Scope

This note records the investigation of the 6p2c/sbus64/NIC hardware-debug F2
AGFI with synthesizable debug output, compared against the nearby working 1p1c
hardware-debug AGFI. The goal is to decide whether the 6p2c bitstream is usable
for Linux and pipeline-runtime testing, and to separate source drift from
implementation failure.

## Short Conclusion

The current 6p2c AGFI is not usable for Linux or pipeline-runtime tests:

- 6p2c F2 AGFI `agfi-0d0fc22b1ba532727` reaches FireSim fingerprint, but the
  first driver step never completes.
- Before the first step, 6p2c F2 already reads `PeekPoke DONE=0`,
  `ClockBridge tcycle=0`, and `ClockBridge hcycle=0`.
- The exact 6p2c Verilator metasim triplet passes bare-metal hello, with
  pre-step `DONE=1` and nonzero `hcycle`.
- The 1p1c F2 AGFI with the same `WithTargetCycleDebug` platform config passes
  the bare-metal smoke workload.

The source-backed boundary is therefore: this is not a Linux, workload, mapping,
or pipeline-runtime DMA problem. It is also not explained by FireSim or other
repo code drift between the two build times. The failure is in the 6p2c F2
implemented bitstream/control state after FPGA implementation.

## AGFIs and Runs

Working 1p1c control:

- Build result:
  `sims/firesim/deploy/results-build/2026-05-11--11-53-05-firesim_gemmini_rerocc_pairmanager_dummy8x8_1c1p1_sbus64_nic_hwdebug/`
- AGFI/AFI: `agfi-098bce7d5e0c3d937` / `afi-0d63b7450829af6c6`
- Target config:
  `FireSimGemminiReRoCCPairDummy8x8C1P1Sbus64NICDebugConfig`
- Platform config:
  `WithTargetCycleDebug_WithPrintfSynthesis_WithSynthAsserts_FRFCFS16GBQuadRank_BaseF2Config`
- Passing result:
  `sims/firesim/deploy/results-workload/2026-05-12--12-25-50-rerocc-lc-baremetal-cfg32-slot-smoke-quick-f2-rerocc-baremetal-cfg32-slot-smoke-quick-1c1p1-nic-hwdebug-tcdlite/`
- Key lines:
  `TARGETCYCLE DEBUG enabled ... labels hport=0 wire_in=9 wire_out=256 rv_in=4 rv_out=6`,
  `FireSim fingerprint: 0x46697265`,
  `*** PASSED *** after 44104832 cycles`.

Failing 6p2c F2:

- Build result:
  `sims/firesim/deploy/results-build/2026-05-11--17-15-12-firesim_gemmini_rerocc_pairmanager_dummy8x8_2c6p6_sbus64_nic_hwdebug/`
- AGFI/AFI: `agfi-0d0fc22b1ba532727` / `afi-025d2a3afc4a7c9ca`
- Target config:
  `FireSimGemminiReRoCCPairDummy8x8C2P6Sbus64NICDebugConfig`
- Platform config:
  `WithTargetCycleDebug_WithPrintfSynthesis_WithSynthAsserts_FRFCFS16GBQuadRank_BaseF2Config`
- Failing result:
  `sims/firesim/deploy/results-workload/2026-05-12--14-51-44-hello-baremetal-f2-hello-baremetal-2c6p6-nic-hwdebug-driverdebug-targetcycle/`
- Key lines:
  `TARGETCYCLE DEBUG enabled ... labels hport=0 wire_in=9 wire_out=256 rv_in=4 rv_out=6`,
  `FireSim fingerprint: 0x46697265`,
  `FIRESIM DRIVER DEBUG [before_step] step=0 step_size=4294967295 done=0 tcycle=0 hcycle=0`,
  `Simulator deadlock detected at target cycle 0. Terminating.`,
  `*** FAILED *** (code = 1) after 0 cycles`.

Passing 6p2c exact metasim:

- Runtime:
  `sims/firesim/deploy/config_runtime_local_metasim_gemmini_rerocc_pairmanager_dummy8x8_2c6p6_sbus64_nic_hwdebug_hello_baremetal_targetcycle.yaml`
- Result:
  `sims/firesim/deploy/results-workload/2026-05-12--15-42-41-hello-baremetal-local-metasim-hello-baremetal-2c6p6-nic-hwdebug-targetcycle/`
- Key lines:
  `TARGETCYCLE DEBUG enabled ... labels hport=0 wire_in=9 wire_out=256 rv_in=4 rv_out=6`,
  `FireSim fingerprint: 0x46697265`,
  `Hello world from core 0, a rocket`,
  `COMMAND_EXIT_CODE="0"`.

## Build-Time Commit Comparison

`AGFI_INFO` records the FireSim commit embedded in the AFI description:

- 1p1c AGFI: FireSim `91888035e601638b356f98aa70793b4005cbf653`
- 6p2c AGFI: FireSim `f2beb072ebdb6a6f4e189ff5d9234e25de3be60d`

The FireSim diff between those commits is not a driver/RTL/source-flow change.
It only:

- updates the 1p1c hwdb AGFI from an older value to
  `agfi-098bce7d5e0c3d937`;
- adds a 1p1c bare-metal smoke runtime YAML.

The nearby top-level Chipyard commits are:

- 1p1c build launch: `650e5f55` at `2026-05-11 11:52:56 +0000`
- 6p2c build launch/restart window: `8c72fe13` at
  `2026-05-11 12:13:48 +0000`
- 6p2c AGFI build completion window: `eb0e5401` at
  `2026-05-11 17:06:44 +0000`

From `650e5f55` to `eb0e5401`, the hardware-relevant submodules checked in at
the top level stayed fixed except for `sims/firesim` and `generators/gemmini`:

- unchanged: `fpga/fpga-shells`, `generators/rocket-chip`,
  `generators/testchipip`, `generators/diplomacy`,
  `generators/rocket-chip-blocks`, `generators/rocket-chip-inclusive-cache`;
- `sims/firesim` moved from `91888035` to `f2beb072`, but that diff is only
  the 1p1c hwdb/runtime change described above;
- `generators/gemmini` moved from `aa4cbc4` to `3f03088`.

The `generators/gemmini` diff is also not a hardware-source change. It only
moves `software/gemmini-rocc-tests`; the rocc-tests diff from the 1p1c build
pointer to the 6p2c build pointer modifies one markdown note:

- `pipeline-runtime/docs/testing/custom_instruction_debug_strategy_20260510.md`

No Scala/Chisel, C/C++, FireSim driver, FPGA shell, Rocket, TestChipIP, or
Gemmini hardware source changed in this time window in a way that explains the
6p2c failure. This is why the generated RTL and reports below are stronger
evidence than the commit messages.

Current `sims/firesim` has later repository-structure changes after `f2beb072`
including the in-tree F2 AWS shell conversion. Those later changes were not in
the two AGFIs above. They matter for future rebuild reproducibility, but they
are not the cause of the already-created 6p2c AGFI failure.

## Source Boundary

The preflight/fingerprint path is not a full simulation-health test. In
`simulation_t::execute_simulation_flow()`, FireSim waits for init and checks the
master widget fingerprint before DRAM load and bridge init. Passing this only
proves that the manager can reach the master widget over the control path.

The first real simulation step is in `firesim_top_t::simulation_run()`:

- before the first step, driver debug reads `clock.tcycle()`, `clock.hcycle()`,
  and `peek_poke.is_done()`;
- then it writes `peek_poke.step(step_size, false)`;
- it polls `peek_poke.is_done()` while ticking bridge drivers.

The generated source says what those pre-step values should be:

- `PeekPokeBridgeModule` initializes `cycleHorizon` to zero and drives
  `DONE := cycleHorizon === 0.U`, so a healthy model should read `DONE=1`
  before the first step.
- `ClockBridgeModule` increments `hCycle` every host-model clock after reset,
  so a healthy FPGA after init should read a nonzero `hCycle`.

Observed values:

- 6p2c F2 reads `DONE=0`, `tcycle=0`, `hcycle=0` before the first step and never
  advances.
- 6p2c metasim reads healthy pre-step state and runs hello.
- 1p1c F2 with TargetCycleDebug runs the smoke workload to PASS.

This pins the failure below guest software and below FireSim model semantics:
the 6p2c F2 implementation does not present valid host-control/clock/reset
state for the generated simulator.

TargetCycleDebug also does not directly observe every relevant path. In
`FPGATop`, `hportLabels = Seq.empty`, and `ClockTokenVector.bridgeChannels()`
returns `Seq()`. The current debug widget observes selected wire and ready-valid
channel masks, not the ClockBridge token hPort itself. Therefore the absence of
a TargetCycleDebug blocker dump does not prove the clock-token path or
PeekPoke/ClockBridge control state is healthy.

## Generated RTL Comparison

Generated file scale:

- 1p1c `FireSim-generated.sv`: 621,855 lines, 53,504,410 bytes
- 6p2c `FireSim-generated.sv`: 2,993,299 lines, 240,935,449 bytes
- 1p1c `FireSim-generated.const.h`: 10,554 lines, 593,230 bytes
- 6p2c `FireSim-generated.const.h`: 45,517 lines, 2,470,702 bytes
- synthesized assertion label matches in generated headers:
  1p1c 1,982 matches, 6p2c 10,509 matches

Module-level hashes from the generated RTL:

| Module | 1p1c vs 6p2c |
| --- | --- |
| `SimulationMaster` | identical, 210 lines each |
| `PeekPokeBridgeModule` | identical, 384 lines each |
| `ClockBridgeModule` | identical, 335 lines each |
| `TargetCycleDebugWidget` | identical, 20,959 lines each |
| `F1Shim` | identical, 1,087 lines each |
| `AssertBridgeModule` | different, 3,908 lines vs 20,977 lines |
| `PrintBridgeModule` | different, 1,507 lines vs 5,374 lines |
| `FPGATop` | different, 108,773 lines vs 1,834,237 lines |
| `FireSim` | different, 13,715 lines vs 56,943 lines |

This is the key comparison: the failing host-control widget definitions are not
different between 1p1c and 6p2c. The large differences are the 6p2c target
logic and the debug collateral around asserts/printfs/top-level FAME wiring.

## Utilization and Timing

Both build recipes use the same platform settings:

- `WithTargetCycleDebug_WithPrintfSynthesis_WithSynthAsserts_FRFCFS16GBQuadRank_BaseF2Config`
- `fpga_frequency: 20`
- `build_strategy: TIMING`

Post-synth top utilization:

| Build | Total LUTs | FFs | DSP |
| --- | ---: | ---: | ---: |
| 1p1c | 370,529 (28.42%) | 197,708 (7.58%) | 179 (1.98%) |
| 6p2c | 809,835 (62.12%) | 450,821 (17.29%) | 999 (11.07%) |

Important hierarchy rows:

| Instance/module | 1p1c LUTs/FFs/DSP | 6p2c LUTs/FFs/DSP |
| --- | ---: | ---: |
| `firesim_top` / `F1Shim` | 334,284 / 154,383 / 176 | 773,128 / 407,219 / 996 |
| `top` / `FPGATop` | 334,284 / 154,381 / 176 | 773,128 / 407,217 / 996 |
| `AssertBridgeModule_0` | 3,666 / 196 / 0 | 31,982 / 198 / 0 |
| `PrintBridgeModule_0` | 1,237 / 7,987 / 0 | 24,651 / 37,903 / 0 |
| `TargetCycleDebugWidget_0` | 25,869 / 46,626 / 0 | 25,989 / 46,626 / 0 |
| `ClockBridgeModule_0` | 83 / 269 / 0 | 77 / 269 / 0 |
| `PeekPokeBridgeModule_0` | 272 / 275 / 0 | 271 / 275 / 0 |
| `SimulationMaster_0` | 130 / 177 / 0 | 128 / 177 / 0 |

Timing:

- 1p1c final post-route phys-opt:
  `WNS=-1.838 | TNS=-2322.467 | WHS=-3.595 | THS=-5160.769`
- 6p2c final post-route phys-opt:
  `WNS=-2.005 | TNS=-3043.743 | WHS=-3.631 | THS=-2048.133`
- Both produced `post_route.VIOLATED.dcp`, and the 1p1c violated checkpoint
  still passes the smoke workload. So the existence of a timing violation alone
  is not a sufficient explanation.
- This matches the AWS F2 HDK documentation: the flow can still emit a DCP
  tarball when timing failures are present, but functionality is not guaranteed
  and such images are only suitable for testing
  (`https://awsdocs-fpga-f2.readthedocs-hosted.com/latest/hdk/README.html`).
  Therefore timing reports must be kept as risk evidence, while FireSim workload
  pass/fail remains the runtime authority.
- The 6p2c route log additionally reports timing congestion level 6:
  `Congestion levels of 5 and greater may impact timing closure`.
- Worst visible timing paths are mostly shell DDR/reset/status or clock-domain
  paths, for example `WRAPPER/CL/SH_DDR/SYNC_RST/...` and
  `WRAPPER/CL/PIPE_DDR_STAT_*`; many endpoints are hidden by shell collateral.

The reports do not expose a single exact failing `ClockBridge` or `PeekPoke`
register path. The stronger evidence is the runtime/code mismatch: the same
generated host-control modules simulate correctly, but their FPGA readback state
is already invalid before step 0 on the 6p2c AGFI.

## Root-Cause Boundary

Pinned root-cause boundary:

- Not pipeline-runtime mapping: no target cycle executes.
- Not Linux/rootfs/workload: bare-metal hello fails at target cycle 0 on F2.
- Not `doneflag` or DMA completion: no DMA workload is reached.
- Not driver/AGFI recipe mismatch: fingerprint/preflight reaches the expected
  FireSim master widget, and the hwdb points at the tested AGFI.
- Not FireSim or other repo source drift between build times: build-window diffs
  are config/docs/submodule-pointer changes, not hardware or driver logic.
- Not TargetCycleDebug alone: 1p1c with TargetCycleDebug passes, and 6p2c
  TargetCycleDebug metasim passes.

Most likely cause:

The 6p2c target plus synthesized assert/printf/debug collateral creates a much
larger and more congested F2 implementation. That implementation does not
reliably deliver the host-control/clock/reset state required by the generated
simulator, even though the generated RTL semantics are valid. The exact physical
path still needs targeted instrumentation because Vivado hides or reports
mostly shell/reset/status path names, but the code and run evidence already pin
the problem to the implemented 6p2c FPGA image rather than to guest software or
FireSim model logic.

## Software Path Check

The failing 6p2c F2 run and the passing 6p2c metasim run use the same runtime
hardware config and deploy quintuplet:

- `RuntimeHWConfig:
  firesim_gemmini_rerocc_pairmanager_dummy8x8_2c6p6_sbus64_nic_hwdebug`
- `DeployQuintuplet:
  f2-firesim-FireSim-FireSimGemminiReRoCCPairDummy8x8C2P6Sbus64NICDebugConfig-WithTargetCycleDebug_WithPrintfSynthesis_WithSynthAsserts_FRFCFS16GBQuadRank_BaseF2Config`

The F2 hwdb entry for that config has no deploy override:

- `agfi: agfi-0d0fc22b1ba532727`
- `deploy_quintuplet_override: null`
- `custom_runtime_config: null`

The failed F2 `sim-run.sh` invokes `./FireSim-f2` with
`+prog0=hello-baremetal0-hello.riscv`; the passing metasim invokes
`./VFireSim` with the same program and the same normal passthrough debug args.
The metasim-only differences are `+max-cycles=5000000` and
`+fesvr-step-size=128`.

Those metasim-only args cannot explain the bad pre-step F2 state:

- `systematic_scheduler_t` initializes `default_step_size` to
  `MAX_MIDAS_STEP = 2^32 - 1`.
- Its constructor parses `+max-cycles=...`, but not `+fesvr-step-size=...`.
- `firesim_top_t::simulation_run()` calls `get_largest_stepsize()`, then
  reads `clock.tcycle()`, `clock.hcycle()`, and `peek_poke.is_done()` for the
  `FIRESIM DRIVER DEBUG [before_step]` line, and only after that calls
  `peek_poke.step(step_size, false)`.

Therefore the observed F2 line
`before_step ... done=0 tcycle=0 hcycle=0` is not caused by the value written to
`STEP`; it is read before any `STEP` write in that loop.

The FireSim fingerprint is also deliberately narrow. In
`simulation_t::execute_simulation_flow()`, the flow waits for
`master.is_init_done()` and checks `master.check_fingerprint()`. In the Scala
`SimulationMaster`, those are the master widget's `INIT_DONE` and
`PRESENCE_READ` registers. This proves the OCL/control path can reach the
master widget, but it does not prove the PeekPoke or ClockBridge registers have
healthy state.

The generated old 1p1c and 6p2c driver headers agree on the relevant
host-control register layout:

- `PEEKPOKEBRIDGEMODULE`: `STEP = 9228`, `DONE = 9232`,
  `PRECISE_PEEKABLE = 9240`
- `CLOCKBRIDGEMODULE`: `hCycle_0 = 9280`, `hCycle_1 = 9284`,
  `hCycle_latch = 9288`, `tCycle_0 = 9292`, `tCycle_1 = 9296`,
  `tCycle_latch = 9300`
- `SIMULATIONMASTER`: `INIT_DONE = 9376`, `PRESENCE_READ = 9380`,
  `PRESENCE_WRITE = 9384`

This makes a basic C++ struct/offset mismatch between those two old builds an
unlikely root cause for `done=0/hcycle=0`. It also explains why the new
diagnostic build adds direct PeekPoke/ClockBridge status words: the existing
fingerprint path validates only the master widget, and TargetCycleDebug does not
observe the ClockBridge token hPort.

## Should We Rebuild 6p2c From the 1p1c Commit?

Rebuilding 6p2c after checking out the exact 1p1c build-time repository state
is not the best first move. The build-window diff does not contain a plausible
hardware or driver source change:

- FireSim changed only 1p1c hwdb/runtime YAML.
- Chipyard changed only submodule pointers for FireSim and Gemmini.
- Gemmini changed only the `gemmini-rocc-tests` submodule pointer.
- `gemmini-rocc-tests` changed only a pipeline-runtime markdown note.
- FPGA shell, Rocket, TestChipIP, Diplomacy, Rocket-chip-blocks, and inclusive
  cache submodule pointers were unchanged.

So a 6p2c rebuild from the 1p1c top-level commit should generate essentially
the same hardware input for the 6p2c target, aside from incidental build-flow
environment effects. It can be used as a reproducibility control later, but it
is unlikely to fix the target-cycle-0 failure by itself.

The higher-value rebuild is a 6p2c diagnostic AGFI from the current consistent
recipe/hwdb state, preserving TargetCycleDebug/synth printf/synth asserts, with
extra narrow readback of reset, ClockBridge, PeekPoke, and the control bus. That
directly tests the failing pre-step state instead of hoping that a commit reset
changes an implementation-level symptom.

## Next Test Plan

1. Do not run Linux or pipeline-runtime mapping on
   `agfi-0d0fc22b1ba532727`; it fails before step 0 completes.
2. Rebuild a 6p2c diagnostic AGFI that preserves the existing hardware debug
   structures, including TargetCycleDebug, synth printf, and synth asserts.
3. Add a minimal always-on diagnostic readback path near the host-control
   boundary. The useful signals are reset released state, ClockBridge `hCycle`,
   ClockBridge token fire/valid/ready, PeekPoke `cycleHorizon`, PeekPoke `DONE`,
   and a small control-bus sanity register outside the large target fabric.
4. Run the same diagnostic on a 1p1c control AGFI and on 6p2c. Required
   pre-step pass condition: `DONE=1`, nonzero `hCycle`, and a visible clock
   token stream before the first driver step.
5. If the diagnostic shows reset/control readback corruption on 6p2c, inspect
   and adjust the F2 OCL/control/reset/floorplan constraints before changing
   target debug features.
6. If the diagnostic shows host-control state is healthy but the first token
   still does not fire, instrument the first blocked FAME channels explicitly.
   The current TargetCycleDebug mask is not enough because it does not observe
   ClockBridge hPort channels.
7. Only after a 6p2c F2 AGFI passes bare-metal hello with driver debug should
   pipeline-runtime mapping and DMA tests be attempted. DMA completion evidence
   must use `hw_dma_fence()` / blocking wait, not doneflag polling.

Removing TargetCycleDebug is not the primary fix. A no-debug or reduced-debug
build can still be useful later as an isolation A/B, but the main debugging path
should preserve the hardware debug structures and add narrower diagnostics that
pin the first bad reset/clock/control condition.

## Diagnostic Rebuild Plan

The next rebuilds preserve the existing hardware-debug structures:

- `WithTargetCycleDebug`
- `WithPrintfSynthesis`
- `WithSynthAsserts`
- `WithRocketSynthPCDebug` in the NIC target configs

They add a separate `WithHostControlDebug` platform config. This does not remove
or weaken the existing debug path. It adds four read-only ClockBridge words and
four read-only PeekPoke words, then gates host-side printing with
`+firesim-host-control-debug`.

The probe address calculation is pinned to FireSim code, not inferred from
logs:

- `WidgetMMIO` and `FPGATop` require `CtrlNastiKey.dataBits == 32`, and
  `simif_t::read()` is a 32-bit MMIO read.
- `MCRFileMap.allocate()` assigns each attached register at
  `bytesPerAddress * name2addr.size`, so with a 32-bit control bus each attach
  advances the address by 4 bytes.
- `ClockBridgeModule` allocates `hCycle_0`, `hCycle_1`, `hCycle_latch`,
  `tCycle_0`, `tCycle_1`, and `tCycle_latch` before the new host-control debug
  attaches. The C++ ClockBridge debug reads therefore start at
  `tCycle_latch + 4`.
- `PeekPokeBridgeModule` allocates `PRECISE_PEEKABLE` after all port registers
  and immediately before the new host-control debug attaches. The C++ PeekPoke
  debug reads therefore start at `PRECISE_PEEKABLE + 4`.

The first 1p1c diagnostic rebuild exposed one real diagnostic-code bug before
Vivado:

- GoldenGate failed in `ClockBridge.scala` while elaborating the 1-clock 1p1c
  target.
- The bad expression was `tokenBits.pad(32)` on a 1-bit Chisel `UInt` derived
  from `Vec[Bool].asUInt`.
- Chisel tried to clone a `Bool`-typed width through `cloneTypeWidth`, producing
  a host-build failure before Verilog generation.
- The fix is explicit width construction:
  `if (clockInfo.size >= 32) tokenBits(31, 0) else Cat(0.U((32 - clockInfo.size).W), tokenBits)`.

This was a build-time bug in the new diagnostic probe, not the original 6p2c
runtime failure. It is nevertheless important because it proves the diagnostic
path is being exercised on the small control target before we trust it on 6p2c.

Three comparable F2 diagnostic builds are prepared:

| Scale | Build config | Build recipe | Builder |
| --- | --- | --- | --- |
| 1p1c | `config_build_f2_gemmini_rerocc_pairmanager_dummy8x8_1c1p1_sbus64_nic_hwdebug_hostdebug_m8i.yaml` | `config_build_recipes_f2_gemmini_rerocc_pairmanager_dummy8x8_1c1p1_sbus64_nic_hwdebug_hostdebug.yaml` | `m8i.2xlarge` |
| 4p2c | `config_build_f2_gemmini_rerocc_pairmanager_dummy8x8_2c4p4_sbus64_nic_hwdebug_hostdebug.yaml` | `config_build_recipes_f2_gemmini_rerocc_pairmanager_dummy8x8_2c4p4_sbus64_nic_hwdebug_hostdebug.yaml` | `z1d.3xlarge` |
| 6p2c | `config_build_f2_gemmini_rerocc_pairmanager_dummy8x8_2c6p6_sbus64_nic_hwdebug_hostdebug.yaml` | `config_build_recipes_f2_gemmini_rerocc_pairmanager_dummy8x8_2c6p6_sbus64_nic_hwdebug_hostdebug.yaml` | `z1d.3xlarge` |
| 6p2c fallback | `config_build_f2_gemmini_rerocc_pairmanager_dummy8x8_2c6p6_sbus64_nic_hwdebug_hostdebug_m8i8.yaml` | `config_build_recipes_f2_gemmini_rerocc_pairmanager_dummy8x8_2c6p6_sbus64_nic_hwdebug_hostdebug.yaml` | `m8i.8xlarge` |
| 6p2c fallback | `config_build_f2_gemmini_rerocc_pairmanager_dummy8x8_2c6p6_sbus64_nic_hwdebug_hostdebug_m8i4swap.yaml` | `config_build_recipes_f2_gemmini_rerocc_pairmanager_dummy8x8_2c6p6_sbus64_nic_hwdebug_hostdebug.yaml` | `m8i.4xlarge` + 64GiB swap |

The 4p2c target is intentionally the same 8x8 dummy Gemmini, sbus64, two-core,
NIC hardware-debug family as 6p2c. It uses `numPairs = 4`, `pairX = 2`,
`pairY = 2`, and keeps the 6p2c queue-depth settings
`pairTlMaxInFlight = Some(64)` and `pairAtlMaxInFlight = Some(64)` so the A/B
primarily varies target size instead of queue policy.

Build-memory observation from the diagnostic rebuild:

- Running 4p2c and 6p2c GoldenGate locally at the same time pushed the manager
  host into heavy swap I/O and very high iowait.
- After stopping the 6p2c local GoldenGate, the 4p2c process continued alone at
  roughly 12-15 GiB RSS.
- The host already has about 105 GiB of swap, so the immediate limiter is not
  swap capacity; it is I/O thrashing when two large GoldenGate JVMs run
  concurrently on this manager.
- The 6p2c diagnostic build was restarted only after the 4p2c local GoldenGate
  phase moved to a remote Vivado builder. During the restarted 6p2c GoldenGate
  phase, swap remained far from full and no OOM evidence appeared; extra swap is
  therefore not the right fix unless future runs approach swap exhaustion.
- A live JVM stack sample during the restarted 6p2c local phase placed the main
  thread in `midas.passes.SimulationMapping` under
  `firrtl.transforms.RemoveWires`, specifically
  `firrtl.graph.DiGraph.linearize/getVertices` while resizing a Scala
  `LinkedHashSet`. This pins the memory/latency pressure to a concrete FIRRTL
  graph pass over the expanded debug-heavy design, not to FireSim manager
  polling, Linux workload setup, or AGFI registration.

6p2c build-host launch retry issue:

- The restarted 6p2c build completed the local GoldenGate and driver compile
  phases far enough to emit `FireSim-generated.sv` and the F2 driver collateral.
- It then failed before remote Vivado because AWS had no immediate
  `z1d.3xlarge` capacity in the supported subnets and reported `z1d.3xlarge`
  unsupported in `us-west-2d`.
- The code-level root cause for the immediate abort is in FireSim's
  `AWSEC2.request_build_host`: it called `launch_instances()` without
  a timeout argument, so `launch_instances()` used its default
  `timeout=timedelta(0)` and aborted after one pass over the subnets. This is
  different from run-farm launch, where `launch_instances_timeout_minutes` is
  parsed and passed through.
- The fix aligns build farm with run farm by parsing
  `launch_instances_timeout_minutes` in `AWSEC2` and forwarding it to
  `launch_instances()`. The hostdebug 1p1c/4p2c/6p2c build configs now set a
  60-minute retry window.
- Because `z1d.3xlarge` continued to have no immediate capacity, a separate
  6p2c fallback build config uses `m8i.8xlarge`. This changes only the remote
  Vivado builder instance type and build-farm tag; it does not change the
  FireSim build recipe, target config, platform config, or generated RTL.
- If large-instance capacity is still unavailable, a second fallback uses
  `m8i.4xlarge` with `build_host_swap_size_gb=64`. This is a capacity/memory
  mitigation for the remote builder only; it does not change the AGFI design
  inputs.
- A separate AWS quota check explains why fallback builders could not launch
  while 1p1c and 4p2c were both active: the account's On-Demand Standard quota
  is 32 vCPUs. The manager (`c5.2xlarge`, 8 vCPUs), the 1p1c builder
  (`m8i.2xlarge`, 8 vCPUs), and the 4p2c builder (`z1d.3xlarge`, 12 vCPUs)
  already consumed 28 vCPUs, leaving only 4. A new `z1d.3xlarge` needs 12
  vCPUs, `m8i.4xlarge` needs 16, and `m8i.8xlarge` needs 32, so the 6p2c
  remote Vivado builder cannot launch until an existing builder exits or the
  quota is raised.
- The release order matters. If the 1p1c builder exits first, only 8 vCPUs are
  freed and the active `m8i.4xlarge` 6p2c retry still cannot launch while the
  4p2c `z1d.3xlarge` builder remains active. That state can support a 12-vCPU
  `z1d.3xlarge` 6p2c retry if regional capacity is available. If the 4p2c
  builder exits first, the active `m8i.4xlarge` 6p2c retry has enough quota.
- A stale orphaned `m8i.8xlarge` 6p2c build process was found after its tmux
  session had already been stopped. It was still retrying a 32-vCPU builder and
  could have consumed the entire On-Demand Standard quota if it launched after
  resources freed. The orphaned process group was terminated; no
  `pairdummy8x8sbus64c2p6hwdbgdiag-m8i8` EC2 instance was running or pending.

Pipeline-runtime mapping preparation:

- Existing pipeline-runtime profiles do not contain a
  `dummy8x8 + 2-core + 6-pair + sbus64` target.
- A matching HybridMapper target key is
  `rerocc_globalnoc_pairmanager_dummy8x8_c2_g6_d6_spad1024kb_dram19_noc64_mac64_sbus64`.
- Its source hardware class is
  `GemminiLearningConfigSpadReRoCCGlobalNoC2C1x2P6x3x2CoupledDMAPairManagerDummy8x8Sbus64`.
- The target entry uses `num_cores=2`, `num_gemmini=6`, `num_dma=6`,
  `num_macs_per_array=64`, `sbus_width_bits=64`, and `memory_channels=1`,
  matching the Chisel source instead of copying the old 4c12p profile.
- Mapping generation should wait until local memory pressure drops; running
  HybridMapper while 6p2c GoldenGate is at peak RSS risks unnecessary swap
  pressure.

Required first smoke after any AGFI becomes available:

1. update hwdb to the new AGFI/AFI;
2. rerun `firesim infrasetup`;
3. run `hello-baremetal.json` with
   `+firesim-driver-debug +firesim-host-control-debug +targetcycle-debug=1`;
4. accept only if pre-step state shows `PeekPoke DONE=1`,
   `ClockBridge hCycle != 0`, and a sane ClockBridge token status.
