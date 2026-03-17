import os
from multiprocessing import Pool, Lock
import subprocess
import argparse
import time
import signal
import logging
from datetime import datetime
from pathlib import Path
import re

BINARY_PATH = "./build/expr_single_pipeline"
OUTPUT_ROOT = "expr/output/expr_single_pipeline/script_output/"

modelList = [
	# "resnet18",

    "mobilenetv2",
    "resnet50",
    # "bertbase",
    # "effinetb0",
    # "vitb16",
    # "vitsmall16",
    # "w2v2base",
    # "gnmt",
 
    # "pointpillars",
    # "berttiny",
    # "bertmini",
    # "bertsmall",
    # "bertmedium",
]

# pipeSpmStrategyList = ["allbank", "mindis"]
pipeSpmStrategyList = ["allbank"]

# pipe_acc_strategy_list = ["hilbert", "free"]
pipe_acc_strategy_list = ["hilbert"]

# per-model accelerator counts
# accList = [16,32,64]
accList = [8,16,24,32]
# accList = [8]

batch_size = [16]
# batch_size = [8]

PARTITION_NUM = [3]
# PARTITION_NUM = [3]

METHOD = ["ours", "gemini", "tangram"]
# METHOD = ["ours"]

# dramChannels = [1,2,3,4]
dramChannels = [1]
dramBWPerChannel = 19

noc_list = [
    "HomoTileMesh" # 不再使用none noc
]

# noc_flit_sizes = [4, 8, 16, 32, 64, 128]
# noc_flit_sizes = [64,32]
noc_flit_sizes = [64]

spm_per_bank_list = [1024] # KB

# debug_flags = ["PIPELINE","STAGE","THREAD_PIPELINE"]
debug_flags = ["THREAD_PIPELINE"]
# debug_flags = ["NONE"]

LATENCY_RE = re.compile(r"Total Latency:\s*(\d+)", re.IGNORECASE)

# 全局锁，用于安全写入日志文件
log_lock = Lock()

TASK_TIMEOUT = 180000 # 超时时间（s）

def init_logger():
    """初始化日志系统"""
    # 创建基于时间戳的日志文件名
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    log_filename = f"expr_single_pipeline_{timestamp}.log"
    
    # 配置日志系统
    logging.basicConfig(
        filename=log_filename,
        level=logging.INFO,
        format='%(asctime)s - %(levelname)s - %(message)s',
        datefmt='%Y-%m-%d %H:%M:%S'
    )
    
    # 添加控制台输出
    console_handler = logging.StreamHandler()
    console_handler.setLevel(logging.INFO)
    formatter = logging.Formatter('%(asctime)s - %(levelname)s - %(message)s')
    console_handler.setFormatter(formatter)
    logging.getLogger().addHandler(console_handler)
    
    return log_filename

def summarize_task(task: dict) -> str:
    return (
        f"Model={task['modelName']} part={task['partition_num']} seg={task['seg_idx']} "
        f"mapping={task['mappingPath']} method={task['method']} batch={task['totalBatch']} "
        f"acc={task['acc_num']} spm={task['spm_bank_size']}KB noc={task['noc']} flit={task['noc_flit_size']} "
        f"channels={task['channels']} spm_strategy={task['pipeSpmStrategy']} acc_strategy={task['pipeAccStrategy']}"
    )


def log_timeout(task: dict, timeout: int = TASK_TIMEOUT):
    """记录任务超时状态"""
    param_summary = summarize_task(task)
    with log_lock:
        logging.error(f"TIMEOUT ({timeout}s): {param_summary}")


def log_completion(task: dict):
    """记录任务完成状态"""
    param_summary = summarize_task(task)
    with log_lock:
        logging.info(f"COMPLETED: {param_summary}")


def log_error(task: dict, message: str):
    param_summary = summarize_task(task)
    with log_lock:
        logging.error(f"ERROR: {param_summary} -> {message}")
