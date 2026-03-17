#!/usr/bin/env python3
"""Aggregate schedule point profiling results into a single YAML per method.

This script walks `output/pipeline/<model>/schedule_point/pipeline_mapping/<partition>/<seg>/`,
reads `profiline.yaml` files, and consolidates them into
`output/pipeline/schedule_point_profiling_<method>.yaml`.
"""
import argparse
import os
from collections import defaultdict
from typing import Any, Dict, List, Optional, Set, Tuple

import yaml

from HybridMapper.CustomYamlDumper import CustomYamlDumper

OUTPUT_DIR = "output/pipeline/"

model_name_to_idx = {
    "resnet50": 0,
    "mobilenetv2": 1,
    "effinetb0": 2,
    "vitb16": 3,
    "pointpillars": 4,
    "w2v2base": 5,
    "bertbase": 6,
    "gnmt": 7,
    "berttiny": 8,
    "bertmini": 9,
    "bertsmall": 10,
    "bertmedium": 11,
    "resnet18": 12,
    "vitsmall16": 13,
}

# Type aliases for clarity
ProfilineEntry = Dict[str, Any]
MethodRecords = Dict[str, Dict[Tuple[str, int, int, int, int, int], Dict[str, Any]]]


def _parse_models_arg(raw: str) -> Optional[Set[str]]:
    if not raw:
        return None
    return {m.strip() for m in raw.split(',') if m.strip()}


def _parse_mapping_filename(path: str) -> Tuple[int, int, int, int, int, str]:
    """Parse `{acc}_{spm}_{batch}_{dram}_{noc}_{method}.yaml` from mapping path."""
    base = os.path.basename(path)
    if not base.endswith(".yaml"):
        raise ValueError(f"Mapping filename missing .yaml suffix: {base}")

    parts = base[:-5].split('_')
    if len(parts) < 6:
        raise ValueError(f"Mapping filename not in expected format: {base}")

    try:
        acc = int(parts[0])
        spm_per_acc_kb = int(parts[1])
        batch = int(parts[2])
        dram_bw = int(parts[3])
        noc_bw = int(parts[4])
    except ValueError as exc:
        raise ValueError(f"Mapping filename has non-integer fields: {base}") from exc

    method = '_'.join(parts[5:])
    return acc, spm_per_acc_kb, batch, dram_bw, noc_bw, method


def _load_profiline(profiline_path: str) -> List[ProfilineEntry]:
    with open(profiline_path, 'r') as f:
        data = yaml.safe_load(f)
    if data is None:
        return []
    if not isinstance(data, list):
        raise ValueError(f"Unexpected profiline format in {profiline_path}")
    return data


def _collect_records(output_dir: str, model_filter: Optional[Set[str]], profiline_name: str) -> MethodRecords:
    records_by_method: MethodRecords = defaultdict(dict)

    for model_name in sorted(os.listdir(output_dir)):
        model_dir = os.path.join(output_dir, model_name)
        if not os.path.isdir(model_dir):
            continue
        if model_filter is not None and model_name not in model_filter:
            continue

        pipeline_mapping_root = os.path.join(model_dir, 'schedule_point', 'pipeline_mapping')
        if not os.path.isdir(pipeline_mapping_root):
            continue

        for partition_name in sorted(os.listdir(pipeline_mapping_root)):
            partition_path = os.path.join(pipeline_mapping_root, partition_name)
            if not os.path.isdir(partition_path):
                continue
            try:
                partition_num = int(partition_name)
            except ValueError:
                continue

            for seg_name in sorted(os.listdir(partition_path)):
                seg_path = os.path.join(partition_path, seg_name)
                if not os.path.isdir(seg_path):
                    continue
                try:
                    seg_idx = int(seg_name)
                except ValueError:
                    continue

                profiline_path = os.path.join(seg_path, profiline_name)
                if not os.path.isfile(profiline_path):
                    continue

                entries = _load_profiline(profiline_path)
                for entry in entries:
                    if not isinstance(entry, dict):
                        continue
                    cost = entry.get('cost')
                    mapping_path = entry.get('mapping_path')
                    if cost is None or mapping_path is None:
                        continue

                    try:
                        acc, spm_per_acc_kb, batch, dram_bw, noc_bw, method = _parse_mapping_filename(mapping_path)
                    except ValueError as exc:
                        print(f"Skip entry in {profiline_path}: {exc}")
                        continue

                    key_without_method = (
                        model_name,
                        partition_num,
                        acc,
                        spm_per_acc_kb,
                        batch,
                        dram_bw,
                        noc_bw,
                    )
                    bucket = records_by_method[method].setdefault(
                        key_without_method,
                        {
                            'model_name': model_name,
                            'model_idx': model_name_to_idx.get(model_name, -1),
                            'partition_num': partition_num,
                            'acc': acc,
                            'spm_per_acc_kb': spm_per_acc_kb,
                            'batch': batch,
                            'dram_bw': dram_bw,
                            'noc_bw': noc_bw,
                            'cost_by_seg': {},
                            'mapping_by_seg': {},
                        },
                    )

                    # Keep the lowest cost per segment if duplicates show up.
                    prev_cost = bucket['cost_by_seg'].get(seg_idx)
                    if prev_cost is None or cost < prev_cost:
                        bucket['cost_by_seg'][seg_idx] = cost
                        bucket['mapping_by_seg'][seg_idx] = mapping_path

    return records_by_method


