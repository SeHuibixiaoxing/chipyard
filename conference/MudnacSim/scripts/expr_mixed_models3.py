import os
from multiprocessing import Pool, Lock
import subprocess
import argparse
import time
import signal
import logging
from datetime import datetime
from pathlib import Path
import sh  # type: ignore

try:
    import yaml  # type: ignore
except ImportError as e:
    raise SystemExit("PyYAML is required: pip install pyyaml") from e

BINARY_PATH = "./build/expr_mixed_models3"
OUTPUT_ROOT = "expr/output/expr_mixed_models3/script_output/"

dramChannels = [4,8]
noc_flit_sizes = [64]
method_list = ["ours"]

# 全局锁，用于安全写入日志文件
log_lock = Lock()

TASK_TIMEOUT = 180000 # 超时时间（s）

def init_logger():
    """初始化日志系统"""
    # 创建基于时间戳的日志文件名
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    log_filename = f"expr_mixed_models3_{timestamp}.log"
    
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
        bundle_index,
        model_csv,
        totalBatch,
        acc_nums,
        spm_bank_sizes,
        dramChannels,
        noc_flit_size,
    ) = args

    param_summary = (
        f"Index: {bundle_index}, Model: {model_csv}, TsotalBatch: {totalBatch}, Acc: {acc_nums}, "
        f"SPM Banks: {spm_bank_sizes}, DRAM: {dramChannels}, noc_flit_size: {noc_flit_size}"
    )

    with log_lock:
        logging.error(f"TIMEOUT ({timeout}s): {param_summary}")


def log_completion(args):
    """记录任务完成状态"""
    (
        bundle_index,
        model_csv,
        totalBatch,
        acc_nums,
        spm_bank_sizes,
        dramChannels,
        noc_flit_size,
    ) = args

    param_summary = (
        f"Index: {bundle_index}, Model: {model_csv}, TotalBatch: {totalBatch}, Acc: {acc_nums}, "
        f"SPM Banks: {spm_bank_sizes},DRAM: {dramChannels}, noc_flit_size: {noc_flit_size}"
    )

    with log_lock:
        logging.info(f"COMPLETED: {param_summary}")
def run_command(cmd, timeout, stdout_path=None, stderr_path=None):
    """使用第三方库 sh 执行命令：stderr 合并到 stdout，输出只写文件，不打印到控制台。
    返回(returncode, stdout_str, stderr_str). stderr_text为空（已合并）。
    """
    stdout_f = None
    stdout_lines: list[str] = []

    def _handle_out(line: str):
        stdout_lines.append(line)
        if stdout_f:
            stdout_f.write(line)

    try:
        if stdout_path:
            os.makedirs(os.path.dirname(stdout_path), exist_ok=True)
            stdout_f = open(stdout_path, "w", encoding="utf-8", buffering=1)

        cmd_runner = sh.bash
        result = cmd_runner(
            "-lc",
            cmd,
            _out=_handle_out,
            _err_to_out=True,  # merge stderr into stdout
            _timeout=timeout,
            _bg=False,
        )
        stdout_text = str(result)
        stderr_text = ""  # merged
        return 0, stdout_text, stderr_text
    except sh.TimeoutException:
        return -1, "".join(stdout_lines), f"命令超时 ({timeout} s)"
    except sh.ErrorReturnCode as e:
        if hasattr(e, "stdout") and e.stdout:
            stdout_lines.append(str(e.stdout))
        return e.exit_code, "".join(stdout_lines), ""
    except Exception as e:
        return -1, "".join(stdout_lines), str(e)
    finally:
        if stdout_f:
            stdout_f.close()
