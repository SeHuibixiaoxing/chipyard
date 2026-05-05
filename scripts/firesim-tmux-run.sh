#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat <<'EOF'
Usage: scripts/firesim-tmux-run.sh [--session-name NAME] [--attach] <firesim-task> [firesim args...]

Launch a FireSim manager command inside a detached tmux session so the task
survives terminal disconnects. The wrapper sources `sourceme-manager.sh`,
runs from `sims/firesim/deploy`, and records metadata under
`tmp/firesim-aws-f2/tmux/`.

Options:
  --session-name NAME  Override the generated tmux session name
  --attach             Attach to the tmux session immediately after launch
  -h, --help           Show this help message

Example:
  scripts/firesim-tmux-run.sh buildbitstream -b config_build.yaml -r config_build_recipes.yaml

Environment:
  FIRESIM_RUNWORKLOAD_WATCHDOG_SECONDS   Override the default 10800s watchdog for
                                         `runworkload`
  FIRESIM_RUNWORKLOAD_WATCHDOG_DISABLE=1 Disable the `runworkload` watchdog
  FIRESIM_RUNWORKLOAD_LIVE_IDLE_TIMEOUT_SECONDS
                                         Override the extended host-monitor
                                         timeout used when heartbeat stays live
                                         but guest-visible files stop growing
  FIRESIM_RUNWORKLOAD_MONITOR_SCRIPT     Optional host-side monitor script to
                                         launch alongside `runworkload`
EOF
}

session_name=""
attach_now=false

while [[ $# -gt 0 ]]; do
  case "$1" in
    --session-name)
      shift
      session_name="${1:?missing session name}"
      ;;
    --attach)
      attach_now=true
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    --)
      shift
      break
      ;;
    -*)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
    *)
      break
      ;;
  esac
  shift
done

