#!/usr/bin/env python3
"""
Emit Gemmini-specific pipeline-runtime artifacts from HybridMapper outputs.

Artifacts under output/pipeline/<model>/:
  - layers_gemmini.yaml
  - mapping_gemmini/<layer>.yaml
  - entire_model/2_1024_16_19_64_{ours2,gemini2,tangram2}.yaml
  - dummy_weight/model.bin + dummy_input/input.bin + dummy_input/golden/golden.bin
"""

import argparse
import subprocess
import sys
from pathlib import Path

from ruamel.yaml import YAML

METHODS = ("ours2", "gemini2", "tangram2")
CANONICAL_ACC = 2
CANONICAL_SPM_PER_ACC_KB = 1024
CANONICAL_BATCH = 16
CANONICAL_DRAM_BW = 19
CANONICAL_NOC_BW = 64


def load_yaml(path: Path):
    y = YAML(typ="safe")
    with path.open("r", encoding="utf-8") as f:
        return y.load(f)


def as_int_list(value):
    if isinstance(value, list):
        return [int(v) for v in value]
    return []


def as_int(value, default=0):
    if value is None:
        return int(default)
    return int(value)


def fmt_list(values):
    return "[" + ", ".join(str(int(v)) for v in values) + "]"


def fmt_list_generic(values):
    out = []
    for value in values:
        if isinstance(value, str):
            out.append(value)
        else:
            out.append(str(int(value)))
    return "[" + ", ".join(out) + "]"


def fmt_map(values):
    if not values:
        return "{}"
    parts = []
    for key, value in values.items():
        parts.append(f"{int(key)}: {int(value)}")
    return "{" + ", ".join(parts) + "}"


def emit_nested_list(lines, key, nested, indent="      "):
    lines.append(f"{indent}{key}:")
    for item in nested:
        lines.append(f"{indent}- {fmt_list_generic(item)}")


def emit_layers_gemmini(model_dir: Path):
    src = model_dir / "layers.yaml"
    dst = model_dir / "layers_gemmini.yaml"
    if not src.exists():
        raise SystemExit(f"layers.yaml not found: {src}")
    dst.write_bytes(src.read_bytes())
    print(f"emitted {dst}")


def candidate_key(candidate):
    target = candidate.get("target", {}) or {}
    mapping = candidate.get("mapping", {}) or {}
    return (
        as_int(target.get("accel", 0)),
        tuple(as_int_list(mapping.get("dramBypass", []))),
        tuple(as_int_list(mapping.get("spmBypass", []))),
    )


def candidate_score(candidate, candidate_index: int):
    perf = candidate.get("performance", {}) or {}
    return (
        as_int(perf.get("spmPageUtil", 1 << 30)),
        as_int(perf.get("dramAccess", 1 << 30)),
        as_int(perf.get("spmUtil", 1 << 30)),
        as_int(perf.get("accelUtil", 1 << 30)),
        candidate_index,
    )


def flatten_candidate(candidate, candidate_index: int):
    target = candidate.get("target", {}) or {}
    mapping = candidate.get("mapping", {}) or {}
    perf = candidate.get("performance", {}) or {}
    others = candidate.get("others", {}) or {}
    return {
        "source_candidate_index": candidate_index,
        "target_accel": as_int(target.get("accel", 0)),
        "mapping_tile": as_int_list(mapping.get("tile", [])),
        "mapping_dram_bypass": as_int_list(mapping.get("dramBypass", [])),
        "mapping_spm_bypass": as_int_list(mapping.get("spmBypass", [])),
        "performance_accel_util": as_int(perf.get("accelUtil", 0)),
        "performance_spm_util": as_int(perf.get("spmUtil", 0)),
        "performance_spm_page_util": as_int(perf.get("spmPageUtil", 0)),
        "performance_dram_access": as_int(perf.get("dramAccess", 0)),
        "others_spm_tensor_page_count": as_int_list(others.get("spmTensorPageCount", [])),
        "others_spm_tensor_util": as_int_list(others.get("spmTensorUtil", [])),
        "others_total_tiles": as_int(others.get("totalTiles", 0)),
        "others_spm_dimensions": as_int_list(others.get("spmDimensions", [])),
        "others_spm_tensor_addr": as_int_list(others.get("spmTensorAddr", [])),
        "others_first_tensor_page_num": as_int_list(others.get("firstTensorPageNum", [])),
    }


