import os
from multiprocessing import Pool, Lock
import subprocess
import argparse
import time
import signal
import logging
from datetime import datetime

BINARY_PATH = "./build/expr_single_pipeline"
OUTPUT_ROOT = "expr/output/expr_single_pipeline/script_output/"

modelList = [
    "resnet18",
]

# caseList = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14]
caseList = [11,12,13,14]

# pipeSpmStrategyList = ["allbank", "mindis"]
pipeSpmStrategyList = ["allbank"]

# pipe_acc_strategy_list = ["hilbert", "free"]
pipe_acc_strategy_list = ["hilbert"]

# per-model accelerator counts
accList = {
    "resnet18": [64],
}

spmBankList = [1] # MB

# batch_size = [
#     1,
#     4,
#     8,
#     16,
#     32,
#     64
# ]
batch_size = [1,2,4]

dramChannels = [1]

noc_list = [
    "HomoTileMesh" # 不再使用none noc
]

# noc_flit_sizes = [4, 8, 16, 32, 64, 128]
noc_flit_sizes = [64]

# debug_flags = ["PIPELINE","STAGE","THREAD_PIPELINE"]
debug_flags = ["PIPELINE"]
# debug_flags = ["NONE"]

# 全局锁，用于安全写入日志文件
log_lock = Lock()

TASK_TIMEOUT = 18000 # 超时时间（s）

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
    # 解包参数
    system, modelName, totalBatch, acc_num, spm_bank_size, mappingPath, caseNum, pipeStrategy, pipeAccStrategy, noc, dramChannels, noc_flit_size, debug_flags = args
    
    # 创建参数摘要
    param_summary = (
        f"Model: {modelName}, TotalBatch: {totalBatch}, Acc: {acc_num}, SPM Banks: {spm_bank_size}, Case {caseNum}, "
        f"Mapping: {mappingPath}, Strategy: {pipeStrategy}, AccStrategy: {pipeAccStrategy}, "
        f"NoC: {noc}, DRAM: {dramChannels}, noc_flit_size: {noc_flit_size}"
    )
    
    # 安全写入日志
    with log_lock:
        logging.error(f"TIMEOUT ({timeout}s): {param_summary}")


def log_completion(args):
    """记录任务完成状态"""
    # 解包参数
    system, modelName, totalBatch, acc_num, spm_bank_size, mappingPath, caseNum, pipeStrategy, pipeAccStrategy, noc, dramChannels, noc_flit_size, debug_flags = args
    
    # 创建参数摘要
    param_summary = (
        f"Model: {modelName}, TotalBatch: {totalBatch}, Acc: {acc_num}, SPM Banks: {spm_bank_size}, Case {caseNum}, Mapping: {mappingPath}, "
        f"Strategy: {pipeStrategy}, AccStrategy: {pipeAccStrategy}, NoC: {noc}, DRAM: {dramChannels}, "
        f"noc_flit_size: {noc_flit_size}"
    )
    
    # 安全写入日志
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
def run(system, modelName, totalBatch, acc_num, spm_bank_size, mappingPath, caseId, pipeSpmStrategy, pipeAccStrategy, noc, channels, noc_flit_size, debug_flags):
    # Build a concise output name
    outputName = f"{modelName}_{system}_dram-{channels}_noc-{noc}-{noc_flit_size}_{acc_num}acc_{spm_bank_size*acc_num}MB_{pipeSpmStrategy}_{pipeAccStrategy}_batch-{totalBatch}_case{caseId}"

    outputRoot = f"{OUTPUT_ROOT}"
    cmd = (
        f"{BINARY_PATH} "
        f"--system {system} "
        f"--model {modelName} "
        f"--output {outputName} "
        f"--output-root {outputRoot} "
        f"--spm-addr-type conti "
        f"--accels {acc_num} "
        f"--spm-size-per-bank {spm_bank_size} "
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
    # prepare stdout/stderr file paths and run
    stdout_path = os.path.join(outputRoot, f"{outputName}-std.log")
    stderr_path = os.path.join(outputRoot, f"{outputName}-err.log")
    start_time = time.time()
    returncode, stdout_text, stderr_text = run_command(cmd, TASK_TIMEOUT, stdout_path=stdout_path, stderr_path=stderr_path)
    elapsed_time = time.time() - start_time

    if returncode != 0:
        error_msg = f"命令失败 ({elapsed_time:.1f} s, 退出码: {returncode})"
        if isinstance(stderr_text, str) and "TimeoutError" in stderr_text:
            error_msg = f"超时终止 ({TASK_TIMEOUT} s)"
        # include stderr in exception
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
    parser.add_argument("--timeout", type=int, default=1800, 
                        help=f"单个任务的超时时间（秒），默认 {1800} s")
    
    args = parser.parse_args()
    
    global TASK_TIMEOUT
    TASK_TIMEOUT = args.timeout
    
    # 初始化日志系统
    log_filename = init_logger()
    logging.info(f"Starting simulation with {args.num_proc} processes.TASK_TIMEOUT: {TASK_TIMEOUT} s")
    logging.info(f"Log file: {os.path.abspath(log_filename)}")

    system = "spm"
    numProcesses = args.num_proc

    argList = []
    for noc in noc_list:
        for dram_channel in dramChannels:
            for noc_flit_size in noc_flit_sizes:
                for modelName in modelList:
                    for acc_num in accList.get(modelName, [64]):
                        for spm_bank_size in spmBankList:
                            for case_num in caseList:
                                mappingPath = f"models/pipeline/{modelName}-test/case{case_num}.yaml"
                                for pipeStrategy in pipeSpmStrategyList:
                                    for pipeAcc in pipe_acc_strategy_list:
                                        for totalBatch in batch_size:
                                            argList.append((system, modelName, totalBatch, acc_num, spm_bank_size, mappingPath, case_num, pipeStrategy, pipeAcc, noc, dram_channel, noc_flit_size, debug_flags))
    logging.info(f"Total tasks to execute: {len(argList)}")
    
    # 使用进程池执行
    with Pool(processes=numProcesses) as pool:
        # 使用 map_async 配合回调
        results = pool.map_async(run_wrapper, argList)
        
        # 等待所有进程完成
        start_time = time.time()
        last_log_time = 0
        while not results.ready():
            completed = len(argList) - results._number_left
            current_time = time.time()
            
            # 每分钟记录一次进度
            if current_time - last_log_time >= 60:
                elapsed = current_time - start_time
                with log_lock:
                    logging.info(f"Process: {completed}/{len(argList)}. Elapsed: {elapsed:.1f}秒")
                last_log_time = current_time
            
            # 检查超时任务
            # 注意：multiprocessing.Pool没有直接的超时检测机制，我们依赖子进程中的处理
            time.sleep(60)  # 每60秒检查一次进度
        
        # 获取所有结果
        output = []
        try:
            output = results.get()
        except Exception as e:
            logging.error(f"进程池错误: {str(e)}")
            output = []
        
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
