#!/usr/bin/env bash

set -euo pipefail

runtime_config="${FIRESIM_MONITOR_RUNTIME_CONFIG:?missing FIRESIM_MONITOR_RUNTIME_CONFIG}"
hwdb="${FIRESIM_MONITOR_HWDB:?missing FIRESIM_MONITOR_HWDB}"
build_recipes="${FIRESIM_MONITOR_BUILD_RECIPES:?missing FIRESIM_MONITOR_BUILD_RECIPES}"
session_name="${FIRESIM_MONITOR_SESSION_NAME:-firesim-runworkload}"
exit_code_file="${FIRESIM_MONITOR_EXIT_CODE_FILE:-}"
ssh_key="${FIRESIM_MONITOR_SSH_KEY:-/home/ubuntu/firesim.pem}"
capture_dir="${FIRESIM_MONITOR_CAPTURE_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/tmp/firesim-aws-f2/captures}"
state_dir="${FIRESIM_MONITOR_STATE_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/tmp/firesim-aws-f2/tmux}"
idle_timeout_seconds="${FIRESIM_RUNWORKLOAD_IDLE_TIMEOUT_SECONDS:-600}"
live_idle_timeout_seconds="${FIRESIM_RUNWORKLOAD_LIVE_IDLE_TIMEOUT_SECONDS:-10800}"
poll_seconds="${FIRESIM_RUNWORKLOAD_MONITOR_POLL_SECONDS:-30}"
ssh_connect_timeout_seconds="${FIRESIM_MONITOR_SSH_CONNECT_TIMEOUT_SECONDS:-10}"
probe_timeout_seconds="${FIRESIM_MONITOR_PROBE_TIMEOUT_SECONDS:-20}"
capture_timeout_seconds="${FIRESIM_MONITOR_CAPTURE_TIMEOUT_SECONDS:-180}"
heartbeat_liveness_enable="${FIRESIM_MONITOR_HEARTBEAT_LIVENESS_ENABLE:-1}"
arm_marker="${FIRESIM_MONITOR_ARM_MARKER:-[firemarshal] watchdog armed at wrapper launch}"
arm_on_guest_status_nonzero="${FIRESIM_MONITOR_ARM_ON_GUEST_STATUS_NONZERO:-1}"
complete_regex="${FIRESIM_MONITOR_COMPLETE_REGEX:-BERTMINI_PIPELINE_RUNTIME_PASS|BERTMINI_PIPELINE_RUNTIME_FAIL|\\[firemarshal\\] pipeline-runtime exited|\\[firemarshal\\] powering off guest}"
guest_log_path="${FIRESIM_MONITOR_GUEST_LOG_PATH:-/root/pipeline-runtime-debug/bertmini-batch8.deep.log}"
guest_sparse_log_path="${FIRESIM_MONITOR_GUEST_SPARSE_LOG_PATH:-/root/pipeline-runtime-debug/bertmini-batch8.log}"
guest_status_path="${FIRESIM_MONITOR_GUEST_STATUS_PATH:-/root/pipeline-runtime-debug/bertmini-batch8.status}"
guest_proc_stage_path="${FIRESIM_MONITOR_GUEST_PROC_STAGE_PATH:-/root/pipeline-runtime-debug/bertmini-batch8.wrapper-proc.stage}"
guest_runner_stage_path="${FIRESIM_MONITOR_GUEST_RUNNER_STAGE_PATH:-/root/pipeline-runtime-debug/bertmini-batch8.runner.stage}"
guest_runner_post_stage_path="${FIRESIM_MONITOR_GUEST_RUNNER_POST_STAGE_PATH:-/root/pipeline-runtime-debug/bertmini-batch8.wrapper.stage}"
guest_runner_early_stage_path="${FIRESIM_MONITOR_GUEST_RUNNER_EARLY_STAGE_PATH:-/root/pipeline-runtime-debug/bertmini-batch8.runner-early.stage}"
guest_binary_stage_path="${FIRESIM_MONITOR_GUEST_BINARY_STAGE_PATH:-}"
guest_runner_proc_stage_path="${FIRESIM_MONITOR_GUEST_RUNNER_PROC_STAGE_PATH:-/root/pipeline-runtime-debug/bertmini-batch8.runner-proc.stage}"
guest_checkpoint_log_path="${FIRESIM_MONITOR_GUEST_CHECKPOINT_LOG_PATH:-/root/pipeline-runtime-debug/bertmini-batch8.checkpoint.log}"
guest_trigger_log_path="${FIRESIM_MONITOR_GUEST_TRIGGER_LOG_PATH:-/root/pipeline-runtime-debug/bertmini-batch8.trigger.log}"
guest_breadcrumb_path="${FIRESIM_MONITOR_GUEST_BREADCRUMB_PATH:-/root/pipeline-runtime-debug/bertmini-batch8.breadcrumb.bin}"
remote_img_glob="${FIRESIM_MONITOR_REMOTE_IMG_GLOB:-/home/ubuntu/sim_slot_0/*rerocc-lc-linux-coupleddma-bertmini-pipeline-runtime-batch8*.img}"
trace_glob="${FIRESIM_MONITOR_TRACE_GLOB:-/home/ubuntu/sim_slot_0/TRACEFILE-C*}"
trace_tail_lines="${FIRESIM_MONITOR_TRACE_TAIL_LINES:-4000}"
arm_marker_b64="$(printf '%s' "${arm_marker}" | base64 -w0)"
complete_regex_b64="$(printf '%s' "${complete_regex}" | base64 -w0)"
guest_log_path_b64="$(printf '%s' "${guest_log_path}" | base64 -w0)"
guest_sparse_log_path_b64="$(printf '%s' "${guest_sparse_log_path}" | base64 -w0)"
guest_status_path_b64="$(printf '%s' "${guest_status_path}" | base64 -w0)"
guest_proc_stage_path_b64="$(printf '%s' "${guest_proc_stage_path}" | base64 -w0)"
guest_runner_stage_path_b64="$(printf '%s' "${guest_runner_stage_path}" | base64 -w0)"
guest_runner_post_stage_path_b64="$(printf '%s' "${guest_runner_post_stage_path}" | base64 -w0)"
guest_runner_early_stage_path_b64="$(printf '%s' "${guest_runner_early_stage_path}" | base64 -w0)"
guest_binary_stage_path_b64="$(printf '%s' "${guest_binary_stage_path}" | base64 -w0)"
guest_runner_proc_stage_path_b64="$(printf '%s' "${guest_runner_proc_stage_path}" | base64 -w0)"
guest_checkpoint_log_path_b64="$(printf '%s' "${guest_checkpoint_log_path}" | base64 -w0)"
guest_trigger_log_path_b64="$(printf '%s' "${guest_trigger_log_path}" | base64 -w0)"
guest_breadcrumb_path_b64="$(printf '%s' "${guest_breadcrumb_path}" | base64 -w0)"
remote_img_glob_b64="$(printf '%s' "${remote_img_glob}" | base64 -w0)"

