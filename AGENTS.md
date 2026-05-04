# Repository Guidelines

## Project Structure & Module Organization
`generators/` holds Chisel/Scala sources; top-level integration is under `generators/chipyard/src/main/scala`. `sims/firesim/` is the default simulation workflow. `tests/` builds bare-metal RISC-V binaries. `software/`, `fpga/`, `vlsi/`, `tools/`, `docs/`, and `scripts/` contain workloads and helpers.

## Build, Test, and Development Commands
- `./build-setup.sh` and `source env.sh`: bootstrap the repo environment.
- `cd sims/firesim && source sourceme-manager.sh --skip-ssh-setup`: enter the FireSim manager env.
- `./scripts/firesim-tmux-run.sh buildbitstream ...` or `./scripts/firesim-tmux-run.sh runworkload ...`: launch long FireSim jobs in detached `tmux`.
- `firesim launchrunfarm -a sims/firesim-staging/sample_config_hwdb.yaml -r sims/firesim-staging/sample_config_build_recipes.yaml`: launch a run farm.
- `firesim infrasetup -a ... -r ...`: build drivers and deploy infra.
- `firesim runworkload -a ... -r ...`: run workloads and collect results in `sims/firesim/deploy/results-workload/`.
- `firesim terminaterunfarm --forceterminate -a ... -r ...`: tear down run farms.
- `cmake -S tests -B tests/build -D CMAKE_BUILD_TYPE=Debug` and `cmake --build tests/build --target hello`: build a bare-metal payload for FireSim workloads.

## FireSim Workflow
Use the FireSim manager sequence from `sims/firesim/docs`: `launchrunfarm -> infrasetup -> runworkload -> terminaterunfarm`. Future FPGA tests must use this path. Future Verilator tests must also run through FireSim metasimulation with `metasimulation_host_simulator: verilator` or `verilator-debug`; do not add standalone regressions under `sims/verilator`.
Run `buildbitstream`, `infrasetup`, and `runworkload` through `scripts/firesim-tmux-run.sh` so the manager survives disconnects and leaves logs in `tmp/firesim-aws-f2/tmux/`. Re-run `firesim infrasetup` after any workload, rootfs, binary, or AGFI change, and after any interrupted run. If an F2 AGFI looks wrong, validate it against the latest `sims/firesim-staging/sample_config_hwdb.yaml`. Do not trust manager exit codes alone: confirm `uartlog` completion markers, then terminate unused EC2 farms.
When using FireSim manager commands, first `cd sims/firesim` and then `source sourceme-manager.sh --skip-ssh-setup`; `sourceme-manager.sh` derives paths from `pwd`, so sourcing it from the repo root misconfigures the manager environment.
For live run-farm inspection, prefer this sequence:
- inspect the latest manager logs in `sims/firesim/deploy/logs/` and tmux panes in `tmp/firesim-aws-f2/tmux/`
- verify live workload output under `sims/firesim/deploy/results-workload/`
- check EC2 state with `aws ec2 describe-instances --filters Name=tag:fsimcluster,Values=<cluster-tag> ...`
- if manager has not copied back `uartlog` yet, SSH to the run host with `/home/ubuntu/firesim.pem` using the instance's private IP from `aws ec2 describe-instances`; do not use the public IP. Inspect `/home/ubuntu/sim_slot_0/uartlog` plus `/home/ubuntu/sim_slot_0/heartbeat.csv`
For stopping FPGA runs, prefer this sequence:
- run `firesim terminaterunfarm --forceterminate -c <runtime-config> -a <hwdb> -r <build-recipes>` from `sims/firesim/deploy`
- verify the target EC2 instance actually enters `shutting-down` or `terminated`
- if `terminaterunfarm` reports success but leaves the instance running, explicitly reclaim it with `aws ec2 terminate-instances --instance-ids <id>`
- after manual termination, re-check `aws ec2 describe-instances` until the instance is no longer `running`
Every F2 run-farm instance must be closed promptly after the required live inspection and artifact copy-back are complete. Do not leave F2 instances running in the background; before switching tasks, ending a debugging round, or starting a new run, explicitly check for `f2.*` instances and terminate stale FireSim run farms.

## Coding Style & Naming Conventions
Match the touched subtree. Scala/Chisel generally uses 2-space indentation, `PascalCase` for classes/config fragments, and `camelCase` for methods and values. Types often end in `Params` or `Config`. In `tests/`, keep source and output names aligned, for example `hello.c` -> `hello.riscv`.

## Testing Guidelines
Prefer FireSim-managed regressions over backend-specific one-offs. For FPGA runs, keep evidence in `deploy/results-workload/`. For Verilator runs, use FireSim metasimulation.
After every key test milestone, commit the tested state with git before starting the next debugging step. The commit message must be detailed enough to reconstruct what was tested, including the AGFI/AFI or simulator target, runtime/build configs, workload, commands or scripts used, pass/fail result, artifact locations, and any known limitations. Do not bundle unrelated dirty work into these checkpoint commits.

## Branch & Pull Request Guidelines
Use `npu/dev` as the main development branch and PR base unless told otherwise. Branch creation, switching, and commits are handled outside this guide. When preparing a PR, follow the repository template and include the required changelog label, related issues, and relevant docs/tests.

## Generated Files
Do not commit transient outputs such as `target/`, `.bloop/`, `.sbt/`, `.conda-env/`, `.conda-lock-env/`, `tests/build/`, or `sims/firesim/deploy/results-workload/` unless the change intentionally updates generated collateral.
