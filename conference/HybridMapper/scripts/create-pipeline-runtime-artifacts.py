#!/usr/bin/env python3

from __future__ import annotations

import argparse
import os
import pickle
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Iterable, List, Sequence


def _maybe_reexec_with_repo_python():
    chipyard_root = Path(__file__).resolve().parents[3]
    preferred = chipyard_root / ".conda-env" / "bin" / "python"
    if os.environ.get("HYBRIDMAPPER_REEXEC") == "1":
        return
    if not preferred.is_file():
        return
    if Path(sys.executable).resolve() == preferred.resolve():
        return
    env = dict(os.environ)
    env["HYBRIDMAPPER_REEXEC"] = "1"
    os.execve(str(preferred), [str(preferred), str(Path(__file__).resolve()), *sys.argv[1:]], env)


_maybe_reexec_with_repo_python()


def _add_local_site_packages():
    chipyard_root = Path(__file__).resolve().parents[3]
    version_tag = f"python{sys.version_info.major}.{sys.version_info.minor}"
    for env_name in (".conda-env", ".conda-lock-env"):
        site_dir = chipyard_root / env_name / "lib" / version_tag / "site-packages"
        site_path = str(site_dir)
        if site_dir.is_dir() and site_path not in sys.path:
            sys.path.append(site_path)


_add_local_site_packages()

try:
    from ruamel.yaml import YAML as RuamelYAML
except ModuleNotFoundError:
    RuamelYAML = None

try:
    import yaml as pyyaml
except ModuleNotFoundError:
    pyyaml = None


HYBRIDMAPPER_ROOT = Path(__file__).resolve().parents[1]
CHIPYARD_ROOT = HYBRIDMAPPER_ROOT.parents[1]
if str(HYBRIDMAPPER_ROOT) not in sys.path:
    sys.path.insert(0, str(HYBRIDMAPPER_ROOT))

METHODS = ("ours2", "gemini2", "tangram2")
INTERMEDIATE_FILENAME_FMT = "{acc}_{spm_per_acc_kb}_{batch}_{dram_bw}_{noc_bw}_{method}.yaml"


@dataclass(frozen=True)
class RuntimeHardwareTarget:
    target_key: str
    chipyard_config: str
    num_cores: int
    num_gemmini: int
    num_dma: int
    shared_spad_local_size_bytes: int
    shared_spad_global_base_addr: int
    per_acc_spm_kb: int
    num_macs_per_array: int
    dram_bw_per_cycle: int
    noc_bw_per_cycle: int
    sbus_width_bits: int
    memory_channels: int
    default_batch: int
    pages_per_acc: int

    def as_dict(self) -> Dict[str, object]:
        return {
            "target_key": self.target_key,
            "chipyard_config": self.chipyard_config,
            "num_cores": self.num_cores,
            "num_gemmini": self.num_gemmini,
            "num_dma": self.num_dma,
            "shared_spad_local_size_bytes": self.shared_spad_local_size_bytes,
            "shared_spad_local_size_kb": self.shared_spad_local_size_bytes // 1024,
            "shared_spad_global_base_addr": hex(self.shared_spad_global_base_addr),
            "per_acc_spm_kb": self.per_acc_spm_kb,
            "per_gemmini_spm_kb": self.per_acc_spm_kb,
            "num_macs_per_array": self.num_macs_per_array,
            "num_macs_per_gemmini": self.num_macs_per_array,
            "dram_bw_per_cycle": self.dram_bw_per_cycle,
            "noc_bw_per_cycle": self.noc_bw_per_cycle,
            "sbus_width_bits": self.sbus_width_bits,
            "memory_channels": self.memory_channels,
            "default_batch": self.default_batch,
            "pages_per_acc": self.pages_per_acc,
        }


STEP1_TARGET = RuntimeHardwareTarget(
    target_key="rerocc_globalnoc_coupleddma_c2_g2_d2_spad1024kb_dram19_noc64_mac1024",
    chipyard_config="GemminiLearningConfigSpadReRoCCGlobalNoC2C1x2G2x1x2D2x1x2CoupledDMA",
    num_cores=2,
    num_gemmini=2,
    num_dma=2,
    shared_spad_local_size_bytes=1024 * 1024,
    shared_spad_global_base_addr=0x40000000,
    per_acc_spm_kb=1024,
    num_macs_per_array=1024,
    dram_bw_per_cycle=19,
    noc_bw_per_cycle=64,
    sbus_width_bits=64 * 8,
    memory_channels=2,
    default_batch=16,
    pages_per_acc=1024,
)

TARGETS: Dict[str, RuntimeHardwareTarget] = {
    STEP1_TARGET.target_key: STEP1_TARGET,
}


def ensure_dir(path: Path):
    path.mkdir(parents=True, exist_ok=True)


def load_yaml(path: Path):
    with path.open("r", encoding="utf-8") as f:
        if RuamelYAML is not None:
            return RuamelYAML(typ="safe").load(f)
        return pyyaml.safe_load(f)


def dump_yaml(path: Path, data: object):
    ensure_dir(path.parent)
    with path.open("w", encoding="utf-8") as f:
        if pyyaml is not None:
            class InlineListDumper(pyyaml.SafeDumper):
                def ignore_aliases(self, _data):
                    return True

            def represent_list(dumper, seq):
                flow_style = all(not isinstance(item, (dict, list)) for item in seq)
                return dumper.represent_sequence("tag:yaml.org,2002:seq", seq, flow_style=flow_style)

            def represent_dict(dumper, mapping):
                flow_style = all(
                    not isinstance(key, (dict, list)) and not isinstance(value, (dict, list))
                    for key, value in mapping.items()
                )
                return dumper.represent_mapping("tag:yaml.org,2002:map", mapping, flow_style=flow_style)

            InlineListDumper.add_representer(list, represent_list)
            InlineListDumper.add_representer(dict, represent_dict)
            pyyaml.dump(
                data,
                f,
                Dumper=InlineListDumper,
                sort_keys=False,
                allow_unicode=False,
                width=1 << 20,
            )
        elif RuamelYAML is not None:
            yaml = RuamelYAML()
            yaml.default_flow_style = False
            yaml.indent(mapping=2, sequence=4, offset=2)
            yaml.width = 1 << 20
            yaml.dump(data, f)
        else:
            raise RuntimeError("no YAML writer available")


def to_int_list(values: Any) -> List[int]:
    if not isinstance(values, list):
        return []
    return [int(v) for v in values]


def value_or_default(value: Any, default: Any) -> Any:
    return default if value is None else value


def layer_type(layer: Any) -> str:
    if isinstance(layer, dict):
        return str(layer.get("type", ""))
    return str(getattr(layer, "type", ""))


def layer_params(layer: Any) -> List[int]:
    if isinstance(layer, dict):
        return to_int_list(layer.get("param", []))
    return [int(v) for v in getattr(layer, "param", [])]


def is_conv(layer: Any) -> bool:
    if hasattr(layer, "isConv"):
        return bool(layer.isConv())
    return layer_type(layer) == "conv"


def is_resadd(layer: Any) -> bool:
    if hasattr(layer, "isResadd"):
        return bool(layer.isResadd())
    return layer_type(layer) == "resadd"


