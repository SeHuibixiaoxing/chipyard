# FireSim F2 6p2c AGFI Smoke Note

Date: 2026-05-12

## Scope

This note records the 6-pair/2-core/sbus64/NIC hardware-debug AGFI smoke test
before attempting pipeline-runtime mapping or Linux workloads.

The AGFI under test is the new synthesizable-output 6p2c build that was produced
near the validated 1p1c build.

## Target

- Target name:
  `firesim_gemmini_rerocc_pairmanager_dummy8x8_2c6p6_sbus64_nic_hwdebug`
- AGFI:
  `agfi-0d0fc22b1ba532727`
- AFI:
  `afi-025d2a3afc4a7c9ca`
- Deploy quintuplet:
  `f2-firesim-FireSim-FireSimGemminiReRoCCPairDummy8x8C2P6Sbus64NICDebugConfig-WithTargetCycleDebug_WithPrintfSynthesis_WithSynthAsserts_FRFCFS16GBQuadRank_BaseF2Config`
- HWDB:
  `sims/firesim/deploy/config_hwdb_f2_gemmini_rerocc_pairmanager_dummy8x8_2c6p6_sbus64_nic_hwdebug.yaml`
- Build recipe:
  `sims/firesim/deploy/config_build_recipes_f2_gemmini_rerocc_pairmanager_dummy8x8_2c6p6_sbus64_nic_hwdebug.yaml`

The AGFI is available in AWS and reports shell version `0x10212415`.

## Runs

### 1p1c TargetCycleDebug Control Run

The known-working 1p1c hardware-debug AGFI also uses TargetCycleDebug:

- Target config:
  `FireSimGemminiReRoCCPairDummy8x8C1P1Sbus64NICDebugConfig`
- Platform config:
  `WithTargetCycleDebug_WithPrintfSynthesis_WithSynthAsserts_FRFCFS16GBQuadRank_BaseF2Config`
- AGFI:
  `agfi-098bce7d5e0c3d937`
- Result:
  `sims/firesim/deploy/results-workload/2026-05-12--12-25-50-rerocc-lc-baremetal-cfg32-slot-smoke-quick-f2-rerocc-baremetal-cfg32-slot-smoke-quick-1c1p1-nic-hwdebug-tcdlite/`

Key `uartlog` lines:

- `TARGETCYCLE DEBUG enabled widget=0 dump_limit=1 mask_chunks=8 labels hport=0 wire_in=9 wire_out=256 rv_in=4 rv_out=6`
- `FireSim fingerprint: 0x46697265`
- `TARGETCYCLE DEBUG [tick] widget=0 hcycle=1187939002 ...`
- `*** PASSED *** after 44104832 cycles`

This is the direct control proving that TargetCycleDebug is not, by itself, a
fatal feature on F2.

### Driver-Debug Smoke

Runtime:
`sims/firesim/deploy/config_runtime_f2_gemmini_rerocc_pairmanager_dummy8x8_2c6p6_sbus64_nic_hwdebug_hello_baremetal_driverdebug.yaml`

Result:
`sims/firesim/deploy/results-workload/2026-05-12--14-31-46-hello-baremetal-f2-hello-baremetal-2c6p6-nic-hwdebug-driverdebug/`

Key `uartlog` lines:

- `FireSim fingerprint: 0x46697265`
- `Commencing simulation.`
- `FIRESIM DRIVER DEBUG [before_step] step=0 step_size=4294967295 done=0 tcycle=0 hcycle=0`
- `FIRESIM DRIVER DEBUG [poll] step=0 poll=100000 total_poll=100000 done=0 tcycle=0 hcycle=0`
- `Simulator deadlock detected at target cycle 0. Terminating.`
- `*** FAILED *** (code = 1) after 0 cycles`

### Driver-Debug plus TargetCycleDebug Smoke

Runtime:
`sims/firesim/deploy/config_runtime_f2_gemmini_rerocc_pairmanager_dummy8x8_2c6p6_sbus64_nic_hwdebug_hello_baremetal_driverdebug_targetcycle.yaml`

Commands were run through `scripts/firesim-tmux-run.sh`:

- `launchrunfarm`
  log:
  `sims/firesim/deploy/logs/2026-05-12--14-47-04-launchrunfarm-HOBP5KG96OBL8V0G.log`
  instance:
  `i-0f1f691e4b50094ad`, private IP `192.168.1.38`
- `infrasetup`
  log:
  `sims/firesim/deploy/logs/2026-05-12--14-47-45-infrasetup-B158MNOLRON1BAS7.log`
  preflight:
  `FireSim driver readiness preflight passed for slot 0`