mkdir -p "${capture_dir}" "${state_dir}"
monitor_log="${state_dir}/${session_name}.host-watchdog.log"
exec > >(tee -a "${monitor_log}") 2>&1
ssh_opts=(
  -o BatchMode=yes
  -o ConnectTimeout="${ssh_connect_timeout_seconds}"
  -o StrictHostKeyChecking=no
  -o UserKnownHostsFile=/dev/null
  -o GlobalKnownHostsFile=/dev/null
  -o LogLevel=ERROR
  -o ServerAliveInterval=5
  -o ServerAliveCountMax=3
  -i "${ssh_key}"
)

run_ssh_probe() {
  timeout --foreground "${probe_timeout_seconds}s" \
    ssh "${ssh_opts[@]}" "$@"
}

run_ssh_capture() {
  timeout --foreground "${capture_timeout_seconds}s" \
    ssh "${ssh_opts[@]}" "$@"
}

echo "[prt-host-watchdog] started at $(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "[prt-host-watchdog] runtime_config=${runtime_config}"
echo "[prt-host-watchdog] idle_timeout_seconds=${idle_timeout_seconds} live_idle_timeout_seconds=${live_idle_timeout_seconds} poll_seconds=${poll_seconds}"
echo "[prt-host-watchdog] ssh_connect_timeout_seconds=${ssh_connect_timeout_seconds} probe_timeout_seconds=${probe_timeout_seconds} capture_timeout_seconds=${capture_timeout_seconds}"
echo "[prt-host-watchdog] heartbeat_liveness_enable=${heartbeat_liveness_enable}"
echo "[prt-host-watchdog] arm_on_guest_status_nonzero=${arm_on_guest_status_nonzero}"

deploy_dir="$(cd "$(dirname "${runtime_config}")" && pwd)"
firesim_dir="$(cd "${deploy_dir}/.." && pwd)"

run_farm_tag="$(
  python - <<'PY' "${runtime_config}"
import sys
import yaml

with open(sys.argv[1], "r", encoding="utf-8") as handle:
    cfg = yaml.safe_load(handle)

tag = (
    cfg.get("run_farm", {})
       .get("recipe_arg_overrides", {})
       .get("run_farm_tag", "")
)
print(tag)
PY
)"

if [[ -z "${run_farm_tag}" ]]; then
  echo "[prt-host-watchdog] missing run_farm_tag in ${runtime_config}" >&2
  exit 2
fi

query_instance() {
  aws ec2 describe-instances \
    --filters \
      "Name=tag:fsimcluster,Values=${run_farm_tag}" \
      "Name=instance-state-name,Values=pending,running" \
    --query 'Reservations[].Instances[].{InstanceId:InstanceId,PrivateIp:PrivateIpAddress,State:State.Name}' \
    --output text
}