def layer_to_split_kind(layer: Any, target_accel: int) -> str:
    target_accel = int(target_accel)
    if target_accel <= 1:
        return "single"
    if is_conv(layer):
        params = layer_params(layer)
        out_channels = params[2] if len(params) > 2 else 0
        return "oc" if out_channels >= target_accel else "spatial"
    if is_resadd(layer):
        return "resadd_spatial"
    return "single"


def load_layer_specs_from_model_yaml(path: Path) -> List[dict]:
    doc = load_yaml(path) or {}
    layers = [dict(layer) for layer in (doc.get("layers", []) or [])]
    layers.sort(key=lambda item: int(item.get("index", 0) or 0))
    return layers


def index_layers(layers: Sequence[Any]) -> Dict[int, Any]:
    indexed: Dict[int, Any] = {}
    for idx, layer in enumerate(layers):
        if isinstance(layer, dict):
            layer_idx = int(value_or_default(layer.get("index", idx), idx))
        else:
            layer_idx = idx
        indexed[layer_idx] = layer
    return indexed


def maybe_import_hybridmapper():
    try:
        from HybridMapper.GraphPartition import GraphPartitionCandidate, GraphPartitioner
        from HybridMapper.HybridMapper import HybridMapper
        from HybridMapper.Model import load_model
        from HybridMapper.PipelineTarget import get_mapping_target
        from HybridMapper.SASearch import (
            GraphPartitionSimulatedAnnealing,
            PipelineSolution,
            PipelineTarget,
            SolutionToRecordFactory,
        )
        from HybridMapper.Target import Target
        from HybridMapper.pipeline_models import models
    except Exception as exc:  # pragma: no cover - depends on external solver env
        return None, exc

    return {
        "GraphPartitionCandidate": GraphPartitionCandidate,
        "GraphPartitioner": GraphPartitioner,
        "HybridMapper": HybridMapper,
        "PipelineSolution": PipelineSolution,
        "PipelineTarget": PipelineTarget,
        "SolutionToRecordFactory": SolutionToRecordFactory,
        "GraphPartitionSimulatedAnnealing": GraphPartitionSimulatedAnnealing,
        "Target": Target,
        "load_model": load_model,
        "get_mapping_target": get_mapping_target,
        "models": models,
    }, None


def old_pipeline_filename(target: RuntimeHardwareTarget, method: str) -> str:
    return INTERMEDIATE_FILENAME_FMT.format(
        acc=target.num_gemmini,
        spm_per_acc_kb=target.per_acc_spm_kb,
        batch=target.default_batch,
        dram_bw=target.dram_bw_per_cycle,
        noc_bw=target.noc_bw_per_cycle,
        method=method,
    )


def runtime_pipeline_filename(target: RuntimeHardwareTarget, method: str) -> str:
    return f"pipeline_mapping.{target.target_key}.{method}.yaml"


def runtime_layer_mapping_filename(target: RuntimeHardwareTarget) -> str:
    return f"gemmini_layer_mapping.{target.target_key}.yaml"


def runtime_graph_partition_filename(target: RuntimeHardwareTarget) -> str:
    return f"graph_partition.{target.target_key}.yaml"


def runtime_hardware_filename(target: RuntimeHardwareTarget) -> str:
    return f"hardware_target.{target.target_key}.yaml"


def runtime_input_filename(target: RuntimeHardwareTarget) -> str:
    return f"runtime_input.{target.target_key}.bin"


def runtime_golden_filename(target: RuntimeHardwareTarget, method: str) -> str:
    return f"golden.{target.target_key}.{method}.bin"


def runtime_manifest_filename(target: RuntimeHardwareTarget) -> str:
    return f"manifest.{target.target_key}.yaml"


def resolve_legacy_source_dir(model_name: str, override: str) -> Path | None:
    if override:
        candidate = Path(override).resolve()
        if candidate.is_dir():
            return candidate
        raise RuntimeError(f"legacy source dir does not exist: {candidate}")

    candidates = [
        CHIPYARD_ROOT
        / "generators"
        / "gemmini"
        / "software"
        / "gemmini-rocc-tests"
        / "rerocc-linux-tests-coupleddma"
        / "workload"
        / "overlay"
        / "root"
        / "rerocc-linux-tests"
        / "pipeline-runtime"
        / model_name,
        CHIPYARD_ROOT
        / "generators"
        / "gemmini"
        / "software"
        / "gemmini-rocc-tests"
        / "rerocc-linux-tests"
        / "workload"
        / "overlay"
        / "root"
        / "rerocc-linux-tests"
        / "pipeline-runtime"
        / model_name,
        HYBRIDMAPPER_ROOT / "output" / "pipeline" / model_name,
    ]
    for candidate in candidates:
        if not candidate.is_dir():
            continue
        if (candidate / "layers_gemmini.yaml").is_file() and (candidate / "mapping_gemmini").is_dir():
            return candidate
    return None


def copy_file(src: Path, dst: Path):
    ensure_dir(dst.parent)
    shutil.copyfile(src, dst)


def choose_model_yaml_path(root: Path) -> Path:
    for name in ("layers_gemmini.yaml", "layers.yaml", "model.layers.yaml"):
        candidate = root / name
        if candidate.is_file():
            return candidate
    raise RuntimeError(f"no model yaml found under {root}")


def flatten_raw_mapping_candidate(candidate: Dict[str, Any], index: int, layer: Any, source_path: Path) -> Dict[str, Any]:
    target = candidate.get("target", {}) or {}
    mapping = candidate.get("mapping", {}) or {}
    perf = candidate.get("performance", {}) or {}
    others = candidate.get("others", {}) or {}
    target_accel = int(target.get("accel", candidate.get("target_accel", 0)) or 0)
    return {
        "layer_id": int(source_path.stem),
        "op_type": layer_type(layer),
        "target_accel": target_accel,
        "split_kind": layer_to_split_kind(layer, target_accel),
        "source_candidate_index": int(index),
        "source_mapping": str(source_path),
        "mapping_tile": to_int_list(mapping.get("tile", [])),
        "mapping_dram_bypass": to_int_list(mapping.get("dramBypass", [])),
        "mapping_spm_bypass": to_int_list(mapping.get("spmBypass", [])),
        "performance_accel_util": int(perf.get("accelUtil", 0) or 0),
        "performance_spm_util": int(perf.get("spmUtil", 0) or 0),
        "performance_spm_page_util": int(perf.get("spmPageUtil", 0) or 0),
        "performance_dram_access": int(perf.get("dramAccess", 0) or 0),
        "others_spm_tensor_page_count": to_int_list(others.get("spmTensorPageCount", [])),
        "others_spm_tensor_util": to_int_list(others.get("spmTensorUtil", [])),
        "others_total_tiles": int(others.get("totalTiles", 0) or 0),
        "others_spm_dimensions": to_int_list(others.get("spmDimensions", [])),
        "others_spm_tensor_addr": to_int_list(others.get("spmTensorAddr", [])),
        "others_first_tensor_page_num": to_int_list(others.get("firstTensorPageNum", [])),
    }