- `runworkload`
  log:
  `sims/firesim/deploy/logs/2026-05-12--14-51-44-runworkload-JM7OLJ3GALR1CUQ7.log`
  result:
  `sims/firesim/deploy/results-workload/2026-05-12--14-51-44-hello-baremetal-f2-hello-baremetal-2c6p6-nic-hwdebug-driverdebug-targetcycle/`
- `terminaterunfarm`
  log:
  `sims/firesim/deploy/logs/2026-05-12--14-53-11-terminaterunfarm-2X01XX6ZQZFWS9EY.log`

Key `uartlog` lines:

- `TARGETCYCLE DEBUG enabled widget=0 dump_limit=1 mask_chunks=8 labels hport=0 wire_in=9 wire_out=256 rv_in=4 rv_out=6`
- `FIRESIM DRIVER DEBUG enabled interval=100000`
- `FireSim fingerprint: 0x46697265`
- `FIRESIM DRIVER DEBUG [before_step] step=0 step_size=4294967295 done=0 tcycle=0 hcycle=0`
- `FIRESIM DRIVER DEBUG [poll] step=0 poll=100000 total_poll=100000 done=0 tcycle=0 hcycle=0`
- `Simulator deadlock detected at target cycle 0. Terminating.`
- `FIRESIM DRIVER DEBUG [terminate] step=0 poll=100001 total_poll=100001 bridge=heartbeat exit_code=1 tcycle=0 hcycle=0`
- `*** FAILED *** (code = 1) after 0 cycles`

TargetCycleDebug initialized but did not emit a blocker dump before heartbeat
terminated the run. That means the first-step hang is earlier than, or outside,
the simple ready/valid blocker classes currently captured by that widget.

No F2 instances remained after termination; AWS `describe-instances` returned
`[]` for active/stopped `f2.*` instances.

## Source-Backed Failure Boundary

The failure is below Linux, ReRoCC software, mapping files, and pipeline-runtime
DMA completion:

- `simulation_t::execute_simulation_flow()` reaches `Commencing simulation`
  after preflight, stream setup, and DRAM/program loading.
- `firesim_top_t::simulation_run()` writes the first
  `peek_poke.step(get_largest_stepsize(), false)` request, then polls
  `peek_poke.is_done()` while ticking bridge drivers.
- `peek_poke_t::step()` only writes the STEP register; completion authority is
  the generated PeekPoke DONE register.
- `heartbeat_t::tick()` reports deadlock because `clock.tcycle()` remains equal
  to the previous target cycle.

The log shows the generated model never raises DONE for step 0 and both
ClockBridge counters stay at zero through 100001 polls. Therefore the first
FAME token never completes. A `hello-baremetal` binary cannot execute even one
target cycle on this AGFI.

The code path is:

- `firesim_top_t::simulation_run()` writes `peek_poke.step(step_size, false)`.
- `peek_poke_t::step()` writes the generated `STEP` register.
- `PeekPokeBridgeModule` clears `DONE` when `cycleHorizon` is non-zero and only
  raises `DONE` again after `tCycleWouldAdvance` drains the requested tokens.
- `tCycleWouldAdvance` is the AND of the bridge's channel decoupling flags.
- `ClockBridgeModule` increments target cycle only when its clock token channel
  fires.

Since `ClockBridge` target and host cycle reads remain zero, the failure is not
guest code running slowly; at least one token path required for the first
simulator target cycle is not firing.

TargetCycleDebug did not contradict this. The widget currently observes selected
top-level wire/ready-valid channel maps, and `FPGATop` intentionally sets
`hportLabels = Seq.empty`. `ClockTokenVector.bridgeChannels()` is also empty.
Therefore the current TargetCycleDebug instance does not directly observe the
ClockBridge token hPort or every bridge hPort. The absence of a blocker dump on
6p2c only says the selected 9 wire inputs, 256 wire outputs, 4 RV inputs, and 6
RV outputs did not trip the widget's derived problem masks before heartbeat
terminated. It does not prove the first-token path is clean.

Generated driver/header scale also differs substantially:

- 1p1c TargetCycleDebug generated header:
  about 10.5k lines and 1,822 synthesized assertion strings.
- 6p2c TargetCycleDebug generated header:
  about 45.5k lines and 10,349 synthesized assertion strings.

Both report the same TargetCycleDebug label counts because the widget labels are
clipped/selected at the FPGATop channel boundary. That count is not a measure of
the full internal 6p2c target complexity.

## What This Is Not

- Not a pipeline-runtime mapping bug: the target cannot advance enough to boot
  or run hello.
