# FireSim F2 1c1p AGFI/Driver Consistency Note

Date: 2026-05-12

## Scope

This note records the current root-cause analysis for the 1c1p/sbus64/NIC
hardware-debug FireSim path and the temporary test plan. It intentionally uses
source code and FireSim manager logs as the authority; `debug_records/` and
`change_records/` are only secondary references.

## Relevant Configs

- HWDB:
  `sims/firesim/deploy/config_hwdb_f2_gemmini_rerocc_pairmanager_dummy8x8_1c1p1_sbus64_nic_hwdebug.yaml`
- Build recipe:
  `sims/firesim/deploy/config_build_recipes_f2_gemmini_rerocc_pairmanager_dummy8x8_1c1p1_sbus64_nic_hwdebug.yaml`
- Conservative runtime:
  `sims/firesim/deploy/config_runtime_f2_gemmini_rerocc_pairmanager_dummy8x8_1c1p1_sbus64_nic_hwdebug_baremetal_cfg32_slot_smoke_quick_tcdlite.yaml`
- AGFI under test:
  `agfi-098bce7d5e0c3d937`

The AGFI description advertises this deploy quintuplet:

`f2-firesim-FireSim-FireSimGemminiReRoCCPairDummy8x8C1P1Sbus64NICDebugConfig-WithTargetCycleDebug_WithPrintfSynthesis_WithSynthAsserts_FRFCFS16GBQuadRank_BaseF2Config`

The HWDB must not override that path with an ad-hoc driver bundle while this
AGFI is being qualified. The current fix is to remove `driver_tar` from the 1c1p
HWDB entry so `infrasetup` builds and packages the driver from the AGFI-derived
deploy quintuplet.

## Preflight Code Path

FireSim driver readiness preflight is not a Linux or workload test. The manager
runs the simulator binary with `+check-fingerprint`.

Source path:

- `sims/firesim/sim/midas/src/main/cc/core/simulation.cc`
  - `execute_simulation_flow()` calls `wait_for_init()`.
  - `wait_for_init()` spins on `master.is_init_done()`.
  - With `+check-fingerprint`, the process immediately calls
    `master.check_fingerprint()` and exits before stream, DRAM, or workload
    initialization.
- `sims/firesim/sim/midas/src/main/cc/bridges/master.cc`
  - `is_init_done()` reads `INIT_DONE` and expects `1`.
  - `check_fingerprint()` reads `PRESENCE_READ` and expects `0x46697265`.
- `sims/firesim/sim/midas/src/main/scala/midas/widgets/Master.scala`
  - `INIT_DONE` is a read-only register that becomes `1`.
  - `PRESENCE_READ` is the FireSim fingerprint constant `0x46697265`.

Therefore a log line:

`FireSim fingerprint: 0x1`

means the host-side read used for `PRESENCE_READ` returned the value just proven
on `INIT_DONE`. That points to the AGFI/driver/AWS-SDK/OCL-MMIO path, not to the
Linux image, pipeline runtime, DMA completion, or `doneflag`.

## Evidence

Passing 1c1p run:

- Log:
  `sims/firesim/deploy/logs/2026-05-11--16-32-03-infrasetup-UUOQQ2QYKDSFWYSJ.log`
- AGFI:
  `agfi-098bce7d5e0c3d937`
- Runtime plusargs included:
  `+targetcycle-debug=1 +targetcycle-debug-limit=4 +targetcycle-debug-labels=1`
- Driver path:
  manager built `../sim/output/f2/<deploy-quintuplet>/driver-bundle.tar.gz`
- Preflight:
  `FireSim fingerprint: 0x46697265`

Failing 1c1p run:

- Log:
  `sims/firesim/deploy/logs/2026-05-12--10-22-23-infrasetup-ECVK2SBHCHXRZ65Z.log`
- Same AGFI:
  `agfi-098bce7d5e0c3d937`
- Same target-cycle debug plusargs:
  `+targetcycle-debug=1 +targetcycle-debug-limit=4 +targetcycle-debug-labels=1`
- The HWDB used an external `driver_tar` and skipped the normal driver build.
- The run also used a temporary local AWS-FPGA SDK copy path:
  only `sdk/`, `shared/`, and `sdk_setup.sh` were copied, producing
  a missing `/home/ubuntu/aws-fpga/release_version.txt` warning.
- Preflight failed three times with:
  `FireSim fingerprint: 0x1`

Important negative evidence:

- `targetcycle-debug-limit=4` and `targetcycle-debug-labels=1` are not the root
  cause. The same AGFI passed preflight with those plusargs on 2026-05-11.
- The external 2026-05-12 driver bundle contents are byte-identical to the
  manager output bundle currently present under `sim/output/f2/...`. That means
  the durable fix is not "this tarball binary is different"; the fix is to stop
  bypassing the AGFI-derived driver/SDK setup path during qualification.
- Several old timing-violated bitstreams have passed FireSim preflight and
  workloads. Timing risk remains real for any new F2 AGFI, but it is not a
  sufficient explanation for the `0x1` readback by itself.