def flatten_legacy_mapping_candidate(
    layer_id: int, candidate: Dict[str, Any], layer: Any, source_path: Path
) -> Dict[str, Any]:
    target_accel = int(candidate.get("target_accel", 0) or 0)
    return {
        "layer_id": int(layer_id),
        "op_type": layer_type(layer),
        "target_accel": target_accel,
        "split_kind": layer_to_split_kind(layer, target_accel),
        "source_candidate_index": int(candidate.get("source_candidate_index", 0) or 0),
        "source_mapping": str(candidate.get("source_mapping", source_path)),
        "mapping_tile": to_int_list(candidate.get("mapping_tile", [])),
        "mapping_dram_bypass": to_int_list(candidate.get("mapping_dram_bypass", [])),
        "mapping_spm_bypass": to_int_list(candidate.get("mapping_spm_bypass", [])),
        "performance_accel_util": int(candidate.get("performance_accel_util", 0) or 0),
        "performance_spm_util": int(candidate.get("performance_spm_util", 0) or 0),
        "performance_spm_page_util": int(candidate.get("performance_spm_page_util", 0) or 0),
        "performance_dram_access": int(candidate.get("performance_dram_access", 0) or 0),
        "others_spm_tensor_page_count": to_int_list(candidate.get("others_spm_tensor_page_count", [])),
        "others_spm_tensor_util": to_int_list(candidate.get("others_spm_tensor_util", [])),
        "others_total_tiles": int(candidate.get("others_total_tiles", 0) or 0),
        "others_spm_dimensions": to_int_list(candidate.get("others_spm_dimensions", [])),
        "others_spm_tensor_addr": to_int_list(candidate.get("others_spm_tensor_addr", [])),
        "others_first_tensor_page_num": to_int_list(candidate.get("others_first_tensor_page_num", [])),
    }


def dedup_mapping_entries(entries: Iterable[Dict[str, Any]]) -> List[Dict[str, Any]]:
    picked: Dict[tuple, Dict[str, Any]] = {}
    for entry in entries:
        key = (
            entry["layer_id"],
            entry["target_accel"],
            tuple(entry["mapping_dram_bypass"]),
            tuple(entry["mapping_spm_bypass"]),
        )
        score = (
            entry["performance_spm_page_util"],
            entry["performance_dram_access"],
            entry["performance_spm_util"],
            entry["performance_accel_util"],
            entry["source_candidate_index"],
        )
        prev = picked.get(key)
        if prev is None or score < prev["_score"]:
            chosen = dict(entry)
            chosen["_score"] = score
            picked[key] = chosen

    result: List[Dict[str, Any]] = []
    for entry in picked.values():
        entry = dict(entry)
        entry.pop("_score", None)
        result.append(entry)
    result.sort(
        key=lambda item: (
            item["layer_id"],
            item["target_accel"],
            item["performance_spm_page_util"],
            item["performance_dram_access"],
            item["source_candidate_index"],
        )
    )
    return result


def write_runtime_layer_mapping(runtime_dir: Path, target: RuntimeHardwareTarget, source_mode: str, entries: List[Dict[str, Any]]):
    out_doc = {
        "schema_version": 1,
        "interface": "pipeline_runtime",
        "target_key": target.target_key,
        "source_mode": source_mode,
        "spm_address_semantics": "local_zero_based_offset",
        "spm_vpage_semantics": "local_zero_based_virtual_page",
        "runtime_rebase_required": True,
        "target": target.as_dict(),
        "entries": dedup_mapping_entries(entries),
    }
    out_path = runtime_dir / runtime_layer_mapping_filename(target)
    dump_yaml(out_path, out_doc)
    return out_path


def emit_runtime_layer_mapping_from_fresh(layers: Sequence[Any], mapping_dir: Path, runtime_dir: Path, target: RuntimeHardwareTarget):
    entries: List[Dict[str, Any]] = []
    for mapping_path in sorted(mapping_dir.glob("*.yaml")):
        if not mapping_path.stem.isdigit():
            continue
        layer_idx = int(mapping_path.stem)
        layer = layers[layer_idx]
        doc = load_yaml(mapping_path) or {}
        for index, candidate in enumerate(doc.get("candidates", []) or []):
            if isinstance(candidate, dict):
                entries.append(flatten_raw_mapping_candidate(candidate, index, layer, mapping_path))
    return write_runtime_layer_mapping(runtime_dir, target, "fresh", entries)


def emit_runtime_layer_mapping_from_legacy(
    legacy_dir: Path, layer_map: Dict[int, Any], runtime_dir: Path, target: RuntimeHardwareTarget
):
    entries: List[Dict[str, Any]] = []
    mapping_dir = legacy_dir / "mapping_gemmini"
    for mapping_path in sorted(mapping_dir.glob("*.yaml")):
        if not mapping_path.stem.isdigit():
            continue
        doc = load_yaml(mapping_path) or {}
        layer_id = int(value_or_default(doc.get("layer_id", mapping_path.stem), mapping_path.stem))
        layer = layer_map[layer_id]
        for candidate in doc.get("candidates", []) or []:
            if isinstance(candidate, dict):
                entries.append(flatten_legacy_mapping_candidate(layer_id, candidate, layer, mapping_path))
    return write_runtime_layer_mapping(runtime_dir, target, "legacy", entries)


def update_stage_runtime_fields(
    stage: Dict[str, Any], layer: Any, stage_id: int, target: RuntimeHardwareTarget
) -> None:
    acc_util = int(stage.get("accUtil", stage.get("acc_util", 0)) or 0)
    if acc_util <= 0:
        raise RuntimeError(f"invalid accUtil for stage {stage_id}")
    if acc_util > target.num_gemmini:
        raise RuntimeError(
            f"stage {stage_id} accUtil={acc_util} exceeds target num_gemmini={target.num_gemmini}"
        )
    stage["globalStageId"] = int(stage_id)
    stage["splitKind"] = layer_to_split_kind(layer, acc_util)
    if "vAccIdxList" not in stage or not stage["vAccIdxList"]:
        stage["vAccIdxList"] = [list(range(acc_util))]
    if "pAccIdxList" not in stage or stage["pAccIdxList"] is None:
        stage["pAccIdxList"] = [[]]

    explicit_stage_phys = [int(v) for v in stage.get("physicalAccIds", []) or []]
    if explicit_stage_phys:
        if len(explicit_stage_phys) != acc_util:
            raise RuntimeError(
                f"stage {stage_id} physicalAccIds size {len(explicit_stage_phys)} != accUtil {acc_util}"
            )
        if any(v < 0 or v >= target.num_gemmini for v in explicit_stage_phys):
            raise RuntimeError(f"stage {stage_id} physicalAccIds out of target range")

    p_acc_rows = stage.get("pAccIdxList", [[]]) or [[]]
    first_p_acc = []
    if p_acc_rows:
        first_row = p_acc_rows[0] or []
        first_p_acc = [int(v) for v in first_row]
    if first_p_acc:
        if len(first_p_acc) != acc_util:
            raise RuntimeError(
                f"stage {stage_id} pAccIdxList[0] size {len(first_p_acc)} != accUtil {acc_util}"
            )
        if any(v < 0 or v >= target.num_gemmini for v in first_p_acc):
            raise RuntimeError(f"stage {stage_id} pAccIdxList[0] out of target range")
    if explicit_stage_phys and first_p_acc and explicit_stage_phys != first_p_acc:
        raise RuntimeError(
            f"stage {stage_id} physicalAccIds and pAccIdxList[0] disagree: {explicit_stage_phys} vs {first_p_acc}"
        )