def run_command(cmd, timeout, stdout_path=None, stderr_path=None):
    """运行命令并处理超时，将标准输出写入stdout_path、标准错误写入stderr_path（如果提供）。
    返回(returncode, stdout_str, stderr_str).
    """
    try:
        # open stdout/stderr files if requested
        stdout_f = None
        # Do not open stderr file up-front; create it only if we have data to write.
        # stderr_f intentionally not opened unless we need to write stderr
        if stdout_path is not None:
            os.makedirs(os.path.dirname(stdout_path), exist_ok=True)
            stdout_f = open(stdout_path, 'wb')

        process = subprocess.Popen(
            cmd,
            shell=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            preexec_fn=os.setsid  # 用于在Linux上创建新的进程组
        )

        try:
            stdout_data, stderr_data = process.communicate(timeout=timeout)
            # If stdout file was requested, write it
            if stdout_f is not None:
                try:
                    if stdout_data is not None:
                        stdout_f.write(stdout_data)
                    stdout_f.flush()
                finally:
                    stdout_f.close()
            # Write stderr only if there is data and a path was provided
            if stderr_path is not None and stderr_data:
                try:
                    os.makedirs(os.path.dirname(stderr_path), exist_ok=True)
                    with open(stderr_path, 'wb') as f:
                        f.write(stderr_data)
                except Exception:
                    pass

            stdout_text = stdout_data.decode('utf-8', errors='replace') if stdout_data is not None else ''
            stderr_text = stderr_data.decode('utf-8', errors='replace') if stderr_data is not None else ''
            return process.returncode, stdout_text, stderr_text
        except subprocess.TimeoutExpired as te:
            # 超时处理 - 终止整个进程组
            try:
                os.killpg(os.getpgid(process.pid), signal.SIGTERM)
            except Exception:
                pass
            # te may contain partial output
            stdout_data = te.output if hasattr(te, 'output') else None
            stderr_data = te.stderr if hasattr(te, 'stderr') else None
            # write stdout if available
            if stdout_f is not None:
                try:
                    if stdout_data is not None:
                        stdout_f.write(stdout_data)
                    stdout_f.flush()
                finally:
                    stdout_f.close()
            # write stderr only if non-empty
            if stderr_path is not None and stderr_data:
                try:
                    os.makedirs(os.path.dirname(stderr_path), exist_ok=True)
                    with open(stderr_path, 'wb') as f:
                        f.write(stderr_data)
                except Exception:
                    pass

            raise TimeoutError(f"命令超时 ({timeout} s)")

    except Exception as e:
        return -1, '', str(e)
def parse_latency_from_log(log_path: Path) -> int:
    if not log_path.exists():
        raise FileNotFoundError(f"Log file not found: {log_path}")
    text = log_path.read_text(errors='ignore')
    m = LATENCY_RE.search(text)
    if not m:
        raise ValueError(f"Cannot find 'Total Latency' in log: {log_path}")
    return int(m.group(1))


def run_task(task: dict):
    """执行单个mapping并返回(latency, stdout_log_path)"""
    modelName = task['modelName']
    system = task['system']
    totalBatch = task['totalBatch']
    acc_num = task['acc_num']
    spm_bank_size = task['spm_bank_size']
    mappingPath = task['mappingPath']
    pipeSpmStrategy = task['pipeSpmStrategy']
    pipeAccStrategy = task['pipeAccStrategy']
    noc = task['noc']
    channels = task['channels']
    noc_flit_size = task['noc_flit_size']
    method = task['method']
    partition_num = task['partition_num']
    seg_idx = task['seg_idx']

    # Build a concise and unique output name
    outputName = (
        f"{modelName}_{system}_part-{partition_num}_seg-{seg_idx}_"
        f"{mappingPath.stem}_noc-{noc}-{noc_flit_size}_ch-{channels}_"
        f"acc-{acc_num}_spm-{spm_bank_size}_batch-{totalBatch}_method-{method}_"
        f"{pipeSpmStrategy}_{pipeAccStrategy}"
    )

    outputRoot = f"{OUTPUT_ROOT}"
    cmd = (
        f"{BINARY_PATH} "
        f"--system {system} "
        f"--model {modelName} "
        f"--output {outputName} "
        f"--output-root {outputRoot} "
        f"--spm-addr-type conti "
        f"--accels {acc_num if (acc_num & (acc_num - 1) == 0) else (1 << (acc_num - 1).bit_length()) } "
        f"--spm-size-per-bank-bytes {spm_bank_size} "
        f"--pipeline-mapping-path {mappingPath} "
        f"--total-batch {totalBatch} "
        f"--pipe-spm-strategy {pipeSpmStrategy} "
        f"--pipe-acc-strategy {pipeAccStrategy} "
        f"--noc {noc} "
        f"--channels {channels} "
        f"--noc0-flit-size {noc_flit_size} "
        f"--noc1-flit-size {noc_flit_size} "
        f"--debug-flags {','.join(debug_flags)} "
    )
    print(cmd)
    stdout_path = Path(outputRoot) / f"{outputName}-std.log"
    stderr_path = Path(outputRoot) / f"{outputName}-err.log"

    start_time = time.time()
    returncode, stdout_text, stderr_text = run_command(cmd, TASK_TIMEOUT, stdout_path=str(stdout_path), stderr_path=str(stderr_path))
    elapsed_time = time.time() - start_time

    if returncode != 0:
        error_msg = f"命令失败 ({elapsed_time:.1f} s, 退出码: {returncode})"
        if isinstance(stderr_text, str) and "TimeoutError" in stderr_text:
            error_msg = f"超时终止 ({TASK_TIMEOUT} s)"
        raise RuntimeError(f"{error_msg}\n命令: {cmd}\n错误: {stderr_text}")

    latency = parse_latency_from_log(stdout_path)
    return latency, stdout_path

