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
state_dir="${cy_dir}/tmp/firemarshal-tmux"

if [[ ! -x "${marshal_dir}/marshal" ]]; then
  echo "Unable to locate executable firemarshal at ${marshal_dir}/marshal" >&2
  exit 1
fi

mkdir -p "${state_dir}"

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
forward_env_names=(
  ENABLE_PIPELINE_RUNTIME
  HOST_INIT_CHECK_ONLY
  PIPELINE_RUNTIME_GEMMINI_PHASE
  PIPELINE_RUNTIME_ONLY_MARKER
  PIPELINE_RUNTIME_PROGRESS
  PIPELINE_RUNTIME_PROGRESS_RAW
  SKIP_BUILD
  TARGET_KEY
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
source env.sh
set -u
$(printf '%s\n' "${forwarded_env_exports[@]}")
cd $(printf '%q' "${marshal_dir}")

echo "[marshal-tmux] started at \$(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "[marshal-tmux] working directory: \$PWD"
echo "[marshal-tmux] command: ./marshal${quoted_args}"

set +e
./marshal${quoted_args}
rc=\$?
set -e

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
EOF

tmux new-session -d -s "${session_name}" "${command_file}"

echo "Started tmux session: ${session_name}"
echo "Pane log: ${pane_log}"
echo "Metadata: ${metadata_file}"

if [[ "${attach_now}" == true ]]; then
  exec tmux attach -t "${session_name}"
fi