def index_runtime_layer_mapping_entries(runtime_layer_mapping_doc: Dict[str, Any]) -> Dict[int, List[Dict[str, Any]]]:
    by_layer: Dict[int, List[Dict[str, Any]]] = {}
    for entry in runtime_layer_mapping_doc.get("entries", []) or []:
        if not isinstance(entry, dict):
            continue
        layer_id = int(value_or_default(entry.get("layer_id", -1), -1))
        if layer_id < 0:
            continue
        by_layer.setdefault(layer_id, []).append(entry)
    return by_layer


def match_stage_runtime_mapping(stage: Dict[str, Any], stage_acc: int, entries: List[Dict[str, Any]]) -> Dict[str, Any]:
    def layout_key(entry: Dict[str, Any]) -> tuple[Any, ...]:
        return (
            tuple(to_int_list(entry.get("others_spm_tensor_addr", []))),
            tuple(to_int_list(entry.get("others_first_tensor_page_num", []))),
            tuple(to_int_list(entry.get("others_spm_tensor_page_count", []))),
            tuple(to_int_list(entry.get("others_spm_tensor_util", []))),
        )

    def collapse_layout_matches(candidates: List[Dict[str, Any]], reason: str) -> Dict[str, Any]:
        if len(candidates) == 1:
            return candidates[0]
        if not candidates:
            raise RuntimeError(reason)
        keys = {layout_key(entry) for entry in candidates}
        if len(keys) == 1:
            return candidates[0]
        raise RuntimeError(reason)

    layer_ids = to_int_list(stage.get("layerIdList", []))
    if len(layer_ids) != 1:
        raise RuntimeError(f"runtime contract requires one layer per stage, got {layer_ids}")
    dram_rows = stage.get("dramBypassList", []) or [[]]
    spm_rows = stage.get("spmBypassList", []) or [[]]
    stage_dram = to_int_list(dram_rows[0] if dram_rows else [])
    stage_spm = to_int_list(spm_rows[0] if spm_rows else [])
    stage_split = str(stage.get("splitKind", "") or "")
    matches = []
    for entry in entries:
        if int(value_or_default(entry.get("layer_id", -1), -1)) != layer_ids[0]:
            continue
        if int(entry.get("target_accel", 0) or 0) != stage_acc:
            continue
        if to_int_list(entry.get("mapping_dram_bypass", [])) != stage_dram:
            continue
        if to_int_list(entry.get("mapping_spm_bypass", [])) != stage_spm:
            continue
        entry_split = str(entry.get("split_kind", "") or "")
        if stage_split and entry_split and stage_split != entry_split:
            continue
        matches.append(entry)
    if matches:
        return collapse_layout_matches(
            matches,
            f"expected exactly one runtime layer mapping for layer={layer_ids[0]} acc={stage_acc}, got {len(matches)}",
        )

    expected_pages = to_int_list(stage.get("_expected_local_spm_page_count", []))
    if expected_pages:
        relaxed_matches = []
        for entry in entries:
            if int(value_or_default(entry.get("layer_id", -1), -1)) != layer_ids[0]:
                continue
            if to_int_list(entry.get("others_spm_tensor_page_count", [])) != expected_pages:
                continue
            relaxed_matches.append(entry)
        if relaxed_matches:
            return collapse_layout_matches(
                relaxed_matches,
                f"expected a unique runtime layout match for layer={layer_ids[0]} expected_pages={expected_pages}, got {len(relaxed_matches)}",
            )

    raise RuntimeError(
        f"expected exactly one runtime layer mapping for layer={layer_ids[0]} acc={stage_acc}, got 0; expected_pages={expected_pages}"
    )