def emit_mapping_gemmini(model_dir: Path):
    src_dir = model_dir / "mapping"
    dst_dir = model_dir / "mapping_gemmini"
    if not src_dir.is_dir():
        raise SystemExit(f"mapping dir not found: {src_dir}")
    dst_dir.mkdir(parents=True, exist_ok=True)

    for src in sorted(src_dir.glob("*.yaml")):
        if not src.stem.isdigit():
            continue
        data = load_yaml(src) or {}
        raw_candidates = data.get("candidates", []) or []
        picked = {}
        for idx, candidate in enumerate(raw_candidates):
            if not isinstance(candidate, dict):
                continue
            key = candidate_key(candidate)
            score = candidate_score(candidate, idx)
            prev = picked.get(key)
            if prev is None or score < prev[0]:
                picked[key] = (score, flatten_candidate(candidate, idx))

        lines = [
            f"layer_id: {int(src.stem)}",
            f"source_mapping: {src}",
            "candidates:",
        ]
        for _, item in sorted(picked.values(), key=lambda entry: entry[0]):
            lines.extend([
                f"  - target_accel: {item['target_accel']}",
                f"    source_candidate_index: {item['source_candidate_index']}",
                f"    mapping_tile: {fmt_list(item['mapping_tile'])}",
                f"    mapping_dram_bypass: {fmt_list(item['mapping_dram_bypass'])}",
                f"    mapping_spm_bypass: {fmt_list(item['mapping_spm_bypass'])}",
                f"    performance_accel_util: {item['performance_accel_util']}",
                f"    performance_spm_util: {item['performance_spm_util']}",
                f"    performance_spm_page_util: {item['performance_spm_page_util']}",
                f"    performance_dram_access: {item['performance_dram_access']}",
                f"    others_spm_tensor_page_count: {fmt_list(item['others_spm_tensor_page_count'])}",
                f"    others_spm_tensor_util: {fmt_list(item['others_spm_tensor_util'])}",
                f"    others_total_tiles: {item['others_total_tiles']}",
                f"    others_spm_dimensions: {fmt_list(item['others_spm_dimensions'])}",
                f"    others_spm_tensor_addr: {fmt_list(item['others_spm_tensor_addr'])}",
                f"    others_first_tensor_page_num: {fmt_list(item['others_first_tensor_page_num'])}",
            ])
        dst = dst_dir / src.name
        dst.write_text("\n".join(lines) + "\n", encoding="utf-8")
        print(f"emitted {dst} ({len(picked)} canonical candidates)")


def stage_acc_values(doc):
    vals = []
    for seg in (doc.get("segments", []) or []):
        for stage_group in (seg.get("stages", []) or []):
            if not isinstance(stage_group, list) or not stage_group:
                continue
            stage = stage_group[0]
            if isinstance(stage, dict):
                vals.append(as_int(stage.get("accUtil", 0)))
    return vals


def pipeline_source_sort_key(path: Path):
    doc = load_yaml(path) or {}
    vals = stage_acc_values(doc)
    acc = int(path.name.split("_", 1)[0])
    max_acc = max(vals) if vals else 1 << 30
    clamp_cost = sum(max(v - CANONICAL_ACC, 0) for v in vals)
    return (max_acc, clamp_cost, acc, path.name)