capture_remote_state() {
  local private_ip="$1"
  local stamp="$2"
  local prefix="${capture_dir}/${session_name}-${private_ip}-host-watchdog-${stamp}"
  local remote_img
  local firesim_pid
  local remote_traces
  local trace_path
  local trace_name

  remote_img="$(
    run_ssh_capture "ubuntu@${private_ip}" \
      "ls ${remote_img_glob} 2>/dev/null | head -n 1" \
      2>/dev/null || true
  )"
  firesim_pid="$(
    run_ssh_capture "ubuntu@${private_ip}" \
      'pgrep -o FireSim-f2 2>/dev/null || true' \
      2>/dev/null || true
  )"
  remote_traces="$(
    run_ssh_capture "ubuntu@${private_ip}" \
      "sudo sh -lc 'ls ${trace_glob} 2>/dev/null || true'" \
      2>/dev/null || true
  )"

  run_ssh_capture "ubuntu@${private_ip}" \
    'cat /home/ubuntu/sim_slot_0/uartlog 2>/dev/null || true' \
    > "${prefix}.uartlog.txt" || true
  run_ssh_capture "ubuntu@${private_ip}" \
    'cat /home/ubuntu/sim_slot_0/heartbeat.csv 2>/dev/null || true' \
    > "${prefix}.heartbeat.csv" || true
  if [[ -n "${firesim_pid}" ]]; then
    run_ssh_capture "ubuntu@${private_ip}" \
      "sudo tr '\0' '\n' </proc/${firesim_pid}/cmdline 2>/dev/null || true" \
      > "${prefix}.firesim-cmdline.txt" || true
    run_ssh_capture "ubuntu@${private_ip}" \
      "sudo find /proc/${firesim_pid}/fd -maxdepth 1 -type l -lname '/home/ubuntu/sim_slot_0/TRACEFILE-C*' -printf '%f -> %l\n' 2>/dev/null || true" \
      > "${prefix}.trace-fds.txt" || true
  fi
  run_ssh_capture "ubuntu@${private_ip}" \
    "sudo sh -lc 'ls -l ${trace_glob} 2>/dev/null || true; wc -c ${trace_glob} 2>/dev/null || true'" \
    > "${prefix}.trace-files.txt" || true
  while IFS= read -r trace_path; do
    [[ -n "${trace_path}" ]] || continue
    trace_name="$(basename "${trace_path}")"
    run_ssh_capture "ubuntu@${private_ip}" \
      "sudo tail -n ${trace_tail_lines} \"${trace_path}\" 2>/dev/null || sudo cat \"${trace_path}\" 2>/dev/null || true" \
      > "${prefix}.${trace_name}.tail.txt" || true
  done <<<"${remote_traces}"

  if [[ -n "${remote_img}" ]]; then
    run_ssh_capture "ubuntu@${private_ip}" \
      "sudo debugfs -R \"cat ${guest_status_path}\" \"${remote_img}\" 2>/dev/null || true" \
      > "${prefix}.status.txt" || true
    run_ssh_capture "ubuntu@${private_ip}" \
      "sudo debugfs -R \"cat ${guest_log_path}\" \"${remote_img}\" 2>/dev/null || true" \
      > "${prefix}.guest-deep-log.txt" || true
    run_ssh_capture "ubuntu@${private_ip}" \
      "sudo debugfs -R \"cat ${guest_sparse_log_path}\" \"${remote_img}\" 2>/dev/null || true" \
      > "${prefix}.guest-sparse-log.txt" || true
    if [[ -n "${guest_checkpoint_log_path}" ]]; then
      run_ssh_capture "ubuntu@${private_ip}" \
        "sudo debugfs -R \"cat ${guest_checkpoint_log_path}\" \"${remote_img}\" 2>/dev/null || true" \
        > "${prefix}.guest-checkpoint-log.txt" || true
    fi
    if [[ -n "${guest_trigger_log_path}" ]]; then
      run_ssh_capture "ubuntu@${private_ip}" \
        "sudo debugfs -R \"cat ${guest_trigger_log_path}\" \"${remote_img}\" 2>/dev/null || true" \
        > "${prefix}.guest-trigger-log.txt" || true
    fi
    if [[ -n "${guest_breadcrumb_path}" ]]; then
      run_ssh_capture "ubuntu@${private_ip}" \
        "sudo debugfs -R \"cat ${guest_breadcrumb_path}\" \"${remote_img}\" 2>/dev/null || true" \
        > "${prefix}.guest-breadcrumb.bin" || true
    fi
    if [[ -n "${guest_proc_stage_path}" ]]; then
      run_ssh_capture "ubuntu@${private_ip}" \
        "sudo debugfs -R \"cat ${guest_proc_stage_path}\" \"${remote_img}\" 2>/dev/null || true" \
        > "${prefix}.guest-proc-stage.txt" || true
    fi
    if [[ -n "${guest_runner_stage_path}" ]]; then
      run_ssh_capture "ubuntu@${private_ip}" \
        "sudo debugfs -R \"cat ${guest_runner_stage_path}\" \"${remote_img}\" 2>/dev/null || true" \
        > "${prefix}.guest-runner-stage.txt" || true
    fi
    if [[ -n "${guest_runner_post_stage_path}" ]]; then
      run_ssh_capture "ubuntu@${private_ip}" \
        "sudo debugfs -R \"cat ${guest_runner_post_stage_path}\" \"${remote_img}\" 2>/dev/null || true" \
        > "${prefix}.guest-runner-post-stage.txt" || true
    fi
    if [[ -n "${guest_runner_early_stage_path}" ]]; then
      run_ssh_capture "ubuntu@${private_ip}" \
        "sudo debugfs -R \"cat ${guest_runner_early_stage_path}\" \"${remote_img}\" 2>/dev/null || true" \
        > "${prefix}.guest-runner-early-stage.txt" || true
    fi
    if [[ -n "${guest_binary_stage_path}" ]]; then
      run_ssh_capture "ubuntu@${private_ip}" \
        "sudo debugfs -R \"cat ${guest_binary_stage_path}\" \"${remote_img}\" 2>/dev/null || true" \
        > "${prefix}.guest-binary-stage.txt" || true
    fi
    if [[ -n "${guest_runner_proc_stage_path}" ]]; then
      run_ssh_capture "ubuntu@${private_ip}" \
        "sudo debugfs -R \"cat ${guest_runner_proc_stage_path}\" \"${remote_img}\" 2>/dev/null || true" \
        > "${prefix}.guest-runner-proc-stage.txt" || true
    fi
  fi

  echo "[prt-host-watchdog] captures:"
  ls -1 "${prefix}".* 2>/dev/null || true
}

