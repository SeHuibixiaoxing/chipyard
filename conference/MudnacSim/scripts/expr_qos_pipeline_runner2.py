#!/usr/bin/env python3
"""
Run expr_qos_pipeline binary across a grid of candidate parameters and
store outputs under `expr/output/expr_qos_pipeline/`.

Usage:
    python3 scripts/expr_qos_pipeline_runner.py --bin ./build/expr_qos_pipeline --num-proc 4

The script contains sensible default candidate lists; adjust them as needed.
"""

import argparse
import itertools
import multiprocessing as mp
import os
import subprocess
import sys
import time
import csv
from pathlib import Path

BIN_PATH = './build/expr_qos_pipeline'

statc_available_threads = [4,8,16,32]

model_to_idx = {
    "resnet50": 0,
    "mobilenetv2": 1,
    "effinetb0": 2,
    "vitb16": 3,
    "pointpillars": 4,
    "w2v2base": 5,
    "bertbase": 6,
    "gnmt": 7
}

qos_per_model =  [
        # 0
        {
            "resnet50": 160,
            "mobilenetv2": 352,
            "effinetb0": 352,
            "vitb16": 32,
            "pointpillars": 32,
            "w2v2base": 64,
            "bertbase": 32,
            "gnmt": 160
        },
        # 1
        {
            "resnet50": 160,
            "mobilenetv2": 352,
            "effinetb0": 352,
            "vitb16": 32,
            "pointpillars": 32,
            "w2v2base": 64,
            "bertbase": 32,
            "gnmt": 160
        },
        #2
        {
            "resnet50": 160,
            "mobilenetv2": 352,
            "effinetb0": 352,
            "vitb16": 48,
            "pointpillars": 48,
            "w2v2base": 64,
            "bertbase": 40,
            "gnmt": 160
        },
        #3
        {
            "resnet50": 160,
            "mobilenetv2": 352,
            "effinetb0": 352,
            "vitb16": 80,
            "pointpillars": 80,
            "w2v2base": 80,
            "bertbase": 80,
            "gnmt": 160
        },
        #4
        {
            "resnet50": 160,
            "mobilenetv2": 352,
            "effinetb0": 352,
            "vitb16": 120,
            "pointpillars": 120,
            "w2v2base": 120,
            "bertbase": 120,
            "gnmt": 160
        },
        #5
        {
            "resnet50": 120,
            "mobilenetv2": 320,
            "effinetb0": 320,
            "vitb16": 160,
            "pointpillars": 160,
            "w2v2base": 120,
            "bertbase": 120,
            "gnmt": 120
        },
        #6
        {
            "resnet50": 120,
            "mobilenetv2": 300,
            "effinetb0": 300,
            "vitb16": 140,
            "pointpillars": 120,
            "w2v2base": 60,
            "bertbase": 160,
            "gnmt": 120
        }
    ]

# Candidate parameter lists (edit as needed)
CANDIDATES = {
    'channels': [8],
    # 'channels': [4],
    
    'accels': [64],
    
    # 'threads': [4,6],
    'threads': [6],
    
    'benchmark': [0],
    
    'tasks': [30],
    # 'tasks': [10],
    
    # 'target_scale': [1.0],
    'target_scale': [0.8,1.0,1.2],
    
    'acc-increase-factor': [1.0],
    # 'acc-increase-factor': [1.0,1.2],
    
    'spm_size_per_bank_kb': [1024],
    
    # 'pipe_spm_strategy': ['allbank', 'mindis'],
    'pipe_spm_strategy': ['allbank'],
    
    # 'pipe_acc_strategy': ['hilbert', 'free'],
    'pipe_acc_strategy': ['hilbert'],
    
    'noc_flit_size': [64],
    
    # 'batch_size': [8,16],
    'batch_size': [16],
    
    'timeout_threshold': [0.1],
    
    'partition_num': [3],
    
    'method': ['ours'],
    # 'method': ['ours', 'gemini', 'tangram'],
    
    'debug_flags': ['THREAD_QOS_PIPELINE,THREAD_RUN_PIPELINE'],
    
    # 'qps_per_model': [0, 1, 2, 3],
    'qps_per_model': [1,2,3,4,5,6],
}

OUTPUT_ROOT = Path('expr/output/expr_qos_pipeline')
SCRIPT_OUTPUT_DIR = OUTPUT_ROOT / 'script_output'
CSV_SUMMARY = OUTPUT_ROOT / 'runs_summary.csv'


def make_output_name(params: dict) -> str:
    # Build a concise output name from params
    parts = [
        f"channel-{params['channels']}",
        f"acc{params['accels']}",
        f"thread{params['threads']}",
        f"acc-increase-factor{params['acc-increase-factor']}",
        f"benchmark{params['benchmark']}",
        f"tasks{params['tasks']}",
        f"scale{params['target_scale']}",
        f"spm{params['spm_size_per_bank_kb']}",
        f"{params['pipe_spm_strategy']}",
        f"{params['pipe_acc_strategy']}",
        f"noc-{params['noc_flit_size']}",
        f"batch{params['batch_size']}",
        f"part{params['partition_num']}"
        f"qos{params['qps_per_model']}",
        f"{params['method']}",
    ]
    name = "qos_" + "_".join(parts)
    # keep name length reasonable
    return name


