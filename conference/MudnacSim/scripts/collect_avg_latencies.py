#!/usr/bin/env python3
"""
Collect model average latencies from simulation output files and export a CSV.

Sources:
- expr/output/expr_multi_pipeline/script_output/mudnacsim/
- expr/output/expr_mixed_models3/script_output/mudnacsim/

Expected filename pattern (examples):
  idx-1_ours2_models-bertbase,effinetb0,resnet50,mobilenetv2_batch-16_acc-16,24,16,8_spm-2048,512,256,512_noc-64_dram-1_allbank_hilbert
  idx-3_camdn_models-bertbase,effinetb0,resnet50,mobilenetv2_batch-16_acc-16,24,16,8_spm-2048,512,256,512_noc-64_dram-1_allbank_hilbert

Each file contains a section:
  Model Avg Latencies:
  Model <name>: <cycles> cycles

Output CSV columns:
  idx, method, models (commas replaced with '|'), batch, acc, spm, noc, dram,
  pipe_spm_strategy, pipe_acc_strategy,
  model_1, latency_1, model_2, latency_2, model_3, latency_3, model_4, latency_4

If fewer than 4 models are present, remaining model/latency columns are left empty.
"""

from __future__ import annotations

import csv
import re
from dataclasses import dataclass
from pathlib import Path
from typing import List, Tuple

# Directories to scan
DIR_MULTI = Path("expr/output/expr_multi_pipeline/script_output/mudnacsim")
DIR_MIXED = Path("expr/output/expr_mixed_models3/script_output/mudnacsim")

# Regex to parse filename fields
# Multi-pipeline files include pipe strategies; mixed-model files omit them.
FILENAME_RE_FULL = re.compile(
    r"^idx-(?P<idx>[^_]+)_(?P<method>[^_]+)_models-(?P<models>[^_]+)_batch-(?P<batch>[^_]+)_acc-(?P<acc>[^_]+)_spm-(?P<spm>[^_]+)_noc-(?P<noc>[^_]+)_dram-(?P<dram>[^_]+)_(?P<pipe_spm>[^_]+)_(?P<pipe_acc>[^_]+)$"
)

FILENAME_RE_SHORT = re.compile(
    r"^idx-(?P<idx>[^_]+)_(?P<method>[^_]+)_models-(?P<models>[^_]+)_batch-(?P<batch>[^_]+)_acc-(?P<acc>[^_]+)_spm-(?P<spm>[^_]+)_noc-(?P<noc>[^_]+)_dram-(?P<dram>[^_]+)$"
)

LATENCY_RE = re.compile(r"Model\s+(?P<model>[^:]+):\s+(?P<latency>\d+)\s+cycles")

MAX_MODELS = 4


@dataclass
class ParsedFile:
    idx: str
    method: str
    models: List[str]
    batch: str
    acc: str
    spm: str
    noc: str
    dram: str
    pipe_spm: str
    pipe_acc: str
    latencies: List[Tuple[str, str]]  # (model, cycles)


def parse_filename(path: Path) -> ParsedFile | None:
    m = FILENAME_RE_FULL.match(path.name)
    pipe_spm = ""
    pipe_acc = ""

    if not m:
        m = FILENAME_RE_SHORT.match(path.name)
        if not m:
            return None
    else:
        pipe_spm = m.group("pipe_spm")
        pipe_acc = m.group("pipe_acc")

    groups = m.groupdict()
    models_list = groups["models"].split(",")
    return ParsedFile(
        idx=groups["idx"],
        method=groups["method"],
        models=models_list,
        batch=groups["batch"],
        acc=groups["acc"],
        spm=groups["spm"],
        noc=groups["noc"],
        dram=groups["dram"],
        pipe_spm=pipe_spm,
        pipe_acc=pipe_acc,
        latencies=[],
    )