terminate_runfarm() {
  set +u
  cd "${firesim_dir}"
  source "${firesim_dir}/sourceme-manager.sh" --skip-ssh-setup
  set -u
  cd "${deploy_dir}"

  echo "[prt-host-watchdog] calling terminaterunfarm --forceterminate"
  firesim terminaterunfarm --forceterminate \
    -c "${runtime_config}" \
    -a "${hwdb}" \
    -r "${build_recipes}" || true
}

last_private_ip=""
armed=0
last_uart_size=0
last_guest_log_size=0
last_guest_sparse_log_size=0
last_guest_status_size=0
last_guest_proc_stage_size=0
last_guest_runner_stage_size=0
last_guest_runner_post_stage_size=0
last_guest_runner_early_stage_size=0
last_guest_binary_stage_size=0
last_guest_runner_proc_stage_size=0
last_guest_checkpoint_log_size=0
last_guest_trigger_log_size=0
last_hb_line=""
last_heartbeat_epoch=0
last_progress_epoch=0

while true; do
  if [[ -n "${exit_code_file}" && -f "${exit_code_file}" ]]; then
    echo "[prt-host-watchdog] exit code file present; stopping monitor"
    exit 0
  fi

  instance_info="$(query_instance || true)"
  if [[ -z "${instance_info}" ]]; then
    echo "[prt-host-watchdog] no pending/running instances remain for ${run_farm_tag}"
    exit 0
  fi

  instance_id="$(awk 'NR==1 {print $1}' <<<"${instance_info}")"
  private_ip="$(awk 'NR==1 {print $2}' <<<"${instance_info}")"
  state="$(awk 'NR==1 {print $3}' <<<"${instance_info}")"

  if [[ -z "${private_ip}" || "${state}" != "running" ]]; then
    sleep "${poll_seconds}"
    continue
  fi

  if [[ "${private_ip}" != "${last_private_ip}" ]]; then
    echo "[prt-host-watchdog] monitoring instance=${instance_id} private_ip=${private_ip}"
    last_private_ip="${private_ip}"
  fi

  probe_output="$(
    run_ssh_probe "ubuntu@${private_ip}" \
      env \
        ARM_MARKER_B64="${arm_marker_b64}" \
        COMPLETE_REGEX_B64="${complete_regex_b64}" \
        GUEST_LOG_PATH_B64="${guest_log_path_b64}" \
        GUEST_SPARSE_LOG_PATH_B64="${guest_sparse_log_path_b64}" \
        GUEST_STATUS_PATH_B64="${guest_status_path_b64}" \
        GUEST_PROC_STAGE_PATH_B64="${guest_proc_stage_path_b64}" \
        GUEST_RUNNER_STAGE_PATH_B64="${guest_runner_stage_path_b64}" \
        GUEST_RUNNER_POST_STAGE_PATH_B64="${guest_runner_post_stage_path_b64}" \
        GUEST_RUNNER_EARLY_STAGE_PATH_B64="${guest_runner_early_stage_path_b64}" \
        GUEST_BINARY_STAGE_PATH_B64="${guest_binary_stage_path_b64}" \
        GUEST_RUNNER_PROC_STAGE_PATH_B64="${guest_runner_proc_stage_path_b64}" \
        GUEST_CHECKPOINT_LOG_PATH_B64="${guest_checkpoint_log_path_b64}" \
        GUEST_TRIGGER_LOG_PATH_B64="${guest_trigger_log_path_b64}" \
        REMOTE_IMG_GLOB_B64="${remote_img_glob_b64}" \
      bash -s <<'EOF'
