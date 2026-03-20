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
if [[ ! -d "${cy_dir}/sims/firesim/deploy" ]]; then
  echo "Unable to locate chipyard root from ${script_dir}" >&2
  exit 1
fi
firesim_dir="${cy_dir}/sims/firesim"
deploy_dir="${firesim_dir}/deploy"
state_dir="${cy_dir}/tmp/firesim-aws-f2/tmux"

mkdir -p "${state_dir}"

task_name="$1"
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

# Reusing a fixed session name is convenient for long FireSim flows, but stale
# state files would make monitors read the previous run's status.
rm -f "${command_file}" "${metadata_file}" "${pane_log}" "${exit_code_file}"

cat > "${command_file}" <<EOF
#!/usr/bin/env bash
set -euo pipefail

exec > >(tee -a $(printf '%q' "${pane_log}")) 2>&1

cd $(printf '%q' "${firesim_dir}")
set +u
source sourceme-manager.sh
set -u
cd deploy

echo "[firesim-tmux] started at \$(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "[firesim-tmux] working directory: \$PWD"
echo "[firesim-tmux] command: firesim${quoted_firesim_args}"

set +e
firesim${quoted_firesim_args}
status=\$?
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