def extract_latencies(text: str) -> List[Tuple[str, str]]:
    latencies: List[Tuple[str, str]] = []
    # First try the original format: "Model <name>: <cycles> cycles"
    for match in LATENCY_RE.finditer(text):
        latencies.append((match.group("model").strip(), match.group("latency")))
    if latencies:
        return latencies

    # Fallback: mixed-model output format, e.g.:
    #    Model Name: effinetb0
    #    Avg Model Latency except last one: 63710365
    model_name_re = re.compile(r"Model\s+Name:\s*(?P<model>.+)")
    avg_except_re = re.compile(r"Avg Model Latency except last one:\s*(?P<latency>\d+)")

    current_model: str | None = None
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        m_name = model_name_re.match(line)
        if m_name:
            current_model = m_name.group("model").strip()
            continue
        if current_model:
            m_lat = avg_except_re.match(line)
            if m_lat:
                latencies.append((current_model, m_lat.group("latency")))
                current_model = None

    return latencies


def gather_from_dir(directory: Path) -> List[ParsedFile]:
    results: List[ParsedFile] = []
    if not directory.exists():
        return results
    for path in directory.iterdir():
        if not path.is_file():
            continue
        parsed = parse_filename(path)
        if not parsed:
            continue
        try:
            content = path.read_text(encoding="utf-8", errors="replace")
        except Exception:
            continue
        parsed.latencies = extract_latencies(content)
        results.append(parsed)
    return results


def to_csv_rows(items: List[ParsedFile]) -> List[List[str]]:
    # Deprecated: we now aggregate by idx in main; keep compatibility if needed
    rows: List[List[str]] = []
    for item in items:
        models_field = "|".join(item.models)  # replace commas for CSV readability
        spm_field = item.spm.replace(",", "|") if item.spm else item.spm
        row = [
            item.idx,
            item.method,
            models_field,
            item.batch,
            item.acc,
            spm_field,
            item.noc,
            item.dram,
            item.pipe_spm,
            item.pipe_acc,
        ]
        # ensure up to MAX_MODELS entries
        lat_pairs = item.latencies[:MAX_MODELS]
        lat_pairs += [("", "")] * (MAX_MODELS - len(lat_pairs))
        for model, lat in lat_pairs:
            row.append(model)
            row.append(lat)
        rows.append(row)
    return rows


def normalize_method(name: str) -> str:
    s = name.lower()
    if "cam" in s or "camd" in s:
        return "camdan"
    if "tang" in s:
        return "tangram"
    if "gem" in s:
        return "gemini"
    if "our" in s:
        return "ours"
    return s


def aggregate_by_idx(items: List[ParsedFile]):
    # Group items by idx
    groups: dict[str, dict] = {}
    for it in items:
        # Keep separate rows when the same idx appears with different DRAM channels
        comp_key = (it.idx, it.dram)
        g = groups.setdefault(
            comp_key,
            {
                "idx": it.idx,
                "models": it.models,
                "acc": it.acc,
                "spm": it.spm,
                "dram": it.dram,
                "methods": {},
            },
        )
        # ensure models/acc/spm consistent; prefer first seen
        # build model->lat map from parsed latencies
        model_to_lat = {m: l for (m, l) in it.latencies}
        method = normalize_method(it.method)
        # create ordered lat list aligned with g['models']
        aligned = []
        for m in g["models"][:MAX_MODELS]:
            aligned.append(model_to_lat.get(m, ""))
        # pad to MAX_MODELS
        aligned += [""] * (MAX_MODELS - len(aligned))
        g["methods"][method] = aligned
    return groups