def choose_pipeline_source(model_dir: Path, method: str):
    entire_dir = model_dir / "entire_model"
    candidates = sorted(entire_dir.glob(f"*_{CANONICAL_SPM_PER_ACC_KB}_{CANONICAL_BATCH}_{CANONICAL_DRAM_BW}_{CANONICAL_NOC_BW}_{method}.yaml"))
    noncanonical = [p for p in candidates if not p.name.startswith(f"{CANONICAL_ACC}_")]
    if noncanonical:
        candidates = noncanonical
    if not candidates:
        raise SystemExit(f"no source pipeline yaml found for method={method} under {entire_dir}")
    return min(candidates, key=pipeline_source_sort_key)


def emit_canonical_pipeline(model_dir: Path, method: str):
    src = choose_pipeline_source(model_dir, method)
    dst = model_dir / "entire_model" / f"{CANONICAL_ACC}_{CANONICAL_SPM_PER_ACC_KB}_{CANONICAL_BATCH}_{CANONICAL_DRAM_BW}_{CANONICAL_NOC_BW}_{method}.yaml"
    doc = load_yaml(src) or {}
    lines = [f"cost: {as_int(doc.get('cost', 0))}", "segments:"]

    for seg in doc.get("segments", []) or []:
        ring_count = {int(k): int(v) for k, v in (seg.get("ring_buffer_count", {}) or {}).items()}
        ring_use = {int(k): int(v) for k, v in (seg.get("ring_buffer_use_count", {}) or {}).items()}
        ring_size = {int(k): 1 for k in ring_count.keys()}
        shared_read_first = {int(k): int(v) for k, v in (seg.get("shared_tensor_is_read_first", {}) or {}).items()}

        lines.append(f"- acc_util: {CANONICAL_ACC}")
        if "cost" in seg:
            lines.append(f"  cost: {as_int(seg.get('cost', 0))}")
        if "start_layer_idx" in seg:
            lines.append(f"  start_layer_idx: {as_int(seg.get('start_layer_idx', 0))}")
        if "end_layer_idx" in seg:
            lines.append(f"  end_layer_idx: {as_int(seg.get('end_layer_idx', 0))}")
        if "segment_idx" in seg:
            lines.append(f"  segment_idx: {as_int(seg.get('segment_idx', 0))}")
        if ring_count:
            lines.append(f"  ring_buffer_count: {fmt_map(ring_count)}")
            lines.append(f"  ring_buffer_size_per: {fmt_map(ring_size)}")
            lines.append(f"  ring_buffer_use_count: {fmt_map(ring_use)}")
        if shared_read_first:
            lines.append(f"  shared_tensor_is_read_first: {fmt_map(shared_read_first)}")
        if "subBatchSize" in seg:
            lines.append(f"  subBatchSize: {as_int(seg.get('subBatchSize', 1))}")
        lines.append("  stages:")

        for stage_group in seg.get("stages", []) or []:
            if not isinstance(stage_group, list) or not stage_group:
                raise SystemExit("pipeline stage contract violation: stages[i] must be non-empty")
            stage = stage_group[0]
            if not isinstance(stage, dict):
                raise SystemExit("pipeline stage contract violation: stages[i][0] must be a mapping")
            layer_ids = stage.get("layerIdList", []) or []
            if len(layer_ids) != 1:
                raise SystemExit(f"only single-layer stages are supported, got layerIdList={layer_ids}")
            acc = min(CANONICAL_ACC, max(1, as_int(stage.get("accUtil", 1))))
            lines.append(f"  - - accUtil: {acc}")
            emit_nested_list(lines, "dramBypassList", stage.get("dramBypassList", []) or [[]])
            lines.append(f"      entryTensorDoubleBufferList: {fmt_list_generic(stage.get('entryTensorDoubleBufferList', []) or [])}")
            lines.append(f"      entryTensorIdList: {fmt_list_generic(stage.get('entryTensorIdList', []) or [])}")
            lines.append(f"      entryTensorTypeList: {fmt_list_generic(stage.get('entryTensorTypeList', []) or [])}")
            lines.append(f"      exportTensorDoubleBufferList: {fmt_list_generic(stage.get('exportTensorDoubleBufferList', []) or [])}")
            lines.append(f"      exportTensorIdList: {fmt_list_generic(stage.get('exportTensorIdList', []) or [])}")
            lines.append(f"      exportTensorTypeList: {fmt_list_generic(stage.get('exportTensorTypeList', []) or [])}")
            lines.append(f"      fixTensorDramBypassIdList: {fmt_list_generic(stage.get('fixTensorDramBypassIdList', []) or [])}")
            lines.append(f"      globalStageId: {as_int(stage.get('globalStageId', 0))}")
            lines.append(f"      innerIsolateTensorId: {fmt_list_generic(stage.get('innerIsolateTensorId', []) or [])}")
            lines.append(f"      innerSharedTensorId: {fmt_list_generic(stage.get('innerSharedTensorId', []) or [])}")
            lines.append(f"      layerIdList: {fmt_list_generic(layer_ids)}")
            emit_nested_list(lines, "pAccIdxList", [[]])
            emit_nested_list(lines, "spmBypassList", stage.get("spmBypassList", []) or [[]])
            lines.append(f"      tensorIdList: {fmt_list_generic(stage.get('tensorIdList', []) or [])}")
            emit_nested_list(lines, "tensorUsageCountList", stage.get("tensorUsageCountList", []) or [[]])
            emit_nested_list(lines, "tensorUseLazyFetch", stage.get("tensorUseLazyFetch", []) or [[]])
            emit_nested_list(lines, "vAccIdxList", [list(range(acc))])

    dst.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"emitted {dst} from {src.name}")
    return dst