if [[ $# -lt 1 ]]; then
  usage >&2
  exit 1
fi

if ! command -v tmux >/dev/null 2>&1; then
  echo "tmux is required but not installed." >&2
  exit 1
fi

resolve_from_deploy() {
  local path="$1"
  if [[ "${path}" = /* ]]; then
    printf '%s\n' "${path}"
  else
    printf '%s\n' "${deploy_dir}/${path}"
  fi
}

get_flag_value() {
  local want="$1"
  shift
  local prev=""
  for arg in "$@"; do
    if [[ "${prev}" == "${want}" ]]; then
      printf '%s\n' "${arg}"
      return 0
    fi
    prev="${arg}"
  done
  return 1
}

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cy_dir="$(cd "${script_dir}/.." && pwd)"
if [[ ! -d "${cy_dir}/sims/firesim/deploy" ]]; then
  echo "Unable to locate chipyard root from ${script_dir}" >&2
  exit 1
fi
firesim_dir="${cy_dir}/sims/firesim"
deploy_dir="${firesim_dir}/deploy"
state_dir="${cy_dir}/tmp/firesim-aws-f2/tmux"

mkdir -p "${state_dir}"

task_name="$1"
watchdog_enabled=false
watchdog_timeout_seconds="${FIRESIM_RUNWORKLOAD_WATCHDOG_SECONDS:-10800}"
watchdog_runtime_config=""
watchdog_hwdb=""
watchdog_build_recipes=""
watchdog_log=""
watchdog_pid_file=""
watchdog_triggered_file=""
monitor_script=""
monitor_pid_file=""
monitor_log=""
propagated_env_names=()

append_env_name_if_set() {
  local env_name="$1"
  if [[ -n "${!env_name+x}" ]]; then
    propagated_env_names+=("${env_name}")
  fi
}

if [[ "${task_name}" == "runworkload" && "${FIRESIM_RUNWORKLOAD_WATCHDOG_DISABLE:-0}" != "1" ]]; then
  watchdog_enabled=true

  if runtime_config_arg="$(get_flag_value "-c" "$@")"; then
    watchdog_runtime_config="$(resolve_from_deploy "${runtime_config_arg}")"
  else
    echo "Warning: watchdog disabled because runworkload was started without -c" >&2
    watchdog_enabled=false
  fi

  if [[ "${watchdog_enabled}" == true ]]; then
    if hwdb_arg="$(get_flag_value "-a" "$@")"; then
      watchdog_hwdb="$(resolve_from_deploy "${hwdb_arg}")"
    else
      echo "Warning: watchdog disabled because runworkload was started without -a" >&2
      watchdog_enabled=false
    fi
  fi

  if [[ "${watchdog_enabled}" == true ]]; then
    if build_recipes_arg="$(get_flag_value "-r" "$@")"; then
      watchdog_build_recipes="$(resolve_from_deploy "${build_recipes_arg}")"
    else
      echo "Warning: watchdog disabled because runworkload was started without -r" >&2
      watchdog_enabled=false
    fi
  fi
fi

if [[ "${task_name}" == "runworkload" && -n "${FIRESIM_RUNWORKLOAD_MONITOR_SCRIPT:-}" ]]; then
  if [[ "${FIRESIM_RUNWORKLOAD_MONITOR_SCRIPT}" = /* ]]; then
    monitor_script="${FIRESIM_RUNWORKLOAD_MONITOR_SCRIPT}"
  else
    monitor_script="${cy_dir}/${FIRESIM_RUNWORKLOAD_MONITOR_SCRIPT}"
  fi

  if [[ ! -x "${monitor_script}" ]]; then
    echo "Warning: runworkload monitor script is not executable: ${monitor_script}" >&2
    monitor_script=""
  fi
fi

while IFS='=' read -r env_name _; do
  if [[ "${env_name}" =~ ^FIRESIM_[A-Za-z0-9_]+$ ]] || \
     [[ "${env_name}" =~ ^PIPELINE_RUNTIME_DEBUG_TRIGGER_[A-Za-z0-9_]+$ ]] || \
     [[ "${env_name}" =~ ^PIPELINE_RUNTIME_DEBUG_FILTER_[A-Za-z0-9_]+$ ]] || \
     [[ "${env_name}" =~ ^PIPELINE_RUNTIME_LOCAL_GDB_[A-Za-z0-9_]+$ ]] || \
     [[ "${env_name}" =~ ^PIPELINE_RUNTIME_DMA_[A-Za-z0-9_]+$ ]] || \
     [[ "${env_name}" =~ ^PIPELINE_RUNTIME_BREADCRUMB_[A-Za-z0-9_]+$ ]] || \
     [[ "${env_name}" =~ ^REROCC_[A-Za-z0-9_]+$ ]] || \
     [[ "${env_name}" =~ ^DMA_[A-Za-z0-9_]+$ ]] || \
     [[ "${env_name}" =~ ^COVERAGE_[A-Za-z0-9_]+$ ]] || \
     [[ "${env_name}" =~ ^NONBLOCKING_[A-Za-z0-9_]+$ ]] || \
     [[ "${env_name}" =~ ^NUM_[A-Za-z0-9_]+$ ]] || \
     [[ "${env_name}" =~ ^GEMMINI_[A-Za-z0-9_]+$ ]] || \
     [[ "${env_name}" =~ ^LOCAL_[A-Za-z0-9_]+$ ]] || \
     [[ "${env_name}" =~ ^PAIR_[A-Za-z0-9_]+$ ]] || \
     [[ "${env_name}" =~ ^LONG_[A-Za-z0-9_]+$ ]] || \
     [[ "${env_name}" =~ ^SHORT_[A-Za-z0-9_]+$ ]] || \
     [[ "${env_name}" == "BYTES" ]]; then
    propagated_env_names+=("${env_name}")
  fi
done < <(env | sort)

for env_name in JAVA_TOOL_OPTIONS JAVA_HEAP_SIZE MAKEFLAGS SBT_OPTS VERILATOR_MAKEFLAGS; do
  append_env_name_if_set "${env_name}"
done

timestamp="$(date -u +%Y-%m-%d--%H-%M-%S)"
if [[ -z "${session_name}" ]]; then
  sanitized_task="$(printf '%s' "${task_name}" | tr -cs 'A-Za-z0-9_.-' '-')"
  session_name="firesim-${sanitized_task}-${timestamp}"
fi

if tmux has-session -t "${session_name}" 2>/dev/null; then
  echo "tmux session already exists: ${session_name}" >&2
  exit 1
fi

command_file="${state_dir}/${session_name}.command.sh"
metadata_file="${state_dir}/${session_name}.metadata"
pane_log="${state_dir}/${session_name}.pane.log"
exit_code_file="${state_dir}/${session_name}.exitcode"
quoted_firesim_args="$(printf ' %q' "$@")"

if [[ "${watchdog_enabled}" == true ]]; then
  watchdog_log="${state_dir}/${session_name}.watchdog.log"
  watchdog_pid_file="${state_dir}/${session_name}.watchdog.pid"
  watchdog_triggered_file="${state_dir}/${session_name}.watchdog.triggered"
fi

if [[ -n "${monitor_script}" ]]; then
  monitor_log="${state_dir}/${session_name}.monitor.log"
  monitor_pid_file="${state_dir}/${session_name}.monitor.pid"
fi

# Reusing a fixed session name is convenient for long FireSim flows, but stale
# state files would make monitors read the previous run's status.
rm -f "${command_file}" "${metadata_file}" "${pane_log}" "${exit_code_file}" \
  "${watchdog_log}" "${watchdog_pid_file}" "${watchdog_triggered_file}" \
  "${monitor_log}" "${monitor_pid_file}"

cat > "${command_file}" <<EOF
#!/usr/bin/env bash
set -euo pipefail

exec > >(tee -a $(printf '%q' "${pane_log}")) 2>&1

watchdog_enabled=$(printf '%q' "${watchdog_enabled}")
watchdog_timeout_seconds=$(printf '%q' "${watchdog_timeout_seconds}")
watchdog_log=$(printf '%q' "${watchdog_log}")
watchdog_pid_file=$(printf '%q' "${watchdog_pid_file}")
watchdog_triggered_file=$(printf '%q' "${watchdog_triggered_file}")
watchdog_runtime_config=$(printf '%q' "${watchdog_runtime_config}")
watchdog_hwdb=$(printf '%q' "${watchdog_hwdb}")
watchdog_build_recipes=$(printf '%q' "${watchdog_build_recipes}")
watchdog_pid=""
monitor_script=$(printf '%q' "${monitor_script}")
monitor_log=$(printf '%q' "${monitor_log}")
monitor_pid_file=$(printf '%q' "${monitor_pid_file}")
monitor_pid=""

$(for env_name in "${propagated_env_names[@]}"; do
    printf 'export %s=%q\n' "${env_name}" "${!env_name}"
  done)

cleanup_watchdog() {
  if [[ -n "\${watchdog_pid}" ]]; then
    if [[ -f "\${watchdog_triggered_file}" ]]; then
      wait "\${watchdog_pid}" || true
    elif kill -0 "\${watchdog_pid}" 2>/dev/null; then
      kill "\${watchdog_pid}" 2>/dev/null || true
      wait "\${watchdog_pid}" || true
    fi
  fi
  rm -f "\${watchdog_pid_file}"

  if [[ -n "\${monitor_pid}" ]]; then
    if kill -0 "\${monitor_pid}" 2>/dev/null; then
      kill "\${monitor_pid}" 2>/dev/null || true
      wait "\${monitor_pid}" || true
    fi
  fi
  rm -f "\${monitor_pid_file}"
}

trap cleanup_watchdog EXIT

cd $(printf '%q' "${firesim_dir}")
set +u
source sourceme-manager.sh --skip-ssh-setup
set -u
cd deploy

echo "[firesim-tmux] started at \$(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "[firesim-tmux] working directory: \$PWD"
echo "[firesim-tmux] command: firesim${quoted_firesim_args}"
echo "[firesim-tmux] propagated FIRESIM env vars: $(printf '%s ' "${propagated_env_names[@]}")"

set +e
if [[ "\${watchdog_enabled}" == "true" ]]; then
  firesim${quoted_firesim_args} &
  firesim_pid=\$!
  rm -f "\${watchdog_triggered_file}"
  echo "[firesim-watchdog] armed timeout=\${watchdog_timeout_seconds}s log=\${watchdog_log}"

  (
    exec > >(tee -a "\${watchdog_log}") 2>&1
    sleep "\${watchdog_timeout_seconds}"
    if [[ -f $(printf '%q' "${exit_code_file}") ]]; then
      exit 0
    fi

    : > "\${watchdog_triggered_file}"
    echo "[firesim-watchdog] timeout reached at \$(date -u +%Y-%m-%dT%H:%M:%SZ)"

    if kill -0 "\${firesim_pid}" 2>/dev/null; then
      echo "[firesim-watchdog] sending SIGTERM to runworkload pid=\${firesim_pid}"
      kill -TERM "\${firesim_pid}" 2>/dev/null || true
      sleep 15
      if kill -0 "\${firesim_pid}" 2>/dev/null; then
        echo "[firesim-watchdog] sending SIGKILL to runworkload pid=\${firesim_pid}"
        kill -KILL "\${firesim_pid}" 2>/dev/null || true
      fi
    fi

    echo "[firesim-watchdog] calling terminaterunfarm --forceterminate"
    firesim terminaterunfarm --forceterminate \
      -c "\${watchdog_runtime_config}" \
      -a "\${watchdog_hwdb}" \
      -r "\${watchdog_build_recipes}"
    term_status=\$?
    echo "[firesim-watchdog] terminaterunfarm exit code \${term_status}"

    WATCHDOG_RUNTIME_CONFIG="\${watchdog_runtime_config}" python - <<'PY'
import subprocess
import sys
import yaml
import os

runtime_config = os.environ["WATCHDOG_RUNTIME_CONFIG"]
with open(runtime_config, "r", encoding="utf-8") as handle:
    cfg = yaml.safe_load(handle)

tag = (
    cfg.get("run_farm", {})
       .get("recipe_arg_overrides", {})
       .get("run_farm_tag")
)
if not tag:
    print("[firesim-watchdog] no run_farm_tag in runtime config; skipping aws fallback")
    sys.exit(0)

query = "Reservations[].Instances[].InstanceId"
describe_cmd = [
    "aws", "ec2", "describe-instances",
    "--filters",
    f"Name=tag:fsimcluster,Values={tag}",
    "Name=instance-state-name,Values=pending,running,stopping,stopped",
    "--query", query,
    "--output", "text",
]
proc = subprocess.run(describe_cmd, check=False, text=True, capture_output=True)
if proc.returncode != 0:
    print(f"[firesim-watchdog] aws describe-instances failed rc={proc.returncode}")
    if proc.stdout:
        print(proc.stdout.strip())
    if proc.stderr:
        print(proc.stderr.strip())
    sys.exit(0)

instance_ids = [tok for tok in proc.stdout.split() if tok]
if not instance_ids:
    print(f"[firesim-watchdog] no live instances remain for fsimcluster={tag}")
    sys.exit(0)

print(
    "[firesim-watchdog] forcing aws terminate-instances for "
    + " ".join(instance_ids)
)
subprocess.run(
    ["aws", "ec2", "terminate-instances", "--instance-ids", *instance_ids],
    check=False,
)
PY
  ) &
  watchdog_pid=\$!
  echo "\${watchdog_pid}" > "\${watchdog_pid_file}"

  if [[ -n "\${monitor_script}" ]]; then
    echo "[firesim-monitor] starting \${monitor_script}"
    (
      exec > >(tee -a "\${monitor_log}") 2>&1
      export FIRESIM_MONITOR_RUNTIME_CONFIG="\${watchdog_runtime_config}"
      export FIRESIM_MONITOR_HWDB="\${watchdog_hwdb}"
      export FIRESIM_MONITOR_BUILD_RECIPES="\${watchdog_build_recipes}"
      export FIRESIM_MONITOR_SESSION_NAME="${session_name}"
      export FIRESIM_MONITOR_EXIT_CODE_FILE=$(printf '%q' "${exit_code_file}")
      export FIRESIM_MONITOR_STATE_DIR=$(printf '%q' "${state_dir}")
      "\${monitor_script}"
    ) &
    monitor_pid=\$!
    echo "\${monitor_pid}" > "\${monitor_pid_file}"
  fi

  wait "\${firesim_pid}"
  status=\$?
else
  firesim${quoted_firesim_args}
  status=\$?
fi
set -e

echo "\${status}" > $(printf '%q' "${exit_code_file}")
echo "[firesim-tmux] finished at \$(date -u +%Y-%m-%dT%H:%M:%SZ) with exit code \${status}"
exit "\${status}"
EOF

chmod +x "${command_file}"

cat > "${metadata_file}" <<EOF
session_name=${session_name}
started_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)
task_name=${task_name}
command=firesim${quoted_firesim_args}
tmux_attach=tmux attach -t ${session_name}
tmux_capture=tmux capture-pane -pt ${session_name}
pane_log=${pane_log}
exit_code_file=${exit_code_file}
command_file=${command_file}
deploy_dir=${deploy_dir}
watchdog_enabled=${watchdog_enabled}
watchdog_timeout_seconds=${watchdog_timeout_seconds}
watchdog_log=${watchdog_log}
watchdog_pid_file=${watchdog_pid_file}
watchdog_triggered_file=${watchdog_triggered_file}
propagated_firesim_env=$(printf '%s ' "${propagated_env_names[@]}")
EOF

tmux new-session -d -s "${session_name}" "${command_file}"

echo "Started FireSim tmux session: ${session_name}"
echo "Attach: tmux attach -t ${session_name}"
echo "Pane log: ${pane_log}"
echo "Metadata: ${metadata_file}"
echo "Exit code file: ${exit_code_file}"

if [[ "${attach_now}" == true ]]; then
  exec tmux attach -t "${session_name}"
fi