def build_segment_runtime_layout(seg: Dict[str, Any], layer_mapping_by_layer: Dict[int, List[Dict[str, Any]]]) -> None:
    next_buffer_id = 1
    next_alias_group_id = 1
    segment_span = 0
    stages = seg.get("stages", []) or []
    shared_groups: Dict[int, Dict[str, int]] = {}
    buffer_bindings: List[Dict[str, int | str]] = []

    for stage_local_id, stage_group in enumerate(stages):
        if not isinstance(stage_group, list) or len(stage_group) != 1 or not isinstance(stage_group[0], dict):
            raise RuntimeError("runtime contract requires each stage group to contain exactly one mapping stage")
        stage = stage_group[0]
        layer_ids = to_int_list(stage.get("layerIdList", []))
        if len(layer_ids) != 1:
            raise RuntimeError(f"runtime contract requires one layer per stage, got {layer_ids}")
        stage_acc = int(stage.get("accUtil", stage.get("acc_util", 0)) or 0)
        tensor_ids = to_int_list(stage.get("tensorIdList", []))
        stage_spm_util_rows = seg.get("tensor_spm_util_in_stage", []) or []
        stage_spm_util = (
            {int(k): int(v) for k, v in (stage_spm_util_rows[stage_local_id] or {}).items()}
            if stage_local_id < len(stage_spm_util_rows) and isinstance(stage_spm_util_rows[stage_local_id], dict)
            else {}
        )
        weight_spm_util = {int(k): int(v) for k, v in (seg.get("tensor_spm_util_weight", {}) or {}).items()}
        shared_spm_util = {int(k): int(v) for k, v in (seg.get("tensor_spm_util_shared", {}) or {}).items()}
        entry_tensor_ids = to_int_list(stage.get("entryTensorIdList", []))
        entry_tensor_types = [str(v) for v in (stage.get("entryTensorTypeList", []) or [])]
        export_tensor_ids = to_int_list(stage.get("exportTensorIdList", []))
        export_tensor_types = [str(v) for v in (stage.get("exportTensorTypeList", []) or [])]
        shared_tensor_ids = {
            tid
            for tid, tensor_type in list(zip(entry_tensor_ids, entry_tensor_types)) + list(zip(export_tensor_ids, export_tensor_types))
            if tensor_type == "SHARED_SPM"
        }
        expected_page_count = []
        for tensor_id in tensor_ids:
            pages = int(weight_spm_util.get(int(tensor_id), 0) or 0)
            if pages <= 0:
                pages = int(stage_spm_util.get(int(tensor_id), 0) or 0)
            if pages <= 0 and int(tensor_id) in shared_tensor_ids:
                pages = int(shared_spm_util.get(int(tensor_id), 0) or 0)
            expected_page_count.append(max(0, pages))
        stage["_expected_local_spm_page_count"] = expected_page_count
        mapping_entry = match_stage_runtime_mapping(stage, stage_acc, layer_mapping_by_layer.get(layer_ids[0], []))
        local_addr = to_int_list(mapping_entry.get("others_spm_tensor_addr", []))
        local_first_vpage = to_int_list(mapping_entry.get("others_first_tensor_page_num", []))
        local_page_count = to_int_list(mapping_entry.get("others_spm_tensor_page_count", []))
        local_tensor_bytes = to_int_list(mapping_entry.get("others_spm_tensor_util", []))
        if not (
            len(tensor_ids)
            == len(local_addr)
            == len(local_first_vpage)
            == len(local_page_count)
            == len(local_tensor_bytes)
        ):
            raise RuntimeError(
                f"stage layer={layer_ids[0]} local layout length mismatch tensorIds={len(tensor_ids)} "
                f"addr={len(local_addr)} first={len(local_first_vpage)} pages={len(local_page_count)} bytes={len(local_tensor_bytes)}"
            )
        stage_span = 0
        for first_vpage, page_count in zip(local_first_vpage, local_page_count):
            stage_span = max(stage_span, int(first_vpage) + int(page_count))
        stage["execBaseVPage"] = int(segment_span)
        stage["localSpmTensorAddrList"] = local_addr
        stage["localSpmFirstVPageList"] = local_first_vpage
        stage["localSpmPageCountList"] = local_page_count
        stage["localSpmTensorBytesList"] = local_tensor_bytes
        stage["localSpmPageSpan"] = int(stage_span)
        segment_span += stage_span

        entry_buffer_ids: List[int] = []
        export_buffer_ids: List[int] = []
        stage["_entry_buffer_ids"] = entry_buffer_ids
        stage["_export_buffer_ids"] = export_buffer_ids
        stage["_local_stage_id"] = int(stage_local_id)

        for tensor_type_list_name, tensor_id_list_name, tensor_db_list_name, is_entry in (
            ("entryTensorTypeList", "entryTensorIdList", "entryTensorDoubleBufferList", 1),
            ("exportTensorTypeList", "exportTensorIdList", "exportTensorDoubleBufferList", 0),
        ):
            tensor_types = list(stage.get(tensor_type_list_name, []) or [])
            tensor_ids_role = to_int_list(stage.get(tensor_id_list_name, []))
            tensor_dbuf = to_int_list(stage.get(tensor_db_list_name, []))
            if len(tensor_types) != len(tensor_ids_role) or len(tensor_dbuf) != len(tensor_ids_role):
                raise RuntimeError(f"stage layer={layer_ids[0]} tensor binding length mismatch for {tensor_id_list_name}")
            for tensor_id, tensor_type, double_buffer in zip(tensor_ids_role, tensor_types, tensor_dbuf):
                tensor_idx = tensor_ids.index(int(tensor_id))
                slot_count = 2 if int(double_buffer) else 1
                pages_per_slot = int(local_page_count[tensor_idx])
                alias_group_id = 0
                if str(tensor_type) == "SHARED_SPM":
                    info = shared_groups.setdefault(
                        int(tensor_id),
                        {"slot_count": 1, "pages_per_slot": 0, "alias_group_id": next_alias_group_id},
                    )
                    if info["alias_group_id"] == next_alias_group_id:
                        next_alias_group_id += 1
                    info["slot_count"] = max(info["slot_count"], slot_count)
                    info["pages_per_slot"] = max(info["pages_per_slot"], pages_per_slot)
                    alias_group_id = int(info["alias_group_id"])
                    slot_count = int(info["slot_count"])
                    pages_per_slot = int(info["pages_per_slot"])
                if str(tensor_type) == "ALL_RINGBUFFER":
                    slot_count = 0
                    pages_per_slot = 0
                buffer_bindings.append(
                    {
                        "buffer_id": next_buffer_id,
                        "tensor_id": int(tensor_id),
                        "stage_local_id": int(stage_local_id),
                        "is_entry": int(is_entry),
                        "kind": "PIPE",
                        "slot_count": int(slot_count),
                        "pages_per_slot": int(pages_per_slot),
                        "alias_group_id": int(alias_group_id),
                    }
                )
                if is_entry:
                    entry_buffer_ids.append(next_buffer_id)
                else:
                    export_buffer_ids.append(next_buffer_id)
                next_buffer_id += 1

        fix_tensor_ids = to_int_list(stage.get("fixTensorDramBypassIdList", []))
        for tensor_id in fix_tensor_ids:
            tensor_idx = tensor_ids.index(int(tensor_id))
            pages_per_slot = int(local_page_count[tensor_idx])
            if pages_per_slot <= 0:
                continue
            buffer_bindings.append(
                {
                    "buffer_id": next_buffer_id,
                    "tensor_id": int(tensor_id),
                    "stage_local_id": int(stage_local_id),
                    "is_entry": 0,
                    "kind": "WEIGHT",
                    "slot_count": 1,
                    "pages_per_slot": pages_per_slot,
                    "alias_group_id": 0,
                }
            )
            next_buffer_id += 1

    ring_count = {int(k): int(v) for k, v in (seg.get("ring_buffer_count", {}) or {}).items()}
    ring_size_per = {int(k): int(v) for k, v in (seg.get("ring_buffer_size_per", {}) or {}).items()}
    ring_total_pages = {int(k): int(v) for k, v in (seg.get("tensor_spm_util_in_ringbuffer", {}) or {}).items()}
    for tensor_id, count in sorted(ring_count.items()):
        if count <= 0:
            continue
        pages_per_slot = int(ring_size_per.get(tensor_id, 0) or 0)
        if pages_per_slot <= 0:
            total_pages = int(ring_total_pages.get(tensor_id, 0) or 0)
            pages_per_slot = (total_pages + count - 1) // count if total_pages > 0 else 0
        buffer_bindings.append(
            {
                "buffer_id": next_buffer_id,
                "tensor_id": int(tensor_id),
                "stage_local_id": 0xFFFFFFFF,
                "is_entry": 0,
                "kind": "RING",
                "slot_count": int(count),
                "pages_per_slot": int(pages_per_slot),
                "alias_group_id": 0,
            }
        )
        next_buffer_id += 1

    seg["segmentSpmPageSpan"] = int(segment_span)
    seg["bufferBindingIdList"] = [int(item["buffer_id"]) for item in buffer_bindings]
    seg["bufferBindingTensorIdList"] = [int(item["tensor_id"]) for item in buffer_bindings]
    seg["bufferBindingStageLocalIdList"] = [int(item["stage_local_id"]) for item in buffer_bindings]
    seg["bufferBindingIsEntryList"] = [int(item["is_entry"]) for item in buffer_bindings]
    seg["bufferBindingKindList"] = [str(item["kind"]) for item in buffer_bindings]
    seg["bufferBindingSlotCountList"] = [int(item["slot_count"]) for item in buffer_bindings]
    seg["bufferBindingPagesPerSlotList"] = [int(item["pages_per_slot"]) for item in buffer_bindings]
    seg["bufferBindingAliasGroupIdList"] = [int(item["alias_group_id"]) for item in buffer_bindings]

    for stage_group in stages:
        stage = stage_group[0]
        stage["entryBufferIdList"] = list(stage.pop("_entry_buffer_ids", []))
        stage["exportBufferIdList"] = list(stage.pop("_export_buffer_ids", []))
        stage.pop("_expected_local_spm_page_count", None)
        stage.pop("_local_stage_id", None)


def transform_pipeline_for_runtime(
    layer_map: Dict[int, Any],
    runtime_layer_mapping_doc: Dict[str, Any],
    pipeline_doc: Dict[str, Any],
    source_path: Path,
    target: RuntimeHardwareTarget,
    source_mode: str,
    method: str,
):
    global_stage_id = 0
    out_doc = {
        "schema_version": 1,
        "interface": "pipeline_runtime",
        "target_key": target.target_key,
        "source_mode": source_mode,
        "source_method": method,
        "source_pipeline": str(source_path),
        "target": target.as_dict(),
        "cost": int(pipeline_doc.get("cost", 0) or 0),
        "segments": [],
    }
    layer_mapping_by_layer = index_runtime_layer_mapping_entries(runtime_layer_mapping_doc)

    for seg_idx, seg in enumerate(pipeline_doc.get("segments", []) or []):
        out_seg = dict(seg)
        out_seg["segment_idx"] = int(value_or_default(seg.get("segment_idx", seg_idx), seg_idx))
        out_stages = []
        for stage_group in seg.get("stages", []) or []:
            if not isinstance(stage_group, list) or len(stage_group) != 1:
                raise RuntimeError("runtime contract requires each stage group to contain exactly one layer")
            stage = dict(stage_group[0])
            layer_ids = to_int_list(stage.get("layerIdList", []))
            if len(layer_ids) != 1:
                raise RuntimeError(f"runtime contract requires one layer per stage, got {layer_ids}")
            layer = layer_map[layer_ids[0]]
            update_stage_runtime_fields(stage, layer, global_stage_id, target)
            global_stage_id += 1
            out_stages.append([stage])
        out_seg["stages"] = out_stages
        build_segment_runtime_layout(out_seg, layer_mapping_by_layer)
        out_doc["segments"].append(out_seg)
    return out_doc


