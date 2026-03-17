#!/usr/bin/env python3
"""
Parse qos pipeline logs and compute SLA, STP, and fairness metrics over task prefixes.

Inputs: log files under expr/output/expr_qos_pipeline/mudnacsim/ with names like:
  qos_channel-8_acc64_thread4_acc-increase-factor1.0_benchmark0_tasks30_scale1.2_spm1024_allbank_hilbert_noc-64_batch16_part3qos4_ours

Outputs: CSV with one row per log, including thread_num, acc_increase_factor, task_num, scale, qos,
SLA satisfaction counts/rates for prefixes, STP sums, and fairness values.
"""

from __future__ import annotations

import argparse
import csv
import re
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional

DEFAULT_DIRS = [
    Path("expr/output/expr_qos_pipeline/mudnacsim"),
    Path("expr/output/expr_qos2/mudnacsim"),
]
PREFIXES = [10, 15, 20, 25, 30]
TASK_WHITELIST = {"30"}  # allowed task_num values when filtering is enabled

MODEL_TO_IDX = {
    "resnet50": 0,
    "mobilenetv2": 1,
    "effinetb0": 2,
    "vitb16": 3,
    "pointpillars": 4,
    "w2v2base": 5,
    "bertbase": 6,
    "gnmt": 7,
}
IDX_TO_MODEL = {v: k for k, v in MODEL_TO_IDX.items()}

# Filename parser
FILE_RE = re.compile(
    r"^qos_channel-(?P<channel>[^_]+)_acc(?P<acc>[^_]+)_thread(?P<thread>[^_]+)_"
    r"acc-increase-factor(?P<aif>[^_]+)_benchmark(?P<bench>[^_]+)_tasks(?P<tasks>[^_]+)_"
    r"scale(?P<scale>[^_]+)_spm(?P<spm>[^_]+)_(?P<pipe_spm>[^_]+)_(?P<pipe_acc>[^_]+)_"
    r"noc-(?P<noc>[^_]+)_batch(?P<batch>[^_]+)_part(?P<part>[^_]+)qos(?P<qos>[^_]+)_"
    r"(?P<method>.+)$"
)

# QoS2 format: no method/pipe strategies/part; method defaults to camdn
FILE_RE_QOS2 = re.compile(
    r"^qos_channel-(?P<channel>[^_]+)_acc(?P<acc>[^_]+)_thread(?P<thread>[^_]+)_"
    r"benchmark(?P<bench>[^_]+)_tasks(?P<tasks>[^_]+)_scale(?P<scale>[^_]+)_"
    r"spm(?P<spm>[^_]+)_noc-(?P<noc>[^_]+)_batch(?P<batch>[^_]+)_qos(?P<qos>[^_]+)$"
)

TASK_RE = re.compile(r"^Task:\s*(?P<idx>\d+)")
SLACK_RE = re.compile(r"Slack:\s*([\-\d]+)")
EXEC_RE = re.compile(r"Execute Time:\s*\[(?P<body>[^\]]*)\]")
EST_TASK_RE = re.compile(r"Est Time per subgraph \(task\):\s*\[(?P<body>[^\]]*)\]")
WORKLOAD_RE = re.compile(r"Workload Type:\s*(?P<type>\d+)")
START_FINISH_RE = re.compile(r"Start to Finish:\s*(?P<stf>[\d\.]+)")


@dataclass
class TaskMetrics:
    slack: Optional[int]
    np: Optional[float]


def _parse_array(text: str) -> List[float]:
    parts = [p.strip() for p in text.split(',') if p.strip()]
    out: List[float] = []
    for p in parts:
        try:
            out.append(float(p))
        except ValueError:
            pass
    return out


def parse_tasks(content: str) -> List[TaskMetrics]:
    tasks: List[TaskMetrics] = []
    current: dict[str, Optional[float]] = {"slack": None, "exec_sum": None, "est_sum": None}

    def flush():
        if current["slack"] is None and current["exec_sum"] is None and current["est_sum"] is None:
            return
        np_val: Optional[float] = None
        if current["exec_sum"] is not None and current["est_sum"]:
            if current["est_sum"] > 0:
                np_val = current["est_sum"] / current["exec_sum"]
        tasks.append(TaskMetrics(slack=current["slack"], np=np_val))
        current["slack"] = None
        current["exec_sum"] = None
        current["est_sum"] = None

    for line in content.splitlines():
        line = line.strip()
        if not line:
            continue
        if TASK_RE.match(line):
            flush()
            continue
        m = SLACK_RE.search(line)
        if m:
            try:
                current["slack"] = float(m.group(1))
            except ValueError:
                current["slack"] = None
            continue
        m = EXEC_RE.search(line)
        if m:
            arr = _parse_array(m.group("body"))
            if arr:
                current["exec_sum"] = sum(arr)
            continue
        m = EST_TASK_RE.search(line)
        if m:
            arr = _parse_array(m.group("body"))
            if arr:
                current["est_sum"] = sum(arr)
            continue
    flush()
    return tasks


def load_profile_times(profiling_root: Path) -> dict[str, float]:
    results: dict[str, float] = {}
    for model, _idx in MODEL_TO_IDX.items():
        p = profiling_root / model
        if not p.exists():
            continue
        try:
            text = p.read_text(encoding="utf-8", errors="replace")
        except Exception:
            continue
        total = 0.0
        for line in text.splitlines():
            parts = [x.strip() for x in line.split(',') if x.strip()]
            if len(parts) < 5:
                continue
            try:
                val = float(parts[4])
            except ValueError:
                continue
            if val != 0:
                total += val
        if total > 0:
            results[model] = total
    return results