arm_marker="$(printf '%s' "${ARM_MARKER_B64}" | base64 -d)"
complete_regex="$(printf '%s' "${COMPLETE_REGEX_B64}" | base64 -d)"
guest_log_path="$(printf '%s' "${GUEST_LOG_PATH_B64}" | base64 -d)"
guest_sparse_log_path="$(printf '%s' "${GUEST_SPARSE_LOG_PATH_B64}" | base64 -d)"
guest_status_path="$(printf '%s' "${GUEST_STATUS_PATH_B64}" | base64 -d)"
guest_proc_stage_path="$(printf '%s' "${GUEST_PROC_STAGE_PATH_B64}" | base64 -d)"
guest_runner_stage_path="$(printf '%s' "${GUEST_RUNNER_STAGE_PATH_B64}" | base64 -d)"
guest_runner_post_stage_path="$(printf '%s' "${GUEST_RUNNER_POST_STAGE_PATH_B64}" | base64 -d)"
guest_runner_early_stage_path="$(printf '%s' "${GUEST_RUNNER_EARLY_STAGE_PATH_B64}" | base64 -d)"
guest_binary_stage_path="$(printf '%s' "${GUEST_BINARY_STAGE_PATH_B64}" | base64 -d)"
guest_runner_proc_stage_path="$(printf '%s' "${GUEST_RUNNER_PROC_STAGE_PATH_B64}" | base64 -d)"
guest_checkpoint_log_path="$(printf '%s' "${GUEST_CHECKPOINT_LOG_PATH_B64}" | base64 -d)"
guest_trigger_log_path="$(printf '%s' "${GUEST_TRIGGER_LOG_PATH_B64}" | base64 -d)"
remote_img_glob="$(printf '%s' "${REMOTE_IMG_GLOB_B64}" | base64 -d)"
uartlog=/home/ubuntu/sim_slot_0/uartlog
heartbeat=/home/ubuntu/sim_slot_0/heartbeat.csv
img="$(ls ${remote_img_glob} 2>/dev/null | head -n 1 || true)"
uart_size=0
arm_seen=0
done_seen=0
guest_log_size=0
guest_sparse_log_size=0
guest_status_size=0
guest_proc_stage_size=0
guest_runner_stage_size=0
guest_runner_post_stage_size=0
guest_runner_early_stage_size=0
guest_binary_stage_size=0
guest_runner_proc_stage_size=0
guest_checkpoint_log_size=0
guest_trigger_log_size=0
if [[ -f "${uartlog}" ]]; then
  uart_size="$(wc -c < "${uartlog}" | tr -d '[:space:]')"
  grep -Fq -- "${arm_marker}" "${uartlog}" && arm_seen=1 || true
  grep -Eq -- "${complete_regex}" "${uartlog}" && done_seen=1 || true
fi
if [[ -n "${img}" ]]; then
  guest_log_size="$(sudo debugfs -R "cat ${guest_log_path}" "${img}" 2>/dev/null | wc -c | tr -d '[:space:]' || true)"
  guest_sparse_log_size="$(sudo debugfs -R "cat ${guest_sparse_log_path}" "${img}" 2>/dev/null | wc -c | tr -d '[:space:]' || true)"
  guest_status_size="$(sudo debugfs -R "cat ${guest_status_path}" "${img}" 2>/dev/null | wc -c | tr -d '[:space:]' || true)"
  if [[ -n "${guest_proc_stage_path}" ]]; then
    guest_proc_stage_size="$(sudo debugfs -R "cat ${guest_proc_stage_path}" "${img}" 2>/dev/null | wc -c | tr -d '[:space:]' || true)"
  fi
  if [[ -n "${guest_runner_stage_path}" ]]; then
    guest_runner_stage_size="$(sudo debugfs -R "cat ${guest_runner_stage_path}" "${img}" 2>/dev/null | wc -c | tr -d '[:space:]' || true)"
  fi
  if [[ -n "${guest_runner_post_stage_path}" ]]; then
    guest_runner_post_stage_size="$(sudo debugfs -R "cat ${guest_runner_post_stage_path}" "${img}" 2>/dev/null | wc -c | tr -d '[:space:]' || true)"
  fi
  if [[ -n "${guest_runner_early_stage_path}" ]]; then
    guest_runner_early_stage_size="$(sudo debugfs -R "cat ${guest_runner_early_stage_path}" "${img}" 2>/dev/null | wc -c | tr -d '[:space:]' || true)"
  fi
  if [[ -n "${guest_binary_stage_path}" ]]; then
    guest_binary_stage_size="$(sudo debugfs -R "cat ${guest_binary_stage_path}" "${img}" 2>/dev/null | wc -c | tr -d '[:space:]' || true)"
  fi
  if [[ -n "${guest_runner_proc_stage_path}" ]]; then
    guest_runner_proc_stage_size="$(sudo debugfs -R "cat ${guest_runner_proc_stage_path}" "${img}" 2>/dev/null | wc -c | tr -d '[:space:]' || true)"
  fi
  if [[ -n "${guest_checkpoint_log_path}" ]]; then
    guest_checkpoint_log_size="$(sudo debugfs -R "cat ${guest_checkpoint_log_path}" "${img}" 2>/dev/null | wc -c | tr -d '[:space:]' || true)"
  fi
  if [[ -n "${guest_trigger_log_path}" ]]; then
    guest_trigger_log_size="$(sudo debugfs -R "cat ${guest_trigger_log_path}" "${img}" 2>/dev/null | wc -c | tr -d '[:space:]' || true)"
  fi
