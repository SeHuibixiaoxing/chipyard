import os
from multiprocessing import Pool, Lock
import subprocess
import argparse
from typing import Dict, List, Tuple, Set
import time
import signal
import logging
from datetime import datetime
from pathlib import Path

try:
    import yaml  # type: ignore
except ImportError as e:
    raise SystemExit("PyYAML is required: pip install pyyaml") from e

BINARY_PATH = "./build/expr_multi_pipeline"
OUTPUT_ROOT = "expr/output/expr_multi_pipeline/script_output/"

pipeSpmStrategyList = ["allbank"]
pipe_acc_strategy_list = ["hilbert"]

dramChannels = [4,8]
noc_list = ["HomoTileMesh"]
noc_flit_sizes = [64]
method_list = ["ours", "gemini", "tangram"]
debug_flags = ['THREAD_QOS_PIPELINE,THREAD_RUN_PIPELINE,RUNTIME_SOURCE_ALLOC']


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

def log_timeout(args, timeout=TASK_TIMEOUT):
    """记录任务超时状态"""
    (
        method,
        bundle_index,
        model_csv,
        totalBatch,
        acc_nums,
        spm_bank_sizes,
        mappingPath,
        pipeStrategy,
        pipeAccStrategy,
        dramChannels,
        noc_flit_size,
        debug_flags,
    ) = args

    param_summary = (
        f"Index: {bundle_index}, Model: {model_csv}, Method: {method}, TotalBatch: {totalBatch}, Acc: {acc_nums}, "
        f"SPM Banks: {spm_bank_sizes}, Mapping: {mappingPath}, Strategy: {pipeStrategy}, AccStrategy: {pipeAccStrategy}, "
        f"DRAM: {dramChannels}, noc_flit_size: {noc_flit_size}"
    )

    with log_lock:
        logging.error(f"TIMEOUT ({timeout}s): {param_summary}")


def log_completion(args):
    """记录任务完成状态"""
    (
        method,
        bundle_index,
        model_csv,
        totalBatch,
        acc_nums,
        spm_bank_sizes,
        mappingPath,
        pipeStrategy,
        pipeAccStrategy,
        dramChannels,
        noc_flit_size,
        debug_flags,
    ) = args

    param_summary = (
        f"Index: {bundle_index}, Model: {model_csv}, Method: {method}, TotalBatch: {totalBatch}, Acc: {acc_nums}, "
        f"SPM Banks: {spm_bank_sizes}, Mapping: {mappingPath}, Strategy: {pipeStrategy}, AccStrategy: {pipeAccStrategy}, DRAM: {dramChannels}, "
        f"noc_flit_size: {noc_flit_size}"
    )

    with log_lock:
        logging.info(f"COMPLETED: {param_summary}")
def run_command(cmd, timeout, stdout_path=None, stderr_path=None):
    """运行命令并处理超时，将标准输出写入stdout_path、标准错误写入stderr_path（如果提供）。
    返回(returncode, stdout_str, stderr_str).
    """
    try:
        # open stdout/stderr files if requested
        stdout_f = None
        # Do not open stderr file up-front; create it only if we have data to write.
        stderr_f = None
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
def run(method, bundle_index, model_csv, totalBatch, acc_nums, spm_bank_sizes, mappingPath, pipeSpmStrategy, pipeAccStrategy, channels, noc_flit_size, debug_flags):
    acc_desc = ",".join(str(a) for a in acc_nums)
    spm_desc = ",".join(str(s) for s in spm_bank_sizes)
    acc_sum = sum(acc_nums)
    # spm_bank_sizes are in KB (from analyze_speedups target), convert first entry to bytes for CLI.

    outputName = (
        f"idx-{bundle_index}_{method}_models-{model_csv}_batch-{totalBatch}_acc-{acc_desc}_"
        f"spm-{spm_desc}_noc-{noc_flit_size}_dram-{channels}_{pipeSpmStrategy}_{pipeAccStrategy}"
    )

    outputRoot = f"{OUTPUT_ROOT}"
    cmd = (
        f"{BINARY_PATH} "
        f"--model {model_csv} "
        f"--output {outputName} "
        f"--output-root {outputRoot} "
        f"--spm-addr-type conti "
        f"--accels 64 "
        f"--spm-size-per-bank-bytes 1024 "
        f"--pipeline-mapping-path {mappingPath} "
        f"--total-batch {totalBatch} "
        f"--pipe-spm-strategy {pipeSpmStrategy} "
        f"--pipe-acc-strategy {pipeAccStrategy} "
        f"--channels {channels} "
        f"--noc0-flit-size {noc_flit_size} "
        f"--noc1-flit-size {noc_flit_size} "
        f"--debug-flags {','.join(debug_flags)} "
    )
    print(cmd)
    stdout_path = os.path.join(outputRoot, f"{outputName}-std.log")
    stderr_path = os.path.join(outputRoot, f"{outputName}-err.log")
    start_time = time.time()
    returncode, stdout_text, stderr_text = run_command(cmd, TASK_TIMEOUT, stdout_path=stdout_path, stderr_path=stderr_path)
    elapsed_time = time.time() - start_time

    if returncode != 0:
        error_msg = f"命令失败 ({elapsed_time:.1f} s, 退出码: {returncode})"
        if isinstance(stderr_text, str) and "TimeoutError" in stderr_text:
            error_msg = f"超时终止 ({TASK_TIMEOUT} s)"
        raise RuntimeError(f"{error_msg}\n命令: {cmd}\n错误: {stderr_text}")

