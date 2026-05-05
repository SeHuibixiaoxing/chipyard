#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat <<'EOF'
Usage: scripts/firemarshal-tmux-run.sh [--session-name NAME] [--attach] <marshal args...>

Launch a FireMarshal command inside a detached tmux session so the task
survives terminal disconnects. The wrapper sources `env.sh`, runs from
`software/firemarshal`, and records logs under `tmp/firemarshal-tmux/`.

Example:
  scripts/firemarshal-tmux-run.sh --session-name marshal-explicit build \
    generators/gemmini/software/gemmini-rocc-tests/rerocc-baremetal-tests-coupleddma/workload/foo.json
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

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cy_dir="$(cd "${script_dir}/.." && pwd)"
marshal_dir="${cy_dir}/software/firemarshal"
firesim_dir="${cy_dir}/sims/firesim"
state_dir="${cy_dir}/tmp/firemarshal-tmux"

resolve_from_cy_dir() {
  local path="$1"
  if [[ "${path}" = /* ]]; then
    printf '%s\n' "${path}"
  else
    printf '%s\n' "${cy_dir}/${path}"
  fi
}

if [[ ! -x "${marshal_dir}/marshal" ]]; then
  echo "Unable to locate executable firemarshal at ${marshal_dir}/marshal" >&2
  exit 1
fi

mkdir -p "${state_dir}"

normalized_args=()
for arg in "$@"; do
  if [[ "${arg}" != -* && "${arg}" == *.json ]]; then
    normalized_args+=("$(resolve_from_cy_dir "${arg}")")
  else
    normalized_args+=("${arg}")
  fi
done
set -- "${normalized_args[@]}"

task_name="$1"
timestamp="$(date -u +%Y-%m-%d--%H-%M-%S)"
if [[ -z "${session_name}" ]]; then
  sanitized_task="$(printf '%s' "${task_name}" | tr -cs 'A-Za-z0-9_.-' '-')"
  session_name="marshal-${sanitized_task}-${timestamp}"
fi

if tmux has-session -t "${session_name}" 2>/dev/null; then
  echo "tmux session already exists: ${session_name}" >&2
  exit 1
fi

command_file="${state_dir}/${session_name}.command.sh"
metadata_file="${state_dir}/${session_name}.metadata"
pane_log="${state_dir}/${session_name}.pane.log"
exit_code_file="${state_dir}/${session_name}.exitcode"
quoted_args="$(printf ' %q' "$@")"
post_check_script=""
post_check_workload=""
if [[ ("${task_name}" == "build" || "${task_name}" == "install") && $# -ge 2 ]]; then
  workload_arg="$(resolve_from_cy_dir "$2")"
  case "$(basename "${workload_arg}")" in
    rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-batch8-fileonly-sync-pairdummy.json)
      post_check_script="${cy_dir}/generators/gemmini/software/gemmini-rocc-tests/pipeline-runtime/scripts/verify_pairdummy_firemarshal_image_freshness.sh"
      post_check_workload="${workload_arg}"
      ;;
  esac
fi

forward_env_names=(
  CAPTURE_AUTO_POWEROFF
  CAPTURE_PERIODIC_SYNC_ENABLE
  CAPTURE_PERIODIC_SYNC_SECONDS
  CAPTURE_PROGRESS_PING_ENABLE
  CAPTURE_PROGRESS_PING_SECONDS
  DEEP_LOG_ENABLE
  DEEP_LOG_GLOBAL_STAGE
  DEEP_LOG_LOCAL_STAGE
  DEEP_LOG_SEGMENT
  DEEP_LOG_STAGE_RADIUS
  DEEP_LOG_SUBBATCH
  DEEP_LOG_SUBBATCH_RADIUS
  DMA_BASE_ID
  DUMMY_GEMMINI_MODE
  ENABLE_PIPELINE_RUNTIME
  EXPORT_DMA_TIMEOUT_MS
  GEMMINI_BASE_ID
  HOST_INIT_CHECK_ONLY
  HUGETLB_MOUNT
  HUGETLB_PAGES
  METHODS
  NUM_CORES
  NUM_DMA
  NUM_GEMMINI
  PAGES_PER_ACC
  PAIR_MANAGER_MODE
  GOLDEN_CHECK_ENABLE
  PIPELINE_RUNTIME_AUDIT_LOG_ENABLE
  PIPELINE_RUNTIME_CHECKPOINT_LOG_ENABLE
  PIPELINE_RUNTIME_CHECKPOINT_LOG_PATH
  PIPELINE_RUNTIME_CRITICAL_UART_PAD_BURST
  PIPELINE_RUNTIME_CRITICAL_UART_PROBE
  PIPELINE_RUNTIME_PROFILE_ID
  PIPELINE_RUNTIME_DMA_EXPORT_CHUNK_LOG_STRIDE
  PIPELINE_RUNTIME_DMA_FORCE_DIRECT_ENABLE
  PIPELINE_RUNTIME_DMA_EXPORT_PROBE_ENABLE
  PIPELINE_RUNTIME_DMA_EXPORT_PROBE_TOKEN_END
  PIPELINE_RUNTIME_DMA_EXPORT_PROBE_TOKEN_START
  PIPELINE_RUNTIME_DMA_EXPORT_PAGE_END
  PIPELINE_RUNTIME_DMA_EXPORT_PAGE_START
  PIPELINE_RUNTIME_DMA_FIXED_LOAD_CHECKPOINT_ENABLE
  PIPELINE_RUNTIME_DMA_FIXED_LOAD_PROBE_ENABLE
  PIPELINE_RUNTIME_DMA_FIXED_LOAD_MONITOR_PROBE_ENABLE
  PIPELINE_RUNTIME_DMA_FIXED_LOAD_PRE_SRC_NOPS
  PIPELINE_RUNTIME_DMA_FIXED_LOAD_PROBE_STAGE_ID
  PIPELINE_RUNTIME_DMA_FIXED_LOAD_PROBE_TENSOR_ID
  PIPELINE_RUNTIME_DMA_FIXED_LOAD_PROBE_TOKEN_END
  PIPELINE_RUNTIME_DMA_FIXED_LOAD_PROBE_TOKEN_START
  PIPELINE_RUNTIME_DMA_FIXED_LOAD_PAGE_END
  PIPELINE_RUNTIME_DMA_FIXED_LOAD_PAGE_START
  PIPELINE_RUNTIME_DMA_SUBMIT_TRACE_ENABLE
  PIPELINE_RUNTIME_BREADCRUMB_ENABLE
  PIPELINE_RUNTIME_BREADCRUMB_PATH
  PIPELINE_RUNTIME_BREADCRUMB_SEGMENT
  PIPELINE_RUNTIME_BREADCRUMB_GLOBAL_STAGE
  PIPELINE_RUNTIME_BREADCRUMB_LOCAL_STAGE
  PIPELINE_RUNTIME_BREADCRUMB_SUBBATCH
  PIPELINE_RUNTIME_BREADCRUMB_STAGE_RADIUS
  PIPELINE_RUNTIME_BREADCRUMB_SUBBATCH_RADIUS
  PIPELINE_RUNTIME_DEBUG_TRIGGER_ENABLE
  PIPELINE_RUNTIME_DEBUG_TRIGGER_KIND
  PIPELINE_RUNTIME_DEBUG_TRIGGER_SEGMENT
  PIPELINE_RUNTIME_DEBUG_TRIGGER_GLOBAL_STAGE
  PIPELINE_RUNTIME_DEBUG_TRIGGER_LOCAL_STAGE
  PIPELINE_RUNTIME_DEBUG_TRIGGER_SUBBATCH
  PIPELINE_RUNTIME_DEBUG_TRIGGER_MANAGER
  PIPELINE_RUNTIME_DEBUG_TRIGGER_TENSOR_ID
  PIPELINE_RUNTIME_DEBUG_TRIGGER_PAGE
  PIPELINE_RUNTIME_DEBUG_TRIGGER_TOKEN
  PIPELINE_RUNTIME_DEBUG_TRIGGER_PRE_RING
  PIPELINE_RUNTIME_DEBUG_TRIGGER_POST_BUDGET
  PIPELINE_RUNTIME_DEBUG_TRIGGER_MATCH_ONCE
  PIPELINE_RUNTIME_DEBUG_TRIGGER_LOG_PATH
  PIPELINE_RUNTIME_LOCAL_GDB_ENABLE
  PIPELINE_RUNTIME_LOCAL_GDB_TOOL
  PIPELINE_RUNTIME_LOCAL_GDB_BIN
  PIPELINE_RUNTIME_LOCAL_GDB_INFO_PATH
  PIPELINE_RUNTIME_LOCAL_GDB_LOG_PATH
  PIPELINE_RUNTIME_LOCAL_GDB_CMDS
  PIPELINE_RUNTIME_LOCAL_GDB_TIMEOUT_SECS
  PIPELINE_RUNTIME_LOCAL_GDB_STATUS_INTERVAL_SECS
  PIPELINE_RUNTIME_LOCAL_GDB_CONTINUE_AFTER_MAIN
  PIPELINE_RUNTIME_LOCAL_GDB_EXTRA_BREAKPOINTS
  PIPELINE_RUNTIME_LOCAL_GDB_TRACE_BREAKPOINTS
  PIPELINE_RUNTIME_LOCAL_GDB_TRACE_IGNORE_COUNTS
  PIPELINE_RUNTIME_LOCAL_GDB_TRACE_CONDITIONS
  PIPELINE_RUNTIME_LOCAL_GDB_TRACE_STATE
  PIPELINE_RUNTIME_LOCAL_GDB_STREAM_CONSOLE
  PIPELINE_RUNTIME_LOCAL_GDB_METHOD
  PIPELINE_RUNTIME_DEBUG_FILTER_SEGMENT
  PIPELINE_RUNTIME_DEBUG_FILTER_GLOBAL_STAGE
  PIPELINE_RUNTIME_DEBUG_FILTER_LOCAL_STAGE
  PIPELINE_RUNTIME_DEBUG_FILTER_SUBBATCH
  PIPELINE_RUNTIME_DEBUG_FILTER_PHASE
  PIPELINE_RUNTIME_DEBUG_FILTER_WAIT_PHASE
  PIPELINE_RUNTIME_DEBUG_FILTER_TENSOR
  PIPELINE_RUNTIME_DEBUG_FILTER_MANAGER
  PIPELINE_RUNTIME_DEBUG_FILTER_OPCODE
  PIPELINE_RUNTIME_DEBUG_FILTER_RR_STAGE
  PIPELINE_RUNTIME_DEBUG_FILTER_RR_MANAGER
  PIPELINE_RUNTIME_DEBUG_FILTER_RR_OPCODE
  PIPELINE_RUNTIME_DEBUG_FILTER_RR_CFG
  PIPELINE_RUNTIME_GUEST_DEEP_LOG_ENABLE
  PIPELINE_RUNTIME_GUEST_LOG_ENABLE
  PIPELINE_RUNTIME_SKIP_GOLDEN_CHECK
  PIPELINE_RUNTIME_GEMMINI_PHASE
  PIPELINE_RUNTIME_LOG_PROFILE
  PIPELINE_RUNTIME_ONLY_MARKER
  PIPELINE_RUNTIME_SKIP_INPUT_LOAD
  PIPELINE_RUNTIME_SKIP_MODEL_BIN_LOAD
  PIPELINE_RUNTIME_PROGRESS
  PIPELINE_RUNTIME_PROGRESS_HOT
  PIPELINE_RUNTIME_PROGRESS_PAD_BURST
  PIPELINE_RUNTIME_PROGRESS_RAW
  PIPELINE_RUNTIME_FIRESIM_TRACERV_WORKER_MARKERS
  PIPELINE_RUNTIME_FIRESIM_TRACERV_DMA_WINDOW_MARKERS
  PIPELINE_RUNTIME_DMA_TRACERV_PAGE_START
  PIPELINE_RUNTIME_DMA_TRACERV_PAGE_END
  PIPELINE_RUNTIME_MLOCKALL_MODE
  PIPELINE_RUNTIME_DISABLE_MAPPING_CACHE
  PIPELINE_RUNTIME_RUNNER_STAGE_SYNC_ENABLE
  PIPELINE_RUNTIME_MAPPING_CACHE_PROBE_START
  PIPELINE_RUNTIME_MAPPING_CACHE_PROBE_END
  PIPELINE_RUNTIME_MAPPING_PARSE_PROGRESS_INTERVAL
  PIPELINE_RUNTIME_MAPPING_PARSE_PROBE_START
  PIPELINE_RUNTIME_MAPPING_PARSE_PROBE_END
  PIPELINE_RUNTIME_STDIO_CAPTURE_MODE
  PIPELINE_RUNTIME_UART_LOG_ENABLE
  REROCC_RUNTIME_STYLE_ONLY
  REROCC_STALL_DIAG_ONLY
  SKIP_BUILD
  TARGET_BATCH
  TARGET_KEY
  TRACE_DIR
  TRACE_ENABLE
  WATCHDOG_MS
)
forwarded_env_exports=()

for env_name in "${forward_env_names[@]}"; do
  if [[ -n "${!env_name+x}" ]]; then
    forwarded_env_exports+=("export ${env_name}=$(printf '%q' "${!env_name}")")
  fi
done

rm -f "${command_file}" "${metadata_file}" "${pane_log}" "${exit_code_file}"

cat > "${command_file}" <<EOF
#!/usr/bin/env bash
set -euo pipefail

exec > >(tee -a $(printf '%q' "${pane_log}")) 2>&1

cd $(printf '%q' "${cy_dir}")
set +u
cd $(printf '%q' "${firesim_dir}")
source sourceme-manager.sh --skip-ssh-setup
cd $(printf '%q' "${cy_dir}")
source env.sh
set -u
$(printf '%s\n' "${forwarded_env_exports[@]}")
cd $(printf '%q' "${marshal_dir}")
post_check_script=$(printf '%q' "${post_check_script}")
post_check_workload=$(printf '%q' "${post_check_workload}")

echo "[marshal-tmux] started at \$(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "[marshal-tmux] working directory: \$PWD"
echo "[marshal-tmux] command: ./marshal${quoted_args}"

set +e
./marshal${quoted_args}
rc=\$?
set -e

if [[ "\${rc}" -eq 0 && -n "\${post_check_script}" ]]; then
  echo "[marshal-tmux] running image freshness check: \${post_check_script} \${post_check_workload}"
  set +e
  "\${post_check_script}" "\${post_check_workload}"
  verify_rc=\$?
  set -e
  if [[ "\${verify_rc}" -ne 0 ]]; then
    rc="\${verify_rc}"
    echo "[marshal-tmux] image freshness check failed rc=\${verify_rc}"
  fi
fi

printf '%s\n' "\${rc}" > $(printf '%q' "${exit_code_file}")
echo "[marshal-tmux] finished at \$(date -u +%Y-%m-%dT%H:%M:%SZ) rc=\${rc}"
exit "\${rc}"
EOF

chmod +x "${command_file}"

cat > "${metadata_file}" <<EOF
session_name=${session_name}
command_file=${command_file}
pane_log=${pane_log}
exit_code_file=${exit_code_file}
marshal_dir=${marshal_dir}
command=./marshal${quoted_args}
post_check_script=${post_check_script}
post_check_workload=${post_check_workload}
EOF

tmux new-session -d -s "${session_name}" "${command_file}"

echo "Started tmux session: ${session_name}"
echo "Pane log: ${pane_log}"
echo "Metadata: ${metadata_file}"

if [[ "${attach_now}" == true ]]; then
  exec tmux attach -t "${session_name}"
fi