- Not a Linux image issue: the same failure appears on bare-metal hello.
- Not a `doneflag`/DMA completion issue: no ReRoCC/DMA test was reached, and
  the evidence uses FireSim driver DONE/ClockBridge/heartbeat.
- Not the earlier 1p1c driver-tar mismatch: this run used the deploy
  quintuplet-derived driver path, and preflight fingerprint passed.
- Not simply "TargetCycleDebug is enabled": the 1p1c TargetCycleDebug AGFI
  completed the bare-metal smoke workload.
- Not simply "Vivado reported a VIOLATED checkpoint": the working 1p1c AGFI
  also has a `post_route.VIOLATED.dcp`. The 6p2c timing reports remain a risk,
  but the violation status alone is not a sufficient explanation.

## Current Root-Cause Boundary

The observed failure is pinned below guest software and below generated-model
semantics:

- The exact 6p2c NIC+TargetCycleDebug Verilator metasim passes bare-metal
  hello.
- The F2 AGFI reaches FireSim fingerprint and master-widget init.
- Before the first F2 driver step, `PeekPoke` already reads `DONE=0` and
  `ClockBridge` reads `hCycle=0`, even though the generated RTL should have
  `DONE=1` and a nonzero host cycle after reset release.

The remaining unknown is the exact FPGA implementation mechanism that corrupts
that host-control state. The leading suspect is timing/resource pressure in
this large debug-heavy 6p2c F2 build, not a functional RTL deadlock and not
TargetCycleDebug alone.

## Local Metasim Boundary Test

An exact local Verilator metasim boundary test passed for the same 6p2c
NIC+TargetCycleDebug triplet:

- Runtime:
  `sims/firesim/deploy/config_runtime_local_metasim_gemmini_rerocc_pairmanager_dummy8x8_2c6p6_sbus64_nic_hwdebug_hello_baremetal_targetcycle.yaml`
- HWDB:
  `sims/firesim/deploy/config_hwdb_f2_gemmini_rerocc_pairmanager_dummy8x8_2c6p6_sbus64_nic_hwdebug.yaml`
- Build recipe:
  `sims/firesim/deploy/config_build_recipes_f2_gemmini_rerocc_pairmanager_dummy8x8_2c6p6_sbus64_nic_hwdebug.yaml`
- `launchrunfarm` log:
  `sims/firesim/deploy/logs/2026-05-12--15-07-50-launchrunfarm-CFMKZKCOEFL2DLRB.log`
- `infrasetup` log:
  `sims/firesim/deploy/logs/2026-05-12--15-08-11-infrasetup-WVRXYQM7XGLD0OPY.log`
- `runworkload` log:
  `sims/firesim/deploy/logs/2026-05-12--15-42-41-runworkload-Z9MYUWIOQIMSBRCT.log`
- Result:
  `sims/firesim/deploy/results-workload/2026-05-12--15-42-41-hello-baremetal-local-metasim-hello-baremetal-2c6p6-nic-hwdebug-targetcycle/`

Key `metasim_stderr.out` lines:

- `FIRESIM DRIVER DEBUG [before_step] step=0 step_size=5000000 done=1 tcycle=0 hcycle=7546`
- `FIRESIM DRIVER DEBUG [poll] step=0 poll=1 total_poll=1 done=0 tcycle=11 hcycle=7562`
- `Hello world from core 0, a rocket`
- `FIRESIM DRIVER DEBUG [terminate] step=0 poll=2500 total_poll=2500 bridge=tsibridge exit_code=0 tcycle=31874 hcycle=59026`
- `*** PASSED *** after 31874 cycles`

This proves the generated 6p2c NIC+TargetCycleDebug model can accept a step,
advance target cycles, retire Rocket instructions, and terminate normally when
executed as the exact Verilator metasim deploy triplet. A larger metasim host is
not needed for this boundary test; the local bare-metal run already separates
generated-model semantics from F2 implementation behavior.

## Why 1p1c TargetCycleDebug Works But 6p2c F2 Does Not

The 1p1c and 6p2c hardware-debug build recipes use the same FireSim platform
configuration:

- `WithTargetCycleDebug_WithPrintfSynthesis_WithSynthAsserts_FRFCFS16GBQuadRank_BaseF2Config`
- `fpga_frequency: 20`
- `build_strategy: TIMING`

The target configurations differ only in the Gemmini/ReRoCC target size:

- 1p1c:
  `FireSimGemminiReRoCCPairDummy8x8C1P1Sbus64NICDebugConfig`
