#!/usr/bin/env python3
"""
Compute speedups of ours2 vs tangram2/gemini2 for 4-model bundles under specific
hardware constraints, and list the top-N bundles by average speedup.

Assumptions based on the request:
- Use rows where channels=1, noc_flit_size=64, pipeSpmStrategy=allbank,
  pipeAccStrategy=hilbert, method in {ours2, tangram2, gemini2}, and
  totalBatch in {16, 32}.
- A valid bundle contains 4 records (ours2) with distinct modelName, the sum of
  acc_num equals 64, and the sum of spm_bank_size * acc_num equals 64 * 1024.
- For each ours2 record in the bundle, corresponding tangram2 and gemini2
  records with identical keys must exist to compute speedups.
- Bundles are scored by the mean of (avg gemini speedup, avg tangram speedup).

Output format per bundle:
model_name: modelA,modelB,modelC,modelD (sorted lexicographically)
target:     acc_num=[...], spm_bank_size=[...], total_batch=<value>
gemini:     avg=<avg>, per_model={model: speedup, ...}
tangram:    avg=<avg>, per_model={model: speedup, ...}
"""

from __future__ import annotations

import argparse
import csv
import itertools
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Tuple
import json

CSV_FIELDNAMES = [
    "modelName",
    "channels",
    "noc_flit_size",
    "acc_num",
    "spm_bank_size",
    "pipeSpmStrategy",
    "pipeAccStrategy",
    "totalBatch",
    "method",
    "latency",
]

FILTERS = {
    "channels": "1",
    "noc_flit_size": "64",
    "pipeSpmStrategy": "allbank",
    "pipeAccStrategy": "hilbert",
}
METHODS = {"ours2", "tangram2", "gemini2"}
VALID_TOTAL_BATCH = {"16", "32"}
ACC_SUM_TARGET = 64
SPM_ACC_SUM_TARGET = 64 * 1024
BLS_ACC_NUM = 16
BLS_SPM_BANK_SIZE = 1024  # baseline spm_bank_size for tangram2/gemini2 comparison
BUNDLE_SIZE = 4


@dataclass(frozen=True)
class ConfigKey:
    model: str
    acc_num: int
    spm_bank_size: int
    total_batch: int

    @property
    def spm_acc_product(self) -> int:
        return self.acc_num * self.spm_bank_size


@dataclass
class Record:
    key: ConfigKey
    method: str
    latency: int


def load_filtered_records(csv_path: Path, allowed_models: set[str] | None) -> List[Record]:
    records: List[Record] = []
    with csv_path.open("r", newline="") as f:
        reader = csv.DictReader(f)
        missing = set(CSV_FIELDNAMES) - set(reader.fieldnames or [])
        if missing:
            raise ValueError(f"CSV missing expected columns: {sorted(missing)}")
        for row in reader:
            if any(row[k] != v for k, v in FILTERS.items()):
                continue
            if row["method"] not in METHODS:
                continue
            if row["totalBatch"] not in VALID_TOTAL_BATCH:
                continue
            if allowed_models is not None and row["modelName"] not in allowed_models:
                continue
            key = ConfigKey(
                model=row["modelName"],
                acc_num=int(row["acc_num"]),
                spm_bank_size=int(row["spm_bank_size"]),
                total_batch=int(row["totalBatch"]),
            )
            rec = Record(key=key, method=row["method"], latency=int(row["latency"]))
            records.append(rec)
    return records


def group_by_method(records: List[Record]) -> Dict[str, Dict[ConfigKey, Record]]:
    grouped: Dict[str, Dict[ConfigKey, Record]] = {m: {} for m in METHODS}
    for rec in records:
        grouped[rec.method][rec.key] = rec
    return grouped


def bundle_iterator(keys: List[ConfigKey]):
    # Generates bundles of size 4 satisfying distinct models, acc_num sum, and spm*acc sum.
    for combo in itertools.combinations(keys, BUNDLE_SIZE):
        models = {k.model for k in combo}
        if len(models) != BUNDLE_SIZE:
            continue
        acc_sum = sum(k.acc_num for k in combo)
        if acc_sum != ACC_SUM_TARGET:
            continue
        spm_acc_sum = sum(k.spm_acc_product for k in combo)
        if spm_acc_sum != SPM_ACC_SUM_TARGET:
            continue
        yield combo