fi
hb_last="$(tail -n 1 "${heartbeat}" 2>/dev/null || true)"
printf 'UART_SIZE=%s\n' "${uart_size:-0}"
printf 'ARM_SEEN=%s\n' "${arm_seen}"
printf 'DONE_SEEN=%s\n' "${done_seen}"
printf 'GUEST_LOG_SIZE=%s\n' "${guest_log_size:-0}"
printf 'GUEST_SPARSE_LOG_SIZE=%s\n' "${guest_sparse_log_size:-0}"
printf 'GUEST_STATUS_SIZE=%s\n' "${guest_status_size:-0}"
printf 'GUEST_PROC_STAGE_SIZE=%s\n' "${guest_proc_stage_size:-0}"
printf 'GUEST_RUNNER_STAGE_SIZE=%s\n' "${guest_runner_stage_size:-0}"
printf 'GUEST_RUNNER_POST_STAGE_SIZE=%s\n' "${guest_runner_post_stage_size:-0}"
printf 'GUEST_RUNNER_EARLY_STAGE_SIZE=%s\n' "${guest_runner_early_stage_size:-0}"
printf 'GUEST_BINARY_STAGE_SIZE=%s\n' "${guest_binary_stage_size:-0}"
printf 'GUEST_RUNNER_PROC_STAGE_SIZE=%s\n' "${guest_runner_proc_stage_size:-0}"
printf 'GUEST_CHECKPOINT_LOG_SIZE=%s\n' "${guest_checkpoint_log_size:-0}"
printf 'GUEST_TRIGGER_LOG_SIZE=%s\n' "${guest_trigger_log_size:-0}"
printf 'HB_LAST=%s\n' "${hb_last}"
EOF
  )"

  uart_size="$(awk -F= '/^UART_SIZE=/{print $2}' <<<"${probe_output}")"
  arm_seen="$(awk -F= '/^ARM_SEEN=/{print $2}' <<<"${probe_output}")"
  done_seen="$(awk -F= '/^DONE_SEEN=/{print $2}' <<<"${probe_output}")"
  guest_log_size="$(awk -F= '/^GUEST_LOG_SIZE=/{print $2}' <<<"${probe_output}")"
  guest_sparse_log_size="$(awk -F= '/^GUEST_SPARSE_LOG_SIZE=/{print $2}' <<<"${probe_output}")"
  guest_status_size="$(awk -F= '/^GUEST_STATUS_SIZE=/{print $2}' <<<"${probe_output}")"
  guest_proc_stage_size="$(awk -F= '/^GUEST_PROC_STAGE_SIZE=/{print $2}' <<<"${probe_output}")"
  guest_runner_stage_size="$(awk -F= '/^GUEST_RUNNER_STAGE_SIZE=/{print $2}' <<<"${probe_output}")"
  guest_runner_post_stage_size="$(awk -F= '/^GUEST_RUNNER_POST_STAGE_SIZE=/{print $2}' <<<"${probe_output}")"
  guest_runner_early_stage_size="$(awk -F= '/^GUEST_RUNNER_EARLY_STAGE_SIZE=/{print $2}' <<<"${probe_output}")"
  guest_binary_stage_size="$(awk -F= '/^GUEST_BINARY_STAGE_SIZE=/{print $2}' <<<"${probe_output}")"
  guest_runner_proc_stage_size="$(awk -F= '/^GUEST_RUNNER_PROC_STAGE_SIZE=/{print $2}' <<<"${probe_output}")"
  guest_checkpoint_log_size="$(awk -F= '/^GUEST_CHECKPOINT_LOG_SIZE=/{print $2}' <<<"${probe_output}")"
  guest_trigger_log_size="$(awk -F= '/^GUEST_TRIGGER_LOG_SIZE=/{print $2}' <<<"${probe_output}")"
  hb_last="$(awk -F= '/^HB_LAST=/{print $2}' <<<"${probe_output}")"

  uart_size="${uart_size:-0}"
  guest_log_size="${guest_log_size:-0}"
  guest_sparse_log_size="${guest_sparse_log_size:-0}"
  guest_status_size="${guest_status_size:-0}"
  guest_proc_stage_size="${guest_proc_stage_size:-0}"
  guest_runner_stage_size="${guest_runner_stage_size:-0}"
  guest_runner_post_stage_size="${guest_runner_post_stage_size:-0}"
  guest_runner_early_stage_size="${guest_runner_early_stage_size:-0}"
  guest_binary_stage_size="${guest_binary_stage_size:-0}"
  guest_runner_proc_stage_size="${guest_runner_proc_stage_size:-0}"
  guest_checkpoint_log_size="${guest_checkpoint_log_size:-0}"
  guest_trigger_log_size="${guest_trigger_log_size:-0}"

  now_epoch="$(date +%s)"

  if [[ "${done_seen}" == "1" ]]; then
    echo "[prt-host-watchdog] completion marker observed; stopping monitor"
    exit 0
  fi

  if [[ "${armed}" == "0" && "${arm_seen}" == "1" ]]; then
    armed=1
    last_progress_epoch="${now_epoch}"
    last_uart_size="${uart_size}"
    last_guest_log_size="${guest_log_size}"
    last_guest_sparse_log_size="${guest_sparse_log_size}"
    last_guest_status_size="${guest_status_size}"
    last_guest_proc_stage_size="${guest_proc_stage_size}"
    last_guest_runner_stage_size="${guest_runner_stage_size}"
    last_guest_runner_post_stage_size="${guest_runner_post_stage_size}"
    last_guest_runner_early_stage_size="${guest_runner_early_stage_size}"
    last_guest_binary_stage_size="${guest_binary_stage_size}"
    last_guest_runner_proc_stage_size="${guest_runner_proc_stage_size}"
    last_guest_checkpoint_log_size="${guest_checkpoint_log_size}"
    last_guest_trigger_log_size="${guest_trigger_log_size}"
    last_hb_line="${hb_last}"
    if [[ -n "${hb_last}" ]]; then
      last_heartbeat_epoch="${now_epoch}"
    else
      last_heartbeat_epoch=0
    fi
    echo "[prt-host-watchdog] arm marker observed at $(date -u +%Y-%m-%dT%H:%M:%SZ) hb='${hb_last}'"
    sleep "${poll_seconds}"
    continue
  fi

  if [[ "${armed}" == "0" && "${arm_on_guest_status_nonzero}" == "1" ]] && (( guest_status_size > 0 )); then
    armed=1
    last_progress_epoch="${now_epoch}"
    last_uart_size="${uart_size}"
    last_guest_log_size="${guest_log_size}"
    last_guest_sparse_log_size="${guest_sparse_log_size}"
    last_guest_status_size="${guest_status_size}"
    last_guest_proc_stage_size="${guest_proc_stage_size}"
    last_guest_runner_stage_size="${guest_runner_stage_size}"
    last_guest_runner_post_stage_size="${guest_runner_post_stage_size}"
    last_guest_runner_early_stage_size="${guest_runner_early_stage_size}"
    last_guest_binary_stage_size="${guest_binary_stage_size}"
    last_guest_runner_proc_stage_size="${guest_runner_proc_stage_size}"
    last_guest_checkpoint_log_size="${guest_checkpoint_log_size}"
    last_guest_trigger_log_size="${guest_trigger_log_size}"
    last_hb_line="${hb_last}"
    if [[ -n "${hb_last}" ]]; then
      last_heartbeat_epoch="${now_epoch}"
    else
      last_heartbeat_epoch=0
    fi
    echo "[prt-host-watchdog] guest-status arm observed at $(date -u +%Y-%m-%dT%H:%M:%SZ) hb='${hb_last}' guest_status=${guest_status_size}"
    sleep "${poll_seconds}"
    continue
  fi

  if [[ "${armed}" == "0" ]]; then
    sleep "${poll_seconds}"
    continue
  fi

  heartbeat_progressed=0
  if [[ "${heartbeat_liveness_enable}" == "1" && -n "${hb_last}" ]]; then
    if [[ "${hb_last}" != "${last_hb_line}" ]]; then
      heartbeat_progressed=1
      last_hb_line="${hb_last}"
      last_heartbeat_epoch="${now_epoch}"
    elif (( last_heartbeat_epoch == 0 )); then
      last_hb_line="${hb_last}"
      last_heartbeat_epoch="${now_epoch}"
    fi
  fi

  progressed=0
  if (( uart_size > last_uart_size )); then
    progressed=1
    last_uart_size="${uart_size}"
  fi
  if (( guest_log_size > last_guest_log_size )); then
    progressed=1
    last_guest_log_size="${guest_log_size}"
  fi
  if (( guest_sparse_log_size > last_guest_sparse_log_size )); then
    progressed=1
    last_guest_sparse_log_size="${guest_sparse_log_size}"
  fi
  if (( guest_status_size > last_guest_status_size )); then
    progressed=1
    last_guest_status_size="${guest_status_size}"
  fi
  if (( guest_runner_stage_size > last_guest_runner_stage_size )); then
    progressed=1
    last_guest_runner_stage_size="${guest_runner_stage_size}"
  fi
  if (( guest_runner_post_stage_size > last_guest_runner_post_stage_size )); then
    progressed=1
    last_guest_runner_post_stage_size="${guest_runner_post_stage_size}"
  fi
  if (( guest_runner_early_stage_size > last_guest_runner_early_stage_size )); then
    progressed=1
    last_guest_runner_early_stage_size="${guest_runner_early_stage_size}"
  fi
  if (( guest_binary_stage_size > last_guest_binary_stage_size )); then
    progressed=1
    last_guest_binary_stage_size="${guest_binary_stage_size}"
  fi
  if (( guest_checkpoint_log_size > last_guest_checkpoint_log_size )); then
    progressed=1
    last_guest_checkpoint_log_size="${guest_checkpoint_log_size}"
  fi
  if (( guest_trigger_log_size > last_guest_trigger_log_size )); then
    progressed=1
    last_guest_trigger_log_size="${guest_trigger_log_size}"
  fi
  # Proc snapshots are diagnostic by design and may keep growing while the
  # workload is otherwise hung, so do not treat them as forward progress.
  last_guest_proc_stage_size="${guest_proc_stage_size}"
  last_guest_runner_proc_stage_size="${guest_runner_proc_stage_size}"

  if (( progressed == 1 )); then
    last_progress_epoch="${now_epoch}"
    echo "[prt-host-watchdog] progress hb='${hb_last}' uart=${uart_size} guest_log=${guest_log_size} guest_sparse=${guest_sparse_log_size} guest_checkpoint=${guest_checkpoint_log_size} guest_trigger=${guest_trigger_log_size} guest_status=${guest_status_size} guest_proc=${guest_proc_stage_size} guest_runner=${guest_runner_stage_size} guest_runner_post=${guest_runner_post_stage_size} guest_runner_early=${guest_runner_early_stage_size} guest_binary=${guest_binary_stage_size} guest_runner_proc=${guest_runner_proc_stage_size}"
  fi

  idle_seconds=$((now_epoch - last_progress_epoch))
  heartbeat_idle_seconds="${idle_seconds}"
  if [[ "${heartbeat_liveness_enable}" == "1" ]] && (( last_heartbeat_epoch > 0 )); then
    heartbeat_idle_seconds=$((now_epoch - last_heartbeat_epoch))
  fi
  if (( heartbeat_progressed == 1 )) && (( progressed == 0 )); then
    echo "[prt-host-watchdog] heartbeat-progress hb='${hb_last}' idle=${idle_seconds}s hb_idle=${heartbeat_idle_seconds}s uart=${uart_size} guest_log=${guest_log_size} guest_sparse=${guest_sparse_log_size} guest_status=${guest_status_size}"
  fi
  echo "[prt-host-watchdog] hb='${hb_last}' idle=${idle_seconds}s hb_idle=${heartbeat_idle_seconds}s uart=${uart_size} guest_log=${guest_log_size} guest_sparse=${guest_sparse_log_size} guest_checkpoint=${guest_checkpoint_log_size} guest_trigger=${guest_trigger_log_size} guest_status=${guest_status_size} guest_proc=${guest_proc_stage_size} guest_runner=${guest_runner_stage_size} guest_runner_post=${guest_runner_post_stage_size} guest_runner_early=${guest_runner_early_stage_size} guest_binary=${guest_binary_stage_size} guest_runner_proc=${guest_runner_proc_stage_size}"

  if (( idle_seconds >= idle_timeout_seconds )); then
    heartbeat_alive=0
    if [[ "${heartbeat_liveness_enable}" == "1" && "${live_idle_timeout_seconds}" != "0" ]] && (( last_heartbeat_epoch > 0 )) && (( heartbeat_idle_seconds < idle_timeout_seconds )) && (( idle_seconds < live_idle_timeout_seconds )); then
      heartbeat_alive=1
    fi
    if (( heartbeat_alive == 0 )); then
      stamp="$(date -u +%Y%m%dT%H%M%SZ)"
      echo "[prt-host-watchdog] host idle timeout reached after ${idle_seconds}s (hb_idle=${heartbeat_idle_seconds}s)"
      capture_remote_state "${private_ip}" "${stamp}"
      terminate_runfarm
      exit 124
    fi
  fi

  if [[ "${heartbeat_liveness_enable}" == "1" && "${live_idle_timeout_seconds}" != "0" ]] && (( idle_seconds >= live_idle_timeout_seconds )); then
    stamp="$(date -u +%Y%m%dT%H%M%SZ)"
    echo "[prt-host-watchdog] host live-idle timeout reached after ${idle_seconds}s while heartbeat remained active (hb_idle=${heartbeat_idle_seconds}s)"
    capture_remote_state "${private_ip}" "${stamp}"
    terminate_runfarm
    exit 124
  fi

  sleep "${poll_seconds}"
done