- 6p2c:
  `FireSimGemminiReRoCCPairDummy8x8C2P6Sbus64NICDebugConfig`

The runtime evidence also differs before any guest software can matter:

- 1p1c F2 TargetCycleDebug run:
  `TARGETCYCLE DEBUG [tick] ... hcycle=1187939002 ...` and
  `*** PASSED *** after 44104832 cycles`.
- 6p2c F2 TargetCycleDebug run:
  `FIRESIM DRIVER DEBUG [before_step] step=0 ... done=0 tcycle=0 hcycle=0`,
  then heartbeat terminates at target cycle 0.
- 6p2c exact local metasim:
  `FIRESIM DRIVER DEBUG [before_step] step=0 ... done=1 tcycle=0 hcycle=7546`,
  then `*** PASSED *** after 31874 cycles`.

The source makes this distinction precise:

- `PeekPokeBridgeModule` initializes `cycleHorizon` to zero and drives `DONE`
  from `cycleHorizon === 0`. Before the first driver step, a healthy model
  should read `DONE=1`. The 6p2c metasim does; the 6p2c F2 AGFI reads `DONE=0`.
- `ClockBridgeModule` increments `hCycle` unconditionally on every host-model
  clock after reset. The 6p2c metasim reads a nonzero `hCycle`; the 6p2c F2 AGFI
  reads zero repeatedly.
- `SimulationMaster`, `PeekPokeBridgeModule`, and `ClockBridgeModule` are wired
  to the same generated `clock` and `reset` in `F1Shim`. `wait_for_init()` and
  fingerprint readback prove the top-level control path can reach at least the
  master widget, but the downstream host-control register state for
  PeekPoke/ClockBridge is already invalid before workload execution.

So the current root-cause boundary is not "TargetCycleDebug breaks 6p2c".
TargetCycleDebug is present in the passing 1p1c AGFI and in the passing 6p2c
metasim. The failing object is this 6p2c F2 implementation/AGFI: the host
control state needed to start simulation is not reliable on FPGA.

The most plausible implementation-level reason is timing/resource pressure:

- 1p1c post-synth utilization:
  `370529 LUTs (28.42%), 197708 FFs (7.58%), DSP 179 (1.98%)`.
- 6p2c post-synth utilization:
  `809835 LUTs (62.12%), 450821 FFs (17.29%), DSP 999 (11.07%)`.
- Both builds produced `post_route.VIOLATED.dcp`, so the presence of a violated
  checkpoint alone is not the explanation.
- The 6p2c build has worse final setup/TNS:
  `WNS=-2.005 | TNS=-3043.743`, versus 1p1c
  `WNS=-1.838 | TNS=-2322.467`.
- Vivado reported the 6p2c post-route/post-phys-opt violation as too large for
  post-route physical optimization to recover, and the failing paths include
  shell DDR/reset/status clock-domain paths such as
  `WRAPPER/CL/SH_DDR/SYNC_RST/... -> WRAPPER/CL/SH_DDR/ddr_stat...`.

Those reported worst paths are not the exact ClockBridge register itself, but
the runtime symptom proves the generated model is not functioning correctly
after F2 implementation. The correct working conclusion is: 6p2c plus this
debug-heavy platform mix produces a bad F2 bitstream; 1p1c with the same
TargetCycleDebug feature remains small enough to work despite unrelated shell
timing violations.

## Recommended Next Test Plan

1. Do not run Linux or pipeline-runtime mapping on
   `agfi-0d0fc22b1ba532727`; it fails before the first target cycle.
2. Rebuild a 6p2c F2 isolation AGFI with the same target config but without
   `WithTargetCycleDebug`, keeping the rest of the platform as close as
   possible. This answers whether TargetCycleDebug's added debug fanout is the
   tipping point for the large target.
3. If that still fails, rebuild a lean 6p2c AGFI without
   `WithPrintfSynthesis`/`WithSynthAsserts` as well. This checks whether debug
   collateral and generated assertion/printf fabric are what push placement and
   routing over the edge.
4. For every rebuilt AGFI, run the same bare-metal hello smoke first with
   `+firesim-driver-debug`. The required pass condition is:
   pre-step `DONE=1`, nonzero `hCycle`, target-cycle advancement, and
   `*** PASSED ***`.
5. Only after a 6p2c F2 AGFI passes bare-metal hello should pipeline-runtime
   mapping and DMA tests be attempted.


## Pipeline Runtime Decision

Do not generate or validate 6p mapping on this AGFI yet. The bitstream is not
usable for pipeline runtime until a bare-metal hello smoke advances target
cycles and reports FireSim `*** PASSED ***`.
