# 2026-04-16 Repo Migration Notes

## Purpose
- Prepare the current `/home/ubuntu/chipyard` workspace for migration to another machine.
- Remove clearly re-generable local caches and build artifacts.
- Preserve runtime debug assets, workflow scripts, and current dirty workspace state needed to continue work on the destination machine.

## Ignore Policy Adjustments
- Root [`.gitignore`](/home/ubuntu/chipyard/.gitignore) now explicitly unignores [AGENTS.md](/home/ubuntu/chipyard/AGENTS.md).
- `tmp/` remains ignored as a hard constraint.
- Policy used for this migration:
  - ignored files that are only useful for machine-to-machine copying do not need to be unignored
  - only files that should become real repository content need ignore-rule changes

## What Was Removed
- Root-local caches and generated directories:
  - `.classpath_cache/`
  - `.claude/`
  - `.conda-env/`
  - `.conda-lock-env/`
  - `.ivy2/`
  - `.metals/`
  - `.sbt/`
  - `.bloop/`
  - `node_modules/`
- Root/local generated artifacts:
  - `build-libfesvr.a.log`
  - `build-setup.log`
  - `init-submodules-no-riscv-tools.log`
  - `package.json`
  - `package-lock.json`
  - `tests/build/`
  - `tests/hello.riscv`
  - `target/`
  - multiple `target/` directories under `generators/`, `project/`, and `tools/`
  - `conference/HybridMapper/output/`
  - `conference/HybridMapper/HybridMapper/test/test_Model_output/`
  - `conference/HybridMapper/HybridMapper/test/test_HybridMapper_output/`
  - `conference/HybridMapper/**/__pycache__/`
  - `sims/firesim-staging/generated-src/`
  - root `tags`
  - `vivado*.log`
  - `vivado*.jou`
- FireSim local cache under [sims/firesim](/home/ubuntu/chipyard/sims/firesim):
  - `.ivy2/`
  - `.sbt/`
  - `tags`
  - `build-setup-log`
  - `tmp/`
- `gemmini-rocc-tests` ignored build outputs via `git clean -fdX` in
  [`generators/gemmini/software/gemmini-rocc-tests`](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests)
  including object files, generated binaries, overlay build outputs, and autotools byproducts.
- `tmp/` cleanup:
  - accidental nested git metadata: `tmp/.git`, `tmp/.gitmodules`
  - temporary probe/build artifacts:
    - `tmp/prt_yaml_loader_smoketest*`
    - `tmp/tsi_load_probe.S`
    - `tmp/tsi_probe_*`
    - `tmp/tsi_probe_ld_0x100.elf`
    - `tmp/rerocc_lc_coverage_linux_coupleddma.i`
  - empty/stale lock/state files under [tmp/firesim-aws-f2](/home/ubuntu/chipyard/tmp/firesim-aws-f2)

## What Was Intentionally Preserved
- Current source changes and debug documents:
  - [`scripts/firesim-tmux-run.sh`](/home/ubuntu/chipyard/scripts/firesim-tmux-run.sh)
  - [`generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/debug_records`](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/debug_records)
  - [`generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/change_records`](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/change_records)
  - [`generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/docs`](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/docs)
  - [`generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/scripts`](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/scripts)
- FireSim/FireMarshal assets that are expensive to recreate or useful for debugging continuation:
  - [`sims/firesim/deploy`](/home/ubuntu/chipyard/sims/firesim/deploy)
  - [`sims/firesim/sim`](/home/ubuntu/chipyard/sims/firesim/sim)
  - [`software/firemarshal/images`](/home/ubuntu/chipyard/software/firemarshal/images)
  - [`software/firemarshal/boards`](/home/ubuntu/chipyard/software/firemarshal/boards)
  - [`software/firemarshal/logs`](/home/ubuntu/chipyard/software/firemarshal/logs)