def run_wrapper(args):
    """包装函数，用于捕获异常和日志记录"""
    try:
        # 执行原始任务
        start_time = time.time()
        result = run(*args)
        elapsed_time = time.time() - start_time
        
        # 记录任务完成
        log_completion(args)
        
        with log_lock:
            logging.info(f"任务耗时: {elapsed_time:.1f} s")
        
        return result
    except TimeoutError as e:
        # 记录超时错误
        log_timeout(args)
        with log_lock:
            logging.error(f"TIMEOUT: 任务超过 {TASK_TIMEOUT} s被终止: {args}")
            logging.error(f"错误信息: {str(e)}")
        return f"Timeout: {args}"
    except Exception as e:
        # 捕获并记录异常
        log_timeout(args)  # 虽然可能是其他错误，但记录为超时便于识别
        with log_lock:
            logging.error(f"ERROR in task {args}: {str(e)}")
        return f"Error: {args}: {str(e)}"

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--num-proc", type=int, default=5)
    parser.add_argument("--yaml", type=Path, default=Path("multi_model_4.yaml"), help="Path to YAML produced by analyze_speedups.py")
    parser.add_argument("--limit-start", type=int, default=None, help="Start index (0-based) of tasks to run")
    parser.add_argument("--limit-end", type=int, default=None, help="End index (inclusive, 0-based) of tasks to run")
    parser.add_argument(
        "--pick",
        type=str,
        default=None,
        help=(
            "Select top-N bundles per model without duplicates. "
            "Format: model1:n1,model2:n2 (e.g., 'bertbase:3,resnet50:2'). "
            "Ranking uses per-model avg speedup = (tangram+gemini)/2."
        ),
    )
    
    args = parser.parse_args()
    
    global TASK_TIMEOUT
    
    # 初始化日志系统
    log_filename = init_logger()
    logging.info(f"Starting simulation with {args.num_proc} processes.TASK_TIMEOUT: {TASK_TIMEOUT} s")
    logging.info(f"Log file: {os.path.abspath(log_filename)}")

    numProcesses = args.num_proc

    def parse_pick_spec(spec: str) -> List[Tuple[str, int]]:
        pairs: List[Tuple[str, int]] = []
        if not spec:
            return pairs
        for token in spec.split(","):
            token = token.strip()
            if not token:
                continue
            if ":" not in token:
                continue
            model, cnt = token.split(":", 1)
            model = model.strip()
            try:
                num = int(cnt.strip())
            except Exception:
                continue
            if model and num > 0:
                pairs.append((model, num))
        return pairs

    def select_entry_indices(data_list: List[dict], pick_pairs: List[Tuple[str, int]]) -> Set[int]:
        # Select unique entry indices by ranking per-model avg speedup (tangram + gemini)/2
        selected: Set[int] = set()
        if not pick_pairs:
            return selected
        for model, need in pick_pairs:
            # Build candidate list (idx, score)
            candidates: List[Tuple[int, float]] = []
            for i, entry in enumerate(data_list):
                if i in selected:
                    continue
                models_csv = entry.get("model_name", "") or ""
                models = [m.strip() for m in models_csv.split(",") if m.strip()]
                if model not in models:
                    continue
                # per-model speedups
                tg = entry.get("tangram", {}) or {}
                gm = entry.get("gemini", {}) or {}
                tg_pm = (tg.get("per_model", {}) or {}).get(model)
                gm_pm = (gm.get("per_model", {}) or {}).get(model)
                vals: List[float] = []
                try:
                    if tg_pm is not None:
                        vals.append(float(tg_pm))
                except Exception:
                    pass
                try:
                    if gm_pm is not None:
                        vals.append(float(gm_pm))
                except Exception:
                    pass
                if not vals:
                    # fallback to overall score if per-model missing
                    try:
                        score = float(entry.get("score", 0.0) or 0.0)
                    except Exception:
                        score = 0.0
                else:
                    score = sum(vals) / len(vals)
                candidates.append((i, score))
            # sort by score desc and pick top 'need'
            candidates.sort(key=lambda x: x[1], reverse=True)
            for idx, _ in candidates[:need]:
                if idx not in selected:
                    selected.add(idx)
        return selected

    def load_tasks(yaml_path: Path):
        data = yaml.safe_load(yaml_path.read_text())
        if not data:
            return []
        # If pick is specified, filter entries first
        pick_pairs = parse_pick_spec(args.pick) if args.pick else []
        selected_indices: Set[int] = set()
        if pick_pairs:
            selected_indices = select_entry_indices(data, pick_pairs)
            with log_lock:
                logging.info(
                    f"--pick applied: {pick_pairs}. Selected {len(selected_indices)} unique bundle(s)."
                )
        tasks = []
        for i, entry in enumerate(data):
            if selected_indices and i not in selected_indices:
                continue
            idx = entry.get("index")
            model_csv = entry.get("model_name", "")
            target = entry.get("target", {})
            acc_nums_raw = target.get("acc_num", [])
            spm_raw = target.get("spm_bank_per_size", [])
            total_batch = target.get("total_batch")
            try:
                total_batch = int(total_batch)
            except Exception:
                logging.warning(f"Skip bundle with invalid total_batch: {total_batch}")
                continue

            try:
                acc_nums = [int(a) for a in acc_nums_raw]
                spm_bank_sizes = [int(s) for s in spm_raw]
            except Exception:
                logging.warning(f"Skip bundle with invalid acc/spm values: {acc_nums_raw}, {spm_raw}")
                continue

            mappings_by_method = {
                "ours": entry.get("ours_mapping_path", []),
                "gemini": entry.get("gemini_mapping_path", []),
                "tangram": entry.get("tangram_mapping_path", []),
            }

            for method in method_list:
                mapping_list = mappings_by_method.get(method, [])
                if not mapping_list:
                    continue
                mapping_csv = ",".join(mapping_list)
                for pipeStrategy in pipeSpmStrategyList:
                    for pipeAcc in pipe_acc_strategy_list:
                        for dram_channel in dramChannels:
                            for flit in noc_flit_sizes:
                                tasks.append(
                                    (
                                        method,
                                        idx,
                                        model_csv,
                                        total_batch,
                                        acc_nums,
                                        spm_bank_sizes,
                                        mapping_csv,
                                        pipeStrategy,
                                        pipeAcc,
                                        dram_channel,
                                        flit,
                                        debug_flags,
                                    )
                                )
        return tasks

    argList = load_tasks(args.yaml)
    # Apply range filtering if provided
    if args.limit_start is not None or args.limit_end is not None:
        start = args.limit_start or 0
        end = args.limit_end if args.limit_end is not None else len(argList) - 1
        if start < 0:
            start = 0
        if end < start:
            end = start
        if end >= len(argList):
            end = len(argList) - 1
        # inclusive end -> slice uses end+1
        argList = argList[start : end + 1]
    logging.info(f"Loaded {len(argList)} tasks from {args.yaml}")
    
    # 使用进程池执行：改为使用 imap_unordered，按实际完成数递增进度计数
    output = []
    with Pool(processes=numProcesses) as pool:
        start_time = time.time()
        last_log_time = 0
        completed = 0
        total_tasks = len(argList)

        # imap_unordered 会随着任务完成逐个返回结果，我们可以据此统计已完成数量
        for res in pool.imap_unordered(run_wrapper, argList):
            output.append(res)
            completed += 1
            current_time = time.time()
            # 每分钟记录一次进度
            if current_time - last_log_time >= 60:
                elapsed = current_time - start_time
                with log_lock:
                    logging.info(f"Process: {completed}/{total_tasks}. Elapsed: {elapsed:.1f}秒")
                last_log_time = current_time
        
        # 检查是否有异常
        success_count = sum(1 for result in output if not isinstance(result, str) or not result.startswith(("Error:", "Timeout:")))
        timeout_count = sum(1 for result in output if isinstance(result, str) and result.startswith("Timeout:"))
        error_count = sum(1 for result in output if isinstance(result, str) and result.startswith("Error:") and not result.startswith("Timeout:"))
        
        with log_lock:
            logging.info(f"任务汇总: 成功 {success_count}，超时 {timeout_count}，错误 {error_count}")
            if timeout_count + error_count > 0:
                logging.warning(f"超时或失败的任务: {timeout_count + error_count}个")
                for result in output:
                    if isinstance(result, str) and result.startswith("Timeout:"):
                        logging.warning(f"超时任务: {result}")
                    elif isinstance(result, str) and result.startswith("Error:"):
                        logging.error(f"失败任务: {result}")


if __name__ == "__main__":
    main()