def _build_output_entries(records_by_method: MethodRecords) -> Dict[str, List[Dict[str, Any]]]:
    output: Dict[str, List[Dict[str, Any]]] = {}

    for method, records in records_by_method.items():
        entries: List[Dict[str, Any]] = []
        for key, bucket in records.items():
            partition_num = bucket['partition_num']
            expected_segs = set(range(partition_num))
            present_segs = set(bucket['mapping_by_seg'])
            if expected_segs - present_segs:
                missing = sorted(expected_segs - present_segs)
                print(f"bucket: {bucket}")
                print(
                    f"Skip incomplete record for model={bucket['model_name']} partition_num={partition_num} "
                    f"missing segments={missing}"
                )
                continue

            mapping_paths = [bucket['mapping_by_seg'][idx] for idx in range(partition_num)]
            costs = [bucket['cost_by_seg'][idx] for idx in range(partition_num)]

            entries.append(
                {
                    'cost': costs,
                    'target': {
                        'model_name': bucket['model_name'],
                        'model_idx': bucket['model_idx'],
                        'acc': bucket['acc'],
                        'spm_per_acc_kb': bucket['spm_per_acc_kb'],
                        'batch': bucket['batch'],
                        'dram_bw': bucket['dram_bw'],
                        'noc_bw': bucket['noc_bw'],
                        'partition_num': partition_num,
                    },
                    'mapping_path': mapping_paths,
                }
            )
        output[method] = entries
    return output


def _build_qps_entries(output_entries: Dict[str, List[Dict[str, Any]]]) -> Dict[str, List[Dict[str, Any]]]:
    """Compute theoretical QPS for each entry.

    QPS = 1e9 / sum(cost_by_seg) * batch, where cost is in ns.
    """
    qps_entries: Dict[str, List[Dict[str, Any]]] = {}
    for method, entries in output_entries.items():
        method_list: List[Dict[str, Any]] = []
        for entry in entries:
            costs = entry.get('cost', [])
            target = entry.get('target', {})
            batch = target.get('batch', 0)
            total_cost_ns = sum(costs) if costs else 0
            qps = 0.0
            if total_cost_ns > 0:
                qps = 1e9 / total_cost_ns * batch

            method_list.append(
                {
                    'qps': int(qps),
                    'total_cost_ns': total_cost_ns,
                    'target': target,
                }
            )
        qps_entries[method] = method_list
    return qps_entries


def _write_outputs(output_dir: str, output_entries: Dict[str, List[Dict[str, Any]]]) -> None:
    for method, entries in output_entries.items():
        out_path = os.path.join(output_dir, f"schedule_point_profiling_{method}.yaml")
        os.makedirs(output_dir, exist_ok=True)
        with open(out_path, 'w') as f:
            yaml.dump(entries, f, Dumper=CustomYamlDumper, sort_keys=False)
        print(f"Wrote {len(entries)} entries to {out_path}")


def _write_qps_outputs(output_dir: str, qps_entries: Dict[str, List[Dict[str, Any]]]) -> None:
    for method, entries in qps_entries.items():
        out_path = os.path.join(output_dir, f"schedule_point_profiling_{method}_qps.yaml")
        os.makedirs(output_dir, exist_ok=True)
        with open(out_path, 'w') as f:
            yaml.dump(entries, f, Dumper=CustomYamlDumper, sort_keys=False)
        print(f"Wrote {len(entries)} QPS entries to {out_path}")


def main() -> None:
    parser = argparse.ArgumentParser(description="Aggregate schedule point profiling results.")
    parser.add_argument(
        '--models',
        type=str,
        default='',
        help='Comma-separated model names to include; empty means all models'
    )
    args = parser.parse_args()

    model_filter = _parse_models_arg(args.models)
    records_by_method = _collect_records(OUTPUT_DIR, model_filter, "profiline.yaml")
    if not records_by_method:
        print('No profiling data found. Nothing written.')
        return

    output_entries = _build_output_entries(records_by_method)
    _write_outputs(OUTPUT_DIR, output_entries)

    qps_entries = _build_qps_entries(output_entries)
    _write_qps_outputs(OUTPUT_DIR, qps_entries)


if __name__ == '__main__':
    main()