def run(bundle_index, model_csv, totalBatch, acc_nums, spm_bank_sizes, channels, noc_flit_size):
    acc_desc = ",".join(str(a) for a in acc_nums)
    spm_desc = ",".join(str(s) for s in spm_bank_sizes)
    # spm_bank_sizes are in KB (from analyze_speedups target), convert first entry to bytes for CLI.

    outputName = (
        f"idx-{bundle_index}_camdn_models-{model_csv}_batch-{totalBatch}_acc-{acc_desc}_"
        f"spm-{spm_desc}_noc-{noc_flit_size}_dram-{channels}"
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
        f"--total-batch {totalBatch} "
        f"--channels {channels} "
        f"--noc0-flit-size {noc_flit_size} "
        f"--noc1-flit-size {noc_flit_size} "
        f"--acc-num-list {acc_desc} "
        f"--spm-alloc-kb-list {spm_desc} "
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
    parser.add_argument("--limit", type=int, default=None, help="Optional limit on number of bundles to run")
    parser.add_argument(
        "--model-topk",
        type=str,
        default=None,
        help="Select top-K bundles per model without duplicates, format: model1:k1,model2:k2 (e.g., 'bertbase:3,gnmt:2')",
    )
    
    args = parser.parse_args()
    
    global TASK_TIMEOUT
    
    # 初始化日志系统
    log_filename = init_logger()
    logging.info(f"Starting simulation with {args.num_proc} processes.TASK_TIMEOUT: {TASK_TIMEOUT} s")
    logging.info(f"Log file: {os.path.abspath(log_filename)}")

    numProcesses = args.num_proc

    def parse_model_topk(spec: str | None):
        if not spec:
            return []
        pairs = []
        for part in spec.split(","):
            part = part.strip()
            if not part:
                continue
            if ":" not in part:
                continue
            name, k = part.split(":", 1)
            name = name.strip()
            try:
                k_val = int(k.strip())
            except Exception:
                continue
            if name and k_val > 0:
                pairs.append((name, k_val))
        return pairs

    def get_score(entry: dict) -> float:
        s = entry.get("score")
        if isinstance(s, (int, float)):
            return float(s)
        # Fallback: average available method avg_speedup
        vals = []
        for m in ("gemini", "tangram"):
            v = entry.get(m, {}).get("avg_speedup") if isinstance(entry.get(m), dict) else None
            if isinstance(v, (int, float)):
                vals.append(float(v))
        return sum(vals) / len(vals) if vals else 0.0

    def select_indices(data: list[dict], model_topk_pairs: list[tuple[str, int]]):
        if not model_topk_pairs:
            # No filtering; keep all indices
            return {d.get("index") for d in data}

        # Build per-model candidate lists sorted by score desc
        name_lists: dict[str, list[tuple[float, int]]] = {}
        for entry in data:
            idx = entry.get("index")
            model_csv = entry.get("model_name", "") or ""
            models = [m.strip().lower() for m in model_csv.split(",") if m.strip()]
            sc = get_score(entry)
            for target_name, _ in model_topk_pairs:
                tn = target_name.strip().lower()
                if tn in models:
                    name_lists.setdefault(tn, []).append((sc, idx))

        for key in list(name_lists.keys()):
            name_lists[key].sort(key=lambda x: x[0], reverse=True)

        selected: set[int] = set()
        for target_name, k in model_topk_pairs:
            tn = target_name.strip().lower()
            cands = name_lists.get(tn, [])
            picked = 0
            for sc, idx in cands:
                if idx in selected:
                    continue
                selected.add(idx)
                picked += 1
                if picked >= k:
                    break
        return selected

    def load_tasks(yaml_path: Path, model_topk_pairs: list[tuple[str, int]]):
        data = yaml.safe_load(yaml_path.read_text())
        if not data:
            return []
        selected_indices = select_indices(data, model_topk_pairs)
        tasks = []
        for entry in data:
            if selected_indices and entry.get("index") not in selected_indices:
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
            }

            for method in method_list:
                mapping_list = mappings_by_method.get(method, [])
                if not mapping_list:
                    continue
                mapping_csv = ",".join(mapping_list)
                for dram_channel in dramChannels:
                    for flit in noc_flit_sizes:
                        tasks.append(
                            (
                                idx,
                                model_csv,
                                total_batch,
                                acc_nums,
                                spm_bank_sizes,
                                dram_channel,
                                flit,
                            )
                        )
        return tasks

    model_topk_pairs = parse_model_topk(args.model_topk)
    if model_topk_pairs:
        logging.info(f"Model-TopK selection requested: {model_topk_pairs}")
    argList = load_tasks(args.yaml, model_topk_pairs)
    if args.limit is not None:
        argList = argList[: args.limit]
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