def compute_speedups(ours: Record, other: Record) -> float:
    # Speedup of ours relative to other: other_latency / ours_latency.
    return other.latency / ours.latency if ours.latency else 0.0


def analyze(csv_path: Path, top_n: int, allowed_models: set[str] | None) -> List[dict]:
    records = load_filtered_records(csv_path, allowed_models)
    grouped = group_by_method(records)
    ours_keys_by_batch: Dict[int, List[ConfigKey]] = {tb: [] for tb in map(int, VALID_TOTAL_BATCH)}
    for key in grouped["ours2"]:
        ours_keys_by_batch[key.total_batch].append(key)

    bundles = []
    for total_batch, keys in ours_keys_by_batch.items():
        for combo in bundle_iterator(keys):
            # For comparison, use baseline keys with same model/acc/total_batch but spm_bank_size=1024.
            baseline_keys = [
                ConfigKey(
                    model=k.model,
                    acc_num=BLS_ACC_NUM,
                    spm_bank_size=BLS_SPM_BANK_SIZE,
                    total_batch=k.total_batch,
                )
                for k in combo
            ]
            if any(bk not in grouped["tangram2"] or bk not in grouped["gemini2"] for bk in baseline_keys):
                continue

            ours_recs = [grouped["ours2"][c] for c in combo]
            tangram_recs = [grouped["tangram2"][bk] for bk in baseline_keys]
            gemini_recs = [grouped["gemini2"][bk] for bk in baseline_keys]

            tangram_speedups = {t.key.model: compute_speedups(o, t) for o, t in zip(ours_recs, tangram_recs)}
            gemini_speedups = {g.key.model: compute_speedups(o, g) for o, g in zip(ours_recs, gemini_recs)}

            avg_tangram = sum(tangram_speedups.values()) / BUNDLE_SIZE
            avg_gemini = sum(gemini_speedups.values()) / BUNDLE_SIZE
            score = (avg_tangram + avg_gemini) / 2.0

            bundle_info = {
                "models": sorted([k.model for k in combo]),
                "acc_nums": [k.acc_num for k in combo],
                "spm_bank_sizes": [k.spm_bank_size for k in combo],
                "total_batch": total_batch,
                "per_model_cfg": {k.model: {"acc_num": k.acc_num, "spm_bank_size": k.spm_bank_size} for k in combo},
                "tangram_speedups": tangram_speedups,
                "gemini_speedups": gemini_speedups,
                "avg_tangram": avg_tangram,
                "avg_gemini": avg_gemini,
                "score": score,
            }
            bundles.append(bundle_info)

    # Sort by score descending, then by avg_gemini, then avg_tangram for stability.
    bundles.sort(key=lambda b: (b["score"], b["avg_gemini"], b["avg_tangram"]), reverse=True)
    if top_n < 0:
        return bundles
    return bundles[:top_n]


def format_bundle(bundle: dict) -> str:
    model_field = ",".join(bundle["models"])
    target_field = (
        f"acc_num={bundle['acc_nums']}, spm_bank_size={bundle['spm_bank_sizes']}, "
        f"total_batch={bundle['total_batch']}"
    )
    gemini_field = (
        f"avg={bundle['avg_gemini']:.4f}, per_model="
        + "{"
        + ", ".join(f"{m}:{bundle['gemini_speedups'][m]:.4f}" for m in bundle["models"])
        + "}"
    )
    tangram_field = (
        f"avg={bundle['avg_tangram']:.4f}, per_model="
        + "{"
        + ", ".join(f"{m}:{bundle['tangram_speedups'][m]:.4f}" for m in bundle["models"])
        + "}"
    )
    return (
        f"model_name: {model_field}\n"
        f"target: {target_field}\n"
        f"gemini: {gemini_field}\n"
        f"tangram: {tangram_field}\n"
    )