def emit_runtime_pipeline(
    layer_map: Dict[int, Any],
    runtime_dir: Path,
    pipeline_doc: Dict[str, Any],
    source_path: Path,
    target: RuntimeHardwareTarget,
    method: str,
    source_mode: str,
):
    runtime_layer_mapping = load_yaml(runtime_dir / runtime_layer_mapping_filename(target)) or {}
    runtime_doc = transform_pipeline_for_runtime(
        layer_map, runtime_layer_mapping, pipeline_doc, source_path, target, source_mode, method
    )
    out_path = runtime_dir / runtime_pipeline_filename(target, method)
    dump_yaml(out_path, runtime_doc)
    return out_path


def emit_runtime_model_yaml(model_yaml_src: Path, runtime_dir: Path):
    out_path = runtime_dir / "model.layers.yaml"
    copy_file(model_yaml_src, out_path)
    return out_path


def emit_runtime_hardware(runtime_dir: Path, target: RuntimeHardwareTarget):
    out_path = runtime_dir / runtime_hardware_filename(target)
    dump_yaml(
        out_path,
        {
            "schema_version": 1,
            "interface": "pipeline_runtime",
            "target_key": target.target_key,
            "target": target.as_dict(),
        },
    )
    return out_path


def emit_runtime_data_with_dummy(runtime_dir: Path, model_name: str, target: RuntimeHardwareTarget, canonical_pipeline: Path):
    script = HYBRIDMAPPER_ROOT / "scripts" / "create-pipeline-dummy-runtime-data.py"
    cmd = [
        sys.executable,
        str(script),
        "--model",
        model_name,
        "--layers-yaml",
        str(runtime_dir / "model.layers.yaml"),
        "--pipeline-yaml",
        str(canonical_pipeline),
        "--output-root",
        str(runtime_dir.parent.relative_to(HYBRIDMAPPER_ROOT)),
    ]
    subprocess.run(cmd, cwd=str(HYBRIDMAPPER_ROOT), check=True)

    dummy_model = runtime_dir / "dummy_weight" / "model.bin"
    dummy_input = runtime_dir / "dummy_input" / "input.bin"
    runtime_model = runtime_dir / "runtime_model.bin"
    runtime_input = runtime_dir / runtime_input_filename(target)
    copy_file(dummy_model, runtime_model)
    copy_file(dummy_input, runtime_input)
    return runtime_model, runtime_input


def emit_runtime_data_from_legacy(
    legacy_dir: Path, runtime_dir: Path, model_name: str, target: RuntimeHardwareTarget, canonical_pipeline: Path
):
    legacy_model = legacy_dir / "model.bin"
    legacy_input = legacy_dir / "input.bin"
    runtime_model = runtime_dir / "runtime_model.bin"
    runtime_input = runtime_dir / runtime_input_filename(target)
    if legacy_model.is_file() and legacy_input.is_file():
        copy_file(legacy_model, runtime_model)
        copy_file(legacy_input, runtime_input)
        return runtime_model, runtime_input
    return emit_runtime_data_with_dummy(runtime_dir, model_name, target, canonical_pipeline)


def emit_runtime_graph_partition_from_fresh(
    hm: Dict[str, Any], graph_record: Any, runtime_dir: Path, target: RuntimeHardwareTarget
):
    key = hm["GraphPartitionCandidate"].KEY
    candidates = []
    for candidate in graph_record.candidate_list:
        node = candidate.target_node
        if node[key.start_layer_idx] != 0:
            continue
        if node[key.acc_num] != target.num_gemmini:
            continue
        if node[key.default_batch] != target.default_batch:
            continue
        if node[key.dram_bw_per_cycle] != target.dram_bw_per_cycle:
            continue
        if node[key.noc_bw_per_cycle] != target.noc_bw_per_cycle:
            continue
        candidates.append(candidate.node)
    if not candidates:
        raise RuntimeError(f"no graph partition candidates emitted for {target.target_key}")

    out_path = runtime_dir / runtime_graph_partition_filename(target)
    dump_yaml(
        out_path,
        {
            "schema_version": 1,
            "interface": "pipeline_runtime",
            "target_key": target.target_key,
            "source_mode": "fresh",
            "target": target.as_dict(),
            "candidates": candidates,
        },
    )
    return out_path


def emit_runtime_graph_partition_from_legacy(
    pipeline_docs: Dict[str, Dict[str, Any]], pipeline_paths: Dict[str, Path], runtime_dir: Path, target: RuntimeHardwareTarget
):
    candidates = []
    for method in METHODS:
        if method not in pipeline_docs:
            continue
        pipeline_doc = pipeline_docs[method]
        segments = pipeline_doc.get("segments", []) or []
        if not segments:
            continue
        start_layer = int(value_or_default(segments[0].get("start_layer_idx", 0), 0))
        end_layer = int(value_or_default(segments[-1].get("end_layer_idx", start_layer), start_layer))
        candidates.append(
            {
                "synthetic": True,
                "method": method,
                "source_pipeline": str(pipeline_paths[method]),
                "start_layer_idx": start_layer,
                "end_layer_idx": end_layer,
                "acc_num": target.num_gemmini,
                "default_batch": target.default_batch,
                "dram_bw_per_cycle": target.dram_bw_per_cycle,
                "noc_bw_per_cycle": target.noc_bw_per_cycle,
                "segment_count": len(segments),
                "cost": int(pipeline_doc.get("cost", 0) or 0),
                "segments": [
                    {
                        "segment_idx": int(value_or_default(seg.get("segment_idx", idx), idx)),
                        "start_layer_idx": int(value_or_default(seg.get("start_layer_idx", start_layer), start_layer)),
                        "end_layer_idx": int(value_or_default(seg.get("end_layer_idx", end_layer), end_layer)),
                        "acc_util": int(seg.get("acc_util", 0) or 0),
                        "cost": int(seg.get("cost", 0) or 0),
                        "stage_count": len(seg.get("stages", []) or []),
                    }
                    for idx, seg in enumerate(segments)
                ],
            }
        )
    if not candidates:
        raise RuntimeError(f"no pipeline docs available to synthesize graph partition for {target.target_key}")

    out_path = runtime_dir / runtime_graph_partition_filename(target)
    dump_yaml(
        out_path,
        {
            "schema_version": 1,
            "interface": "pipeline_runtime",
            "target_key": target.target_key,
            "source_mode": "legacy",
            "synthetic": True,
            "target": target.as_dict(),
            "candidates": candidates,
        },
    )
    return out_path