def emit_dummy_runtime_data(repo_root: Path, model: str, output_root: str, layers_yaml: Path, pipeline_yaml: Path):
    script = repo_root / "scripts" / "create-pipeline-dummy-runtime-data.py"
    cmd = [
        sys.executable,
        str(script),
        "--model",
        model,
        "--layers-yaml",
        str(layers_yaml),
        "--pipeline-yaml",
        str(pipeline_yaml),
        "--output-root",
        output_root,
    ]
    subprocess.run(cmd, cwd=str(repo_root), check=True)


def main():
    parser = argparse.ArgumentParser(description="Emit Gemmini pipeline-runtime artifacts for HybridMapper outputs.")
    parser.add_argument("--model", required=True, help="model name under output/pipeline, e.g. bertmini")
    parser.add_argument("--output-root", default="output/pipeline", help="pipeline output root relative to HybridMapper repo root")
    parser.add_argument("--skip-mapping", action="store_true", help="skip mapping_gemmini emission")
    parser.add_argument("--skip-canonical", action="store_true", help="skip canonical 2_1024_16_19_64 pipeline emission")
    parser.add_argument("--skip-dummy", action="store_true", help="skip dummy model/input/golden emission")
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parents[1]
    model_dir = repo_root / args.output_root / args.model
    if not model_dir.is_dir():
        raise SystemExit(f"model output dir not found: {model_dir}")

    emit_layers_gemmini(model_dir)
    if not args.skip_mapping:
        emit_mapping_gemmini(model_dir)

    generated = {}
    if not args.skip_canonical:
        for method in METHODS:
            generated[method] = emit_canonical_pipeline(model_dir, method)

    if not args.skip_dummy:
        pipeline_yaml = generated.get("ours2")
        if pipeline_yaml is None:
            pipeline_yaml = model_dir / "entire_model" / (
                f"{CANONICAL_ACC}_{CANONICAL_SPM_PER_ACC_KB}_{CANONICAL_BATCH}_{CANONICAL_DRAM_BW}_{CANONICAL_NOC_BW}_ours2.yaml"
            )
        if pipeline_yaml.exists():
            emit_dummy_runtime_data(repo_root, args.model, args.output_root, model_dir / "layers_gemmini.yaml", pipeline_yaml)
        else:
            print(f"skipping dummy runtime data because canonical ours2 pipeline yaml is missing: {pipeline_yaml}")


if __name__ == "__main__":
    main()