def bundle_to_yaml_obj(bundle: dict) -> dict:
    # Convert bundle dict to a clean YAML-serializable structure.
    # Build mapping paths for ours2/gemini2/tangram2 using target fields.
    def build_paths(method: str) -> List[str]:
        paths: List[str] = []
        tb = bundle["total_batch"]
        for m in bundle["models"]:
            cfg = bundle["per_model_cfg"][m]
            if method in ("gemini2", "tangram2"):
                acc = BLS_ACC_NUM
                spm = BLS_SPM_BANK_SIZE
            else:
                acc = cfg["acc_num"]
                spm = cfg["spm_bank_size"]
            paths.append(
                f"models/pipeline/{m}/entire_model/{acc}_{spm}_{tb}_19_64_{method}.yaml"
            )
        return paths

    return {
        "model_name": ",".join(bundle["models"]),
        "target": {
            "acc_num": bundle["acc_nums"],
            "spm_bank_per_size": bundle["spm_bank_sizes"],
            "total_batch": bundle["total_batch"],
        },
        "ours_mapping_path": build_paths("ours2"),
        "gemini_mapping_path": build_paths("gemini2"),
        "tangram_mapping_path": build_paths("tangram2"),
        "gemini": {
            "avg_speedup": round(bundle["avg_gemini"], 6),
            "per_model": {m: round(bundle["gemini_speedups"][m], 6) for m in bundle["models"]},
        },
        "tangram": {
            "avg_speedup": round(bundle["avg_tangram"], 6),
            "per_model": {m: round(bundle["tangram_speedups"][m], 6) for m in bundle["models"]},
        },
        "score": round(bundle["score"], 6),
    }


def write_yaml(bundles: List[dict], out_path: Path):
    # Write a minimal YAML without external dependencies.
    # Structure: list of entries produced by bundle_to_yaml_obj.
    obj_list = [bundle_to_yaml_obj(b) for b in bundles]
    # Minimal YAML emitter: rely on json for safe quoting, but format as YAML-ish.
    # We'll build YAML manually for readability.
    lines: List[str] = []
    for idx, obj in enumerate(obj_list, start=1):
        lines.append("- index: " + str(idx))
        lines.append("  model_name: " + obj["model_name"])
        lines.append("  target:")
        lines.append("    acc_num: " + json.dumps(obj["target"]["acc_num"]))
        lines.append("    spm_bank_per_size: " + json.dumps(obj["target"]["spm_bank_per_size"]))
        lines.append("    total_batch: " + str(obj["target"]["total_batch"]))
        # mapping paths
        lines.append("  ours_mapping_path:")
        for p in obj["ours_mapping_path"]:
            lines.append("    - " + p)
        lines.append("  gemini_mapping_path:")
        for p in obj["gemini_mapping_path"]:
            lines.append("    - " + p)
        lines.append("  tangram_mapping_path:")
        for p in obj["tangram_mapping_path"]:
            lines.append("    - " + p)
        lines.append("  gemini:")
        lines.append("    avg_speedup: " + str(obj["gemini"]["avg_speedup"]))
        lines.append("    per_model:")
        for m, v in obj["gemini"]["per_model"].items():
            lines.append(f"      {m}: {v}")
        lines.append("  tangram:")
        lines.append("    avg_speedup: " + str(obj["tangram"]["avg_speedup"]))
        lines.append("    per_model:")
        for m, v in obj["tangram"]["per_model"].items():
            lines.append(f"      {m}: {v}")
        lines.append("  score: " + str(obj["score"]))
    out_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description="Analyze speedups and list top bundles")
    parser.add_argument(
        "--csv",
        type=Path,
        default=Path("single_model_latencies.csv"),
        help="Path to single_model_latencies.csv",
    )
    parser.add_argument("--top", type=int, default=100, help="Number of bundles to show")
    parser.add_argument("--yaml", action="store_true", help="Also write YAML output of results")
    args = parser.parse_args()

    allowed_models =[
        "bertbase",
        "effinetb0",
        "mobilenetv2",
        "pointpillars",
        "resnet50",
        "vitb16",
        "w2v2base",
        "gnmt"
    ]

    top_bundles = analyze(args.csv, args.top, allowed_models)
    if not top_bundles:
        print("No bundles satisfy the constraints.")
        return
    for idx, bundle in enumerate(top_bundles, start=1):
        print(f"=== bundle #{idx} (score={bundle['score']:.4f}) ===")
        print(format_bundle(bundle))

    if args.yaml:
        out_name = f"multi_model_{BUNDLE_SIZE}.yaml"
        out_path = Path(out_name)
        write_yaml(top_bundles, out_path)
        print(f"Wrote YAML: {out_path}")


if __name__ == "__main__":
    main()