def create_or_load_model(hm: Dict[str, Any], model_name: str, intermediate_dir: Path):
    layers_yaml = intermediate_dir / "layers.yaml"
    layers_dump = intermediate_dir / "layers.dump"
    if layers_yaml.exists() and layers_dump.exists():
        return hm["load_model"](str(layers_dump))

    model = hm["models"].create(model_name, True)
    if not model.checkLayerGroup():
        raise RuntimeError(f"model {model_name} failed checkLayerGroup")
    model.generateMemoryMapping()
    ensure_dir(intermediate_dir)
    model.write(str(layers_yaml))
    model.dump(str(layers_dump))
    return model


def ensure_mapping(hm: Dict[str, Any], model: Any, intermediate_dir: Path):
    mapping_dir = intermediate_dir / "mapping"
    if mapping_dir.is_dir() and any(mapping_dir.glob("*.yaml")):
        return mapping_dir
    ensure_dir(mapping_dir)
    hm["HybridMapper"](model.getProblemList(), hm["get_mapping_target"](), str(mapping_dir)).run()
    return mapping_dir


def build_target_object(hm: Dict[str, Any], target: RuntimeHardwareTarget):
    return hm["Target"](
        numArrays=target.num_gemmini,
        spmKB=target.per_acc_spm_kb * target.num_gemmini,
        enablePipeline=True,
        num_macs_per_array=target.num_macs_per_array,
        dram_bw_per_cycle=target.dram_bw_per_cycle,
        noc_bw_per_cycle=target.noc_bw_per_cycle,
    )


def ensure_graph_partition(hm: Dict[str, Any], model: Any, mapping_dir: Path, intermediate_dir: Path, target: RuntimeHardwareTarget):
    graph_yaml = intermediate_dir / "model_partition.yaml"
    graph_dump = intermediate_dir / "model_partition.dump"
    if graph_yaml.exists() and graph_dump.exists():
        with graph_dump.open("rb") as f:
            return pickle.load(f)

    model.load_layer_mapping(str(mapping_dir))
    partitioner = hm["GraphPartitioner"](
        model=model,
        target=build_target_object(hm, target),
        default_batch=target.default_batch,
    )
    record, _ = partitioner.partition(start_layer_idx=0)
    record.write(str(graph_yaml))
    with graph_dump.open("wb") as f:
        pickle.dump(record, f)
    return record


def find_full_model_candidate(hm: Dict[str, Any], record: Any, model: Any, target: RuntimeHardwareTarget):
    key = hm["GraphPartitionCandidate"].KEY
    for candidate in record.candidate_list:
        node = candidate.target_node
        if node[key.start_layer_idx] != 0:
            continue
        if node[key.end_layer_idx] != model.getNumLayers() - 1:
            continue
        if node[key.acc_num] != target.num_gemmini:
            continue
        if node[key.default_batch] != target.default_batch:
            continue
        if node[key.dram_bw_per_cycle] != target.dram_bw_per_cycle:
            continue
        if node[key.noc_bw_per_cycle] != target.noc_bw_per_cycle:
            continue
        return candidate
    raise RuntimeError(f"no full-model graph partition candidate for {target.target_key}")


def run_sa_for_method(
    hm: Dict[str, Any], model: Any, mapping_dir: Path, base_candidate: Any, target: RuntimeHardwareTarget, method: str
):
    pipeline_target = hm["PipelineTarget"](
        model=model,
        layer_mapping_dir_path=str(mapping_dir),
        start_layer_idx=0,
        end_layer_idx=model.getNumLayers() - 1,
        default_batch=target.default_batch,
        target=build_target_object(hm, target),
        remote_layer_compute_ratio=1.1,
        resource_util_ratio=0,
        enable_decoupling=False if method in ("tangram2", "gemini2") else True,
    )

    common_kwargs = {
        "pipeline_target": pipeline_target,
        "partition_plan": base_candidate.node[hm["GraphPartitionCandidate"].KEY.partition_plan],
        "acc_plan": base_candidate.node[hm["GraphPartitionCandidate"].KEY.acc_plan],
        "layer_mapping": model._layer_mapping,
        "max_iterations": 1000,
        "enable_partition_search": True,
    }

    if method == "ours2":
        sa = hm["GraphPartitionSimulatedAnnealing"](**common_kwargs)
    elif method == "gemini2":
        sa = hm["GraphPartitionSimulatedAnnealing"](gemini_like=True, **common_kwargs)
    elif method == "tangram2":
        sa = hm["GraphPartitionSimulatedAnnealing"](tangram_like=True, **common_kwargs)
    else:
        raise ValueError(f"unsupported method: {method}")

    sa.anneal()
    best_state, _ = sa.get_best_state()
    final_solution = sa._state_to_solution(best_state)
    if final_solution.get_cost() >= hm["PipelineSolution"].SegmentSolution.SPM_EXCEED_PANITY:
        raise RuntimeError(f"SA result exceeds SPM budget for method={method}")
    return hm["SolutionToRecordFactory"].create_pipeline_record_from_pipeline_solution(final_solution, model)


def ensure_intermediate_pipeline(
    hm: Dict[str, Any],
    model: Any,
    mapping_dir: Path,
    graph_record: Any,
    intermediate_dir: Path,
    target: RuntimeHardwareTarget,
    method: str,
):
    entire_dir = intermediate_dir / "entire_model"
    ensure_dir(entire_dir)
    output_path = entire_dir / old_pipeline_filename(target, method)
    if output_path.exists():
        return output_path

    model.load_layer_mapping(str(mapping_dir))
    base_candidate = find_full_model_candidate(hm, graph_record, model, target)
    pipeline_record = run_sa_for_method(hm, model, mapping_dir, base_candidate, target, method)
    pipeline_record.write(str(output_path))
    return output_path


def build_target_manifest(
    model_name: str,
    target: RuntimeHardwareTarget,
    methods: List[str],
    mode: str,
    source_root: Path,
    runtime_dir: Path,
) -> Dict[str, Any]:
    return {
        "schema_version": 1,
        "interface": "pipeline_runtime",
        "model": model_name,
        "target_key": target.target_key,
        "mode": mode,
        "target": target.as_dict(),
        "source_root": str(source_root),
        "runtime_root": str(runtime_dir),
        "artifacts": {
            "model_yaml": "model.layers.yaml",
            "hardware_yaml": runtime_hardware_filename(target),
            "graph_partition_yaml": runtime_graph_partition_filename(target),
            "layer_mapping_yaml": runtime_layer_mapping_filename(target),
            "runtime_model_bin": "runtime_model.bin",
            "runtime_input_bin": runtime_input_filename(target),
            "pipeline_yaml": {
                method: runtime_pipeline_filename(target, method)
                for method in methods
            },
            "runtime_golden_bin": {
                method: runtime_golden_filename(target, method)
                for method in methods
            },
        },
    }


def write_target_manifest(runtime_dir: Path, target: RuntimeHardwareTarget, manifest: Dict[str, Any]):
    out_path = runtime_dir / runtime_manifest_filename(target)
    dump_yaml(out_path, manifest)
    return out_path


def update_aggregate_manifest(runtime_dir: Path, model_name: str, target_manifests: Dict[str, Dict[str, Any]]):
    out_path = runtime_dir / "manifest.yaml"
    aggregate = {
        "schema_version": 1,
        "interface": "pipeline_runtime",
        "model": model_name,
        "targets": target_manifests,
    }
    if out_path.is_file():
        existing = load_yaml(out_path) or {}
        if isinstance(existing.get("targets"), dict):
            merged = dict(existing["targets"])
            merged.update(target_manifests)
            aggregate["targets"] = merged
    dump_yaml(out_path, aggregate)
    return out_path