## Adjacent Commits

Relevant FireSim submodule commits around the 1c1p AGFI:

- `91888035e` - `Add m8i build config for PCIS floorplan test`
- `df8b32613` - `Add 1C1P F2 baremetal smoke runtime`
- `f2beb072e` - `Point 1c1p hwdebug hwdb at PCIS SLR1 AGFI`

The AGFI description records FireSim commit `91888035e601638b356f98aa70793b4005cbf653`.
The current FireSim submodule HEAD is later than that, so the safe qualification
path is to let the AGFI metadata select the deploy quintuplet and rebuild the
driver from the current checked-out FireSim sources before each run.

## 12p/sbus64/NIC Reference Check

Known-good 12p/4c/sbus64/NIC AGFI:

- `agfi-077451484fe3b63c3`
- Driver bundle:
  `tmp/firesim-aws-f2/driver-bundles/agfi077451-dummy8x8-sbus64-cfg32-nic-notrace-20260507/driver-bundle.tar.gz`

2026-05-12 recheck:

- Preflight passed with `FireSim fingerprint: 0x46697265`.
- Linux booted, mounted `iceblk`, initialized IceNet, launched pipeline runtime,
  and exposed `gdbserver` at `172.16.0.2:2345`.
- Cross-GDB connected as the first TCP client and detached cleanly.
- This was not counted as an unattended workload PASS because the gdbserver
  profile is interactive and did not naturally reach `Simulation complete` in
  the observation window.

## Current Fix

1. Remove `driver_tar` from the 1c1p HWDB entry.
2. Keep explicit run-farm AMI support:
   `ami_id: ami-0d7cdfb6b3ce5b5e0`.
   The default FireSim AMI lookup is currently fragile for the subscribed F2
   Ubuntu 1.17 image, while the explicit AMI launches correctly.
3. Re-run `launchrunfarm -> infrasetup -> runworkload -> terminaterunfarm`
   through `scripts/firesim-tmux-run.sh`.

## 2026-05-12 Validation

Commands were launched through `scripts/firesim-tmux-run.sh` with:

- runtime:
  `sims/firesim/deploy/config_runtime_f2_gemmini_rerocc_pairmanager_dummy8x8_1c1p1_sbus64_nic_hwdebug_baremetal_cfg32_slot_smoke_quick_tcdlite.yaml`
- HWDB:
  `sims/firesim/deploy/config_hwdb_f2_gemmini_rerocc_pairmanager_dummy8x8_1c1p1_sbus64_nic_hwdebug.yaml`
- recipe:
  `sims/firesim/deploy/config_build_recipes_f2_gemmini_rerocc_pairmanager_dummy8x8_1c1p1_sbus64_nic_hwdebug.yaml`

Results:

- `launchrunfarm`:
  `sims/firesim/deploy/logs/2026-05-12--12-20-51-launchrunfarm-MHEDAZURSC1FSBJN.log`
  launched `i-0404269925a7fe9f3` at `192.168.1.216`.
- `infrasetup`:
  `sims/firesim/deploy/logs/2026-05-12--12-22-17-infrasetup-8M1AUL9Z5WCKXJGC.log`
  built the driver from the deploy quintuplet and passed preflight:
  `FireSim fingerprint: 0x46697265`.
- `runworkload`:
  `sims/firesim/deploy/logs/2026-05-12--12-25-50-runworkload-B01WNREB6TMB01AV.log`
  completed successfully.
- Result directory:
  `sims/firesim/deploy/results-workload/2026-05-12--12-25-50-rerocc-lc-baremetal-cfg32-slot-smoke-quick-f2-rerocc-baremetal-cfg32-slot-smoke-quick-1c1p1-nic-hwdebug-tcdlite/`
- `uartlog` authority:
  `FireSim fingerprint: 0x46697265`,
  `Simulation complete`,
  `*** PASSED *** after 44104832 cycles`.
- `terminaterunfarm`:
  `sims/firesim/deploy/logs/2026-05-12--12-27-22-terminaterunfarm-6UYNSVWPJ4YGBQCA.log`
  found no remaining F2 instances; AWS `describe-instances` also returned `[]`.

## Test Plan

1. Sanity baseline:
   launch the 1c1p F2 farm with the tcdlite baremetal cfg32-slot smoke runtime.
2. Infrasetup authority:
   require `Building FPGA software driver for <AGFI deploy quintuplet>` in the
   manager log and require preflight `FireSim fingerprint: 0x46697265`.
3. Workload authority:
   require the `uartlog` completion marker and FireSim `*** PASSED ***`; do not
   use `doneflag` as completion evidence.
4. Cleanup:
   run `terminaterunfarm --forceterminate` and verify no `f2.*` instance remains.
5. If preflight still returns `0x1` with the manager-built driver path, split the
   next test between:
   - the same AGFI on a fresh F2 host;
   - a rebuilt AGFI from commit `91888035e`;
   - the original FireSim AWS-FPGA SDK clone path versus any local SDK copy path.