def build_cmd(bin_path: str, params: dict, output_name: str, output_root: str) -> list:
    cmd = [str(bin_path)]
    cmd += ['--channels', str(params['channels'])]
    cmd += ['--accels', str(params['accels'])]
    cmd += ['--threads', str(params['threads'])]
    cmd += ['--benchmark', str(params['benchmark'])]
    cmd += ['--tasks', str(params['tasks'])]
    cmd += ['--target-scale', str(params['target_scale'])]
    cmd += ['--output', output_name]
    cmd += ['--spm-size-per-bank', str(params['spm_size_per_bank_kb'])]
    cmd += ['--pipe-spm-strategy', params['pipe_spm_strategy']]
    cmd += ['--pipe-acc-strategy', params['pipe_acc_strategy']]
    cmd += ['--noc-flit-size', str(params['noc_flit_size'])]
    cmd += ['--batch-size', str(params['batch_size'])]
    cmd += ['--timeout-threshold', str(params['timeout_threshold'])]
    cmd += ['--partition-num', str(params['partition_num'])]
    cmd += ['--output-root', output_root]
    cmd += ['--method', params['method']]
    cmd += ['--debug-flags', params['debug_flags']]
    # qps-per-model, 按照idx排序
    cmd += ['--qps-per-model', ','.join(str(qos_per_model[params['qps_per_model']][model]) for model in sorted(model_to_idx.keys(), key=lambda x: model_to_idx[x]))]
    cmd += ['--acc-increase-factor', str(params['acc-increase-factor'])]
    cmd += ['--schedule-acc-alloc-method', 'dynamic_base' if params['method'] == 'ours' else 'static_fixed']
    
    return cmd


def run_one(task):
    bin_path, params, output_root = task
    output_name = make_output_name(params)
    script_out_dir = SCRIPT_OUTPUT_DIR
    script_out_dir.mkdir(parents=True, exist_ok=True)
    log_path = script_out_dir / (output_name + '.log')

    cmd = build_cmd(bin_path, params, output_name, str(output_root) + '/')
    cmd_str = ' '.join(cmd)
    start = time.time()
    ret = {
        'output_name': output_name,
        'params': params,
        'returncode': None,
        'duration_s': None,
        'log': str(log_path),
        'cmd': cmd_str,
    }
    # Run process and stream output to log file in real-time
    try:
        with open(log_path, 'w', encoding='utf-8', buffering=1) as lf:
            lf.write(f"# CMD: {cmd_str}\n")
            lf.flush()
            proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=1, universal_newlines=True)
            try:
                # Stream output line by line
                for line in proc.stdout:
                    lf.write(line)
                    lf.flush()
            except Exception as e:
                lf.write(f"# Exception while streaming output: {e}\n")
                lf.flush()
            proc.stdout.close()
            ret['returncode'] = proc.wait()
    except Exception as e:
        # Could not start process or open log file
        try:
            with open(log_path, 'a', encoding='utf-8') as lf:
                lf.write(f"# Exception when running: {e}\n")
        except Exception:
            pass
        ret['returncode'] = -1
        ret['error'] = str(e)
    ret['duration_s'] = time.time() - start
    return ret


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--num-proc', type=int, default=4, help='number of parallel processes')
    args = parser.parse_args()

    bin_path = Path(BIN_PATH)
    if not bin_path.exists():
        print('Binary not found:', bin_path)
        sys.exit(1)

    OUTPUT_ROOT.mkdir(parents=True, exist_ok=True)
    SCRIPT_OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

    # build parameter grid
    keys = list(CANDIDATES.keys())
    values = [CANDIDATES[k] for k in keys]
    combos = list(itertools.product(*values))

    tasks = []
    for combo in combos:
        params = dict(zip(keys, combo))
        if params['method'] != 'ours' and params['threads'] not in statc_available_threads:
            print('Skipping invalid config:', params)
            continue
        tasks.append((str(bin_path), params, OUTPUT_ROOT))

    total_tasks = len(tasks)
    print('Starting {} runs using {} processes'.format(total_tasks, args.num_proc))

    results = []
    with mp.Pool(processes=args.num_proc) as pool:
        completed = 0
        start_time = time.time()
        last_log = 0
        for res in pool.imap_unordered(run_one, tasks):
            results.append(res)
            completed += 1
            now = time.time()
            if now - last_log >= 10 or completed == total_tasks:
                elapsed = now - start_time
                print(f"Progress: {completed}/{total_tasks} finished, elapsed {elapsed:.1f}s")
                last_log = now

    # write summary CSV
    with open(CSV_SUMMARY, 'w', newline='') as csvf:
        w = csv.writer(csvf)
        header = ['output_name', 'returncode', 'duration_s'] + list(keys) + ['log', 'cmd']
        w.writerow(header)
        for r in results:
            row = [r['output_name'], r['returncode'], r['duration_s']]
            for k in keys:
                row.append(r['params'][k])
            row.append(r['log'])
            row.append(r.get('cmd', ''))
            w.writerow(row)

    # print failed commands for quick inspection
    failed = [r for r in results if r.get('returncode', 0) != 0]
    if failed:
        print(f"\nFailed runs ({len(failed)}):")
        for r in failed:
            print(f"rc={r.get('returncode')}, cmd={r.get('cmd')}, log={r.get('log')}")

    print('All runs finished. Summary written to', CSV_SUMMARY)

if __name__ == '__main__':
    main()