def generate_from_fresh(
    model_name: str,
    target: RuntimeHardwareTarget,
    methods: List[str],
    intermediate_root: Path,
    runtime_root: Path,
):
    hm, import_exc = maybe_import_hybridmapper()
    if hm is None:
        raise RuntimeError(f"fresh path unavailable: {import_exc}")

    intermediate_dir = intermediate_root / model_name
    runtime_dir = runtime_root / model_name
    ensure_dir(intermediate_dir)
    ensure_dir(runtime_dir)

    model = create_or_load_model(hm, model_name, intermediate_dir)
    mapping_dir = ensure_mapping(hm, model, intermediate_dir)
    graph_record = ensure_graph_partition(hm, model, mapping_dir, intermediate_dir, target)

    model_yaml_src = choose_model_yaml_path(intermediate_dir)
    emit_runtime_model_yaml(model_yaml_src, runtime_dir)
    emit_runtime_hardware(runtime_dir, target)
    emit_runtime_layer_mapping_from_fresh(model.layerList, mapping_dir, runtime_dir, target)
    emit_runtime_graph_partition_from_fresh(hm, graph_record, runtime_dir, target)

    layer_map = {idx: layer for idx, layer in enumerate(model.layerList)}
    emitted_pipelines = {}
    for method in methods:
        intermediate_pipeline = ensure_intermediate_pipeline(
            hm, model, mapping_dir, graph_record, intermediate_dir, target, method
        )
        emitted_pipelines[method] = emit_runtime_pipeline(
            layer_map,
            runtime_dir,
            load_yaml(intermediate_pipeline) or {},
            intermediate_pipeline,
            target,
            method,
            "fresh",
        )

    canonical_pipeline = emitted_pipelines[methods[0]]
    emit_runtime_data_with_dummy(runtime_dir, model_name, target, canonical_pipeline)

    return runtime_dir, build_target_manifest(model_name, target, methods, "fresh", intermediate_dir, runtime_dir)


def generate_from_legacy(
    model_name: str,
    target: RuntimeHardwareTarget,
    methods: List[str],
    runtime_root: Path,
    legacy_source_dir: str,
):
    legacy_dir = resolve_legacy_source_dir(model_name, legacy_source_dir)
    if legacy_dir is None:
        raise RuntimeError(f"no legacy artifact source found for model {model_name}")

    runtime_dir = runtime_root / model_name
    ensure_dir(runtime_dir)

    model_yaml_src = choose_model_yaml_path(legacy_dir)
    layers = load_layer_specs_from_model_yaml(model_yaml_src)
    layer_map = index_layers(layers)

    emit_runtime_model_yaml(model_yaml_src, runtime_dir)
    emit_runtime_hardware(runtime_dir, target)
    emit_runtime_layer_mapping_from_legacy(legacy_dir, layer_map, runtime_dir, target)

    pipeline_docs: Dict[str, Dict[str, Any]] = {}
    pipeline_paths: Dict[str, Path] = {}
    emitted_pipelines = {}
    for method in methods:
        source_pipeline = legacy_dir / "entire_model" / old_pipeline_filename(target, method)
        if not source_pipeline.is_file():
            raise RuntimeError(f"missing legacy pipeline mapping for {method}: {source_pipeline}")
        pipeline_doc = load_yaml(source_pipeline) or {}
        pipeline_docs[method] = pipeline_doc
        pipeline_paths[method] = source_pipeline
        emitted_pipelines[method] = emit_runtime_pipeline(
            layer_map,
            runtime_dir,
            pipeline_doc,
            source_pipeline,
            target,
            method,
            "legacy",
        )

    emit_runtime_graph_partition_from_legacy(pipeline_docs, pipeline_paths, runtime_dir, target)
    emit_runtime_data_from_legacy(legacy_dir, runtime_dir, model_name, target, emitted_pipelines[methods[0]])

    return runtime_dir, build_target_manifest(model_name, target, methods, "legacy", legacy_dir, runtime_dir)


def ensure_runtime_artifacts(
    model_name: str,
    target: RuntimeHardwareTarget,
    methods: List[str],
    intermediate_root: Path,
    runtime_root: Path,
    mode: str,
    legacy_source_dir: str,
):
    if mode in ("auto", "fresh"):
        try:
            return generate_from_fresh(model_name, target, methods, intermediate_root, runtime_root)
        except Exception as exc:
            if mode == "fresh":
                raise
            print(
                f"[pipeline-runtime-export] fresh path unavailable for {model_name}/{target.target_key}: {exc}",
                file=sys.stderr,
            )

    return generate_from_legacy(model_name, target, methods, runtime_root, legacy_source_dir)


def parse_csv(value: str) -> List[str]:
    return [item.strip() for item in value.split(",") if item.strip()]


def parse_args():
    parser = argparse.ArgumentParser(
        description="Generate pipeline-runtime artifacts from HybridMapper or legacy artifacts."
    )
    parser.add_argument("--model", required=True, help="model name, for example bertmini")
    parser.add_argument(
        "--methods",
        default="ours2,gemini2,tangram2",
        help="comma-separated SA methods",
    )
    parser.add_argument(
        "--target-keys",
        default=STEP1_TARGET.target_key,
        help="comma-separated hardware target keys",
    )
    parser.add_argument(
        "--mode",
        choices=("auto", "fresh", "legacy"),
        default="auto",
        help="auto tries fresh HybridMapper generation first and falls back to legacy overlay artifacts",
    )
    parser.add_argument(
        "--legacy-source-dir",
        default="",
        help="optional explicit legacy artifact directory override",
    )
    parser.add_argument(
        "--intermediate-root",
        default="output/pipeline",
        help="HybridMapper intermediate root relative to conference/HybridMapper",
    )
    parser.add_argument(
        "--runtime-root",
        default="output/pipeline_runtime",
        help="pipeline-runtime artifact root relative to conference/HybridMapper",
    )
    return parser.parse_args()


def main():
    args = parse_args()
    methods = parse_csv(args.methods)
    if not methods:
        raise SystemExit("no methods selected")
    for method in methods:
        if method not in METHODS:
            raise SystemExit(f"unsupported method: {method}")

    target_keys = parse_csv(args.target_keys)
    if not target_keys:
        raise SystemExit("no target keys selected")

    target_manifests: Dict[str, Dict[str, Any]] = {}
    runtime_dir: Path | None = None
    for target_key in target_keys:
        if target_key not in TARGETS:
            raise SystemExit(f"unsupported target key: {target_key}")
        target = TARGETS[target_key]
        runtime_dir, manifest = ensure_runtime_artifacts(
            model_name=args.model,
            target=target,
            methods=methods,
            intermediate_root=HYBRIDMAPPER_ROOT / args.intermediate_root,
            runtime_root=HYBRIDMAPPER_ROOT / args.runtime_root,
            mode=args.mode,
            legacy_source_dir=args.legacy_source_dir,
        )
        target_manifests[target_key] = manifest
        write_target_manifest(runtime_dir, target, manifest)

    if runtime_dir is None:
        raise SystemExit("no runtime artifacts emitted")
    update_aggregate_manifest(runtime_dir, args.model, target_manifests)


if __name__ == "__main__":
    main()