def run_wrapper(task):
    """包装函数，用于捕获异常和日志记录，返回结构化结果"""
    try:
        start_time = time.time()
        latency, stdout_path = run_task(task)
        elapsed_time = time.time() - start_time
        log_completion(task)
        with log_lock:
            logging.info(f"任务耗时: {elapsed_time:.1f} s")
        return {"status": "ok", "task": task, "latency": latency, "stdout": str(stdout_path)}
    except TimeoutError as e:
        log_timeout(task)
        with log_lock:
            logging.error(f"TIMEOUT: 任务超过 {TASK_TIMEOUT} s被终止: {summarize_task(task)}")
            logging.error(f"错误信息: {str(e)}")
        return {"status": "timeout", "task": task, "error": str(e)}
    except Exception as e:
        log_error(task, str(e))
        return {"status": "error", "task": task, "error": str(e)}


def discover_tasks():
    """扫描所有mapping文件，生成任务列表"""
    tasks = []
    system = "spm"
    for modelName in modelList:
        for partition_num in PARTITION_NUM:
            part_dir = Path(f"models/pipeline/{modelName}/schedule_point/pipeline_mapping/{partition_num}")
            if not part_dir.exists():
                continue
            for seg_dir in sorted([p for p in part_dir.iterdir() if p.is_dir()]):
                seg_idx = int(seg_dir.name)
                for acc_num in accList:
                    for spm_bank_size in spm_per_bank_list:
                        for totalBatch in batch_size:
                            for dram_channel in dramChannels:
                                dram_bw = dram_channel * dramBWPerChannel
                                for noc_flit_size in noc_flit_sizes:
                                    for method in METHOD:
                                        filename = f"{acc_num}_{spm_bank_size}_{totalBatch}_{dram_bw}_{noc_flit_size}_{method}.yaml"
                                        mapping_file = seg_dir / filename
                                        if not mapping_file.exists():
                                            continue
                                        for noc in noc_list:
                                            for pipeStrategy in pipeSpmStrategyList:
                                                for pipeAcc in pipe_acc_strategy_list:
                                                    task = {
                                                        "system": system,
                                                        "method": method,
                                                        "modelName": modelName,
                                                        "totalBatch": totalBatch,
                                                        "acc_num": acc_num,
                                                        "spm_bank_size": spm_bank_size,
                                                        "mappingPath": mapping_file,
                                                        "pipeSpmStrategy": pipeStrategy,
                                                        "pipeAccStrategy": pipeAcc,
                                                        "noc": noc,
                                                        "channels": dram_channel,
                                                        "noc_flit_size": noc_flit_size,
                                                        "partition_num": partition_num,
                                                        "seg_idx": seg_idx,
                                                    }
                                                    tasks.append(task)
    return tasks

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--num-proc", type=int, default=5)
    
    args = parser.parse_args()
    
    global TASK_TIMEOUT
    
    # 初始化日志系统
    log_filename = init_logger()
    logging.info(f"Starting simulation with {args.num_proc} processes. TASK_TIMEOUT: {TASK_TIMEOUT} s")
    logging.info(f"Log file: {os.path.abspath(log_filename)}")

    numProcesses = args.num_proc

    tasks = discover_tasks()
    logging.info(f"Total tasks to execute: {len(tasks)}")
    if not tasks:
        logging.warning("No tasks found. Please check mapping directories.")
        return
    
    output = []
    with Pool(processes=numProcesses) as pool:
        start_time = time.time()
        last_log_time = 0
        completed = 0
        total_tasks = len(tasks)

        for res in pool.imap_unordered(run_wrapper, tasks):
            output.append(res)
            completed += 1
            current_time = time.time()
            if current_time - last_log_time >= 60:
                elapsed = current_time - start_time
                with log_lock:
                    logging.info(f"Process: {completed}/{total_tasks}. Elapsed: {elapsed:.1f}秒")
                last_log_time = current_time
    
    success_results = [r for r in output if isinstance(r, dict) and r.get("status") == "ok"]
    timeout_results = [r for r in output if isinstance(r, dict) and r.get("status") == "timeout"]
    error_results = [r for r in output if isinstance(r, dict) and r.get("status") == "error"]

    with log_lock:
        logging.info(f"任务汇总: 成功 {len(success_results)}，超时 {len(timeout_results)}，错误 {len(error_results)}")
        if timeout_results or error_results:
            logging.warning(f"超时或失败的任务: {len(timeout_results) + len(error_results)}个")
            for res in timeout_results:
                logging.warning(f"超时任务: {summarize_task(res['task'])} -> {res.get('error')}")
            for res in error_results:
                logging.error(f"失败任务: {summarize_task(res['task'])} -> {res.get('error')}")


if __name__ == "__main__":
    main()