- `tmp/` diagnostic assets that remain worth copying:
  - [`tmp/firesim-aws-f2`](/home/ubuntu/chipyard/tmp/firesim-aws-f2)
  - [`tmp/firemarshal-tmux`](/home/ubuntu/chipyard/tmp/firemarshal-tmux)
  - [`tmp/prt_live`](/home/ubuntu/chipyard/tmp/prt_live)
  - [`tmp/pipeline-runtime-effective-guest-env`](/home/ubuntu/chipyard/tmp/pipeline-runtime-effective-guest-env)
  - other small analysis/reference directories under [`tmp`](/home/ubuntu/chipyard/tmp)

## Why Some Large Directories Were Not Deleted
- [`sims/firesim/sim`](/home/ubuntu/chipyard/sims/firesim/sim) and [`sims/firesim/deploy`](/home/ubuntu/chipyard/sims/firesim/deploy) are large, but they are useful when migrating an in-progress FireSim environment.
- [`software/firemarshal/images`](/home/ubuntu/chipyard/software/firemarshal/images) and [`software/firemarshal/boards`](/home/ubuntu/chipyard/software/firemarshal/boards) are also expensive to rebuild and useful to keep if the destination machine should continue from the current FireMarshal state.
- [`software/firemarshal/logs`](/home/ubuntu/chipyard/software/firemarshal/logs) remains ignored in git because it is high-churn generated output, but it should still be copied during machine migration if you want full build history.

## Current Important Dirty State
- Root repo:
  - modified: [`.gitignore`](/home/ubuntu/chipyard/.gitignore)
  - modified: [`scripts/firesim-tmux-run.sh`](/home/ubuntu/chipyard/scripts/firesim-tmux-run.sh)
  - untracked and now no longer ignored: [AGENTS.md](/home/ubuntu/chipyard/AGENTS.md)
- Dirty submodules:
  - [`generators/gemmini`](/home/ubuntu/chipyard/generators/gemmini)
  - [`software/firemarshal`](/home/ubuntu/chipyard/software/firemarshal)
- Active working set for the current pipeline-runtime work lives under
  [`generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime`](/home/ubuntu/chipyard/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime)

## Recommended Transfer Method
- `scp` cannot exclude `tmp/` by itself, so use a two-step transfer:
  1. archive the repo without `tmp/`
  2. copy `tmp/` separately

### Step 1: pack the repo without `tmp/`
```bash
cd /home/ubuntu
tar --exclude='chipyard/tmp' -czf chipyard-no-tmp-20260416.tgz chipyard
```

### Step 2: copy the main repo archive
```bash
scp /home/ubuntu/chipyard-no-tmp-20260416.tgz <user>@<dst-host>:/home/<user>/
```

### Step 3: copy `tmp/` separately
```bash
scp -r /home/ubuntu/chipyard/tmp <user>@<dst-host>:/home/<user>/chipyard/
```

### Optional: copy FireMarshal logs separately if you use a narrower main transfer
```bash
scp -r /home/ubuntu/chipyard/software/firemarshal/logs <user>@<dst-host>:/home/<user>/chipyard/software/firemarshal/
```

## Suggested Post-Copy Validation On The Destination Machine
```bash
cd /home/<user>/chipyard
git status --short
git -C generators/gemmini/software/gemmini-rocc-tests status --short
du -sh tmp/firesim-aws-f2 tmp/firemarshal-tmux software/firemarshal/logs
```

## Notes
- This cleanup intentionally favored migration continuity over maximal disk reduction.
- If you later decide the destination machine does not need preserved FireSim/FireMarshal build artifacts, the next safe cleanup targets are still
  [`sims/firesim/deploy`](/home/ubuntu/chipyard/sims/firesim/deploy),
  [`sims/firesim/sim`](/home/ubuntu/chipyard/sims/firesim/sim),
  [`software/firemarshal/images`](/home/ubuntu/chipyard/software/firemarshal/images),
  and [`software/firemarshal/boards`](/home/ubuntu/chipyard/software/firemarshal/boards).