def parse_tasks_qos2(content: str, profile_time_map: dict[str, float]) -> List[TaskMetrics]:
    tasks: List[TaskMetrics] = []
    current: dict[str, Optional[float]] = {"slack": None, "work_type": None, "stf": None}

    def flush():
        if current["slack"] is None and current["work_type"] is None and current["stf"] is None:
            return
        np_val: Optional[float] = None
        wt = current["work_type"]
        stf = current["stf"]
        if wt is not None and stf is not None and stf > 0:
            model = IDX_TO_MODEL.get(int(wt))
            if model:
                pred = profile_time_map.get(model)
                if pred is not None:
                    np_val = stf / pred
        tasks.append(TaskMetrics(slack=current["slack"], np=np_val))
        current["slack"] = None
        current["work_type"] = None
        current["stf"] = None

    for line in content.splitlines():
        line = line.strip()
        if not line:
            continue
        if TASK_RE.match(line):
            flush()
            continue
        m = WORKLOAD_RE.search(line)
        if m:
            try:
                current["work_type"] = int(m.group("type"))
            except ValueError:
                current["work_type"] = None
            continue
        m = START_FINISH_RE.search(line)
        if m:
            try:
                current["stf"] = float(m.group("stf"))
            except ValueError:
                current["stf"] = None
            continue
        m = SLACK_RE.search(line)
        if m:
            try:
                current["slack"] = float(m.group(1))
            except ValueError:
                current["slack"] = None
            continue
    flush()
    return tasks


def compute_prefix_metrics(tasks: List[TaskMetrics], n: int):
    subset = tasks[:n]
    if not subset:
        return {"sla_count": "", "sla_rate": "", "stp": "", "fairness": ""}

    valid_tasks = [t for t in subset if t.slack is not None]
    sla_count = sum(1 for t in valid_tasks if t.slack is not None and t.slack > 0)
    total_sla = len(valid_tasks) if valid_tasks else len(subset)
    sla_rate = sla_count / total_sla if total_sla else 0.0

    np_values = [t.np for t in subset if t.np is not None]
    stp = sum(np_values) if np_values else None

    fairness = None
    if len(np_values) >= 2:
        min_np = min(np_values)
        max_np = max(np_values)
        if max_np > 0:
            fairness = min_np / max_np

    return {
        "sla_count": sla_count if valid_tasks else "",
        "sla_rate": f"{sla_rate:.4f}" if valid_tasks else "",
        "stp": f"{stp:.4f}" if stp is not None else "",
        "fairness": f"{fairness:.4f}" if fairness is not None else "",
    }


def parse_filename(path: Path):
    m = FILE_RE.match(path.name)
    if m:
        g = m.groupdict()
        return {
            "thread_num": g.get("thread", ""),
            "acc_increase_factor": g.get("aif", ""),
            "task_num": g.get("tasks", ""),
            "scale": g.get("scale", ""),
            "qos": g.get("qos", ""),
            "method": g.get("method", ""),
            "type": "qos_pipeline",
        }

    m = FILE_RE_QOS2.match(path.name)
    if m:
        g = m.groupdict()
        return {
            "thread_num": g.get("thread", ""),
            "acc_increase_factor": "",  # not present
            "task_num": g.get("tasks", ""),
            "scale": g.get("scale", ""),
            "qos": g.get("qos", ""),
            "method": "camdn",
            "type": "qos2",
        }
    return None


def process_file(path: Path, profile_time_map: dict[str, float]):
    meta = parse_filename(path)
    if not meta:
        return None
    try:
        content = path.read_text(encoding="utf-8", errors="replace")
    except Exception:
        return None
    if meta.get("type") == "qos2":
        tasks = parse_tasks_qos2(content, profile_time_map)
    else:
        tasks = parse_tasks(content)
    row: list[str] = [
        path.name,
        meta["thread_num"],
        meta["acc_increase_factor"],
        meta["task_num"],
        meta["scale"],
        meta["qos"],
        meta["method"],
    ]

    for n in PREFIXES:
        metrics = compute_prefix_metrics(tasks, n)
        row.append(str(metrics["sla_count"]))
        row.append(str(metrics["sla_rate"]))
        row.append(str(metrics["stp"]))
        row.append(str(metrics["fairness"]))
    return row


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, default=Path("qos_summary.csv"), help="Output CSV path")
    parser.add_argument(
        "--profiling-dir",
        type=Path,
        default=Path("profiling"),
        help="Directory containing profiling data per model (used for qos2 logs)",
    )
    parser.add_argument(
        "--exclude-ours-thread4",
        action="store_true",
        help="If set, skip rows where method=ours and thread_num=4",
    )
    args = parser.parse_args()

    headers = [
        "file",
        "thread_num",
        "acc_increase_factor",
        "task_num",
        "scale",
        "qos",
        "method",
    ]
    for n in PREFIXES:
        headers.extend([f"sla_count_{n}", f"sla_rate_{n}", f"stp_{n}", f"fairness_{n}"])

    profile_time_map = load_profile_times(args.profiling_dir)

    rows: List[List[str]] = []
    for directory in DEFAULT_DIRS:
        if not directory.exists():
            continue
        for path in sorted(directory.iterdir()):
            if not path.is_file():
                continue
            res = process_file(path, profile_time_map)
            if res:
                # res layout: [file, thread_num, acc_increase_factor, task_num, scale, qos, method, ...]
                if args.exclude_ours_thread4 and len(res) >= 7:
                    thread_val = res[1]
                    method_val = res[6].lower()
                    if thread_val == "4" and method_val == "ours":
                        continue
                if len(res) >= 4:
                    task_val = res[3]
                    if task_val not in TASK_WHITELIST:
                        continue
                rows.append(res)

    with args.out.open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(headers)
        writer.writerows(rows)
    print(f"Wrote {len(rows)} rows to {args.out}")


if __name__ == "__main__":
    main()