def main():
    out_path = Path("collected_latencies.csv")
    items = gather_from_dir(DIR_MULTI) + gather_from_dir(DIR_MIXED)
    # aggregate by idx
    groups = aggregate_by_idx(items)

    # Prepare headers as requested
    headers = ["idx", "models", "acc", "spm", "dram_channel"]
    for i in range(1, MAX_MODELS + 1):
        headers.append(f"model_{i}")

    methods = ["camdan", "tangram", "gemini", "ours"]
    # latency fields for methods (3 methods * 4 models)
    for m in methods:
        for i in range(1, MAX_MODELS + 1):
            headers.append(f"{m}_{i}")

    # normalized speed fields vs camdan and tangram
    norm_bases = ["camdan", "tangram"]
    speedup_methods = methods
    for base in norm_bases:
        for m in speedup_methods:
            for i in range(1, MAX_MODELS + 1):
                headers.append(f"{m}_norm_vs_{base}_{i}")

    # average normalized speedups
    for base in norm_bases:
        for m in speedup_methods:
            headers.append(f"{m}_avg_norm_vs_{base}")

    rows: List[List[str]] = []
    # deterministic order by numeric idx if possible
    def idx_key(k: str):
        try:
            return int(k)
        except Exception:
            return k

    def idx_key(key):
        if isinstance(key, tuple) and len(key) == 2:
            idx_part, dram_part = key
        else:
            idx_part, dram_part = key, None

        def to_num(val):
            try:
                return int(val)
            except Exception:
                return val

        return (to_num(idx_part), to_num(dram_part) if dram_part is not None else -1)

    for comp_key in sorted(groups.keys(), key=idx_key):
        g = groups[comp_key]
        spm_field = g.get("spm", "")
        if spm_field:
            spm_field = spm_field.replace(",", "|")
        row: List[str] = [
            g["idx"],
            "|".join(g["models"]),
            g.get("acc", ""),
            spm_field,
            g.get("dram", ""),
        ]
        # model_1..4
        for m in g["models"][:MAX_MODELS]:
            row.append(m)
        if len(g["models"]) < MAX_MODELS:
            row += [""] * (MAX_MODELS - len(g["models"]))

        # method latencies
        method_lat_map = g["methods"]
        for m in methods:
            lat_list = method_lat_map.get(m, [""] * MAX_MODELS)
            # ensure length
            lat_list = lat_list[:MAX_MODELS] + [""] * (MAX_MODELS - len(lat_list))
            row.extend(lat_list)

        # make floats or None
        def to_float(x: str):
            try:
                return float(x)
            except Exception:
                return None

        base_maps = {
            base: [to_float(x) for x in method_lat_map.get(base, [""] * MAX_MODELS)]
            for base in norm_bases
        }

        # per-base normalization values (normalized = method_latency / base_latency)
        for base in norm_bases:
            base_lats = base_maps.get(base, [None] * MAX_MODELS)
            for m in speedup_methods:
                lat_list = method_lat_map.get(m, [""] * MAX_MODELS)
                lat_list = lat_list[:MAX_MODELS] + [""] * (MAX_MODELS - len(lat_list))
                for i in range(MAX_MODELS):
                    b = base_lats[i] if i < len(base_lats) else None
                    cur = to_float(lat_list[i])
                    if b is None or b == 0 or cur is None:
                        row.append("")
                    else:
                        norm = cur / b
                        row.append(f"{norm:.4f}")

        # average normalized speedups per base and method
        for base in norm_bases:
            base_lats = base_maps.get(base, [None] * MAX_MODELS)
            for m in speedup_methods:
                lat_list = method_lat_map.get(m, [""] * MAX_MODELS)
                lat_list = lat_list[:MAX_MODELS] + [""] * (MAX_MODELS - len(lat_list))
                vals: List[float] = []
                for i in range(MAX_MODELS):
                    b = base_lats[i] if i < len(base_lats) else None
                    cur = to_float(lat_list[i])
                    if b is None or b == 0 or cur is None:
                        continue
                    vals.append(cur / b)
                if vals:
                    avg = sum(vals) / len(vals)
                    row.append(f"{avg:.4f}")
                else:
                    row.append("")

        rows.append(row)

    with out_path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(headers)
        writer.writerows(rows)
    print(f"Wrote {len(rows)} rows to {out_path}")


if __name__ == "__main__":
    main()
