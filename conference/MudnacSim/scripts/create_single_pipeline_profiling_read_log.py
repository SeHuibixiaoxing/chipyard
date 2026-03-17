#!/usr/bin/env python3
"""
Read stdout log files produced by expr_single_pipeline_profiling runs, parse Total Latency,
reconstruct mapping paths from filenames, and write profiling results (profiline.yaml)
under each segment directory.

Assumptions about log filename pattern (stdout logs):
  {model}_{system}_part-{part}_seg-{seg}_{mapping_stem}_noc-{noc}-{noc_flit}_ch-{ch}_
    acc-{acc}_spm-{spm}_batch-{batch}_method-{method}_{pipe_spm}_{pipe_acc}-std.log

Mapping path is reconstructed as:
  models/pipeline/{model}/schedule_point/pipeline_mapping/{part}/{seg}/{mapping_stem}.yaml

Usage (examples):
  python3 scripts/create_single_pipeline_profiling_read_log.py \
      --log-dir expr/output/expr_single_pipeline/script_output \
      --write   # actually write profiline.yaml (otherwise dry-run)

  # Specify custom project root (for mapping path resolution)
  python3 scripts/create_single_pipeline_profiling_read_log.py --project-root /mnt/eda2/wangzy/proj/MudnacSim
"""
import argparse
import re
import sys
from pathlib import Path
import yaml

LATENCY_RE = re.compile(r"Total Latency:\s*(\d+)", re.IGNORECASE)

# Filename regex
FILENAME_RE = re.compile(
    r"^(?P<model>[^_]+)_(?P<system>[^_]+)_part-(?P<part>\d+)_seg-(?P<seg>\d+)_"
    r"(?P<mapstem>.+?)_noc-(?P<noc>[^-]+)-(?P<nocflit>\d+)_ch-(?P<ch>\d+)_"
    r"acc-(?P<acc>\d+)_spm-(?P<spm>\d+)_batch-(?P<batch>\d+)_method-(?P<method>[^_]+)_"
    r"(?P<pipe_spm>[^_]+)_(?P<pipe_acc>[^_.]+)$"
)


def parse_latency(log_path: Path) -> int:
    text = log_path.read_text(errors="ignore")
    m = LATENCY_RE.search(text)
    if not m:
        raise ValueError(f"Cannot find 'Total Latency' in log: {log_path}")
    return int(m.group(1))


def parse_filename(log_path: Path):
    m = FILENAME_RE.match(log_path.name)
    if not m:
        print(f"not match for {log_path.name}")
        return None
    d = m.groupdict()
    # Convert numeric fields
    for k in ["part", "seg", "nocflit", "ch", "acc", "spm", "batch"]:
        d[k] = int(d[k])
    return d


def build_mapping_path(info: dict, project_root: Path) -> Path:
    # models/pipeline/{model}/schedule_point/pipeline_mapping/{part}/{seg}/{mapstem}.yaml
    return project_root / "models" / "pipeline" / info["model"] / "schedule_point" / "pipeline_mapping" / str(info["part"]) / str(info["seg"]) / f"{info['mapstem']}.yaml"


def collect(log_dir: Path, project_root: Path):
    profile_buckets = {}
    bad_files = []
    for log_path in log_dir.glob("*"):
        info = parse_filename(log_path)
        if not info:
            continue
        try:
            latency = parse_latency(log_path)
        except Exception as e:
            bad_files.append((log_path, str(e)))
            continue
        mapping_path = build_mapping_path(info, project_root)
        seg_dir = mapping_path.parent
        entry = {"cost": latency, "mapping_path": str(mapping_path.relative_to(project_root))}
        profile_buckets.setdefault(seg_dir, []).append(entry)
    return profile_buckets, bad_files


def write_profiles(profile_buckets: dict, write: bool):
    for seg_dir, entries in profile_buckets.items():
        profile_path = seg_dir / "profiline.yaml"
        if write:
            seg_dir.mkdir(parents=True, exist_ok=True)
            with open(profile_path, "w") as f:
                yaml.safe_dump(entries, f, sort_keys=False)
        print(f"profiline: {profile_path} ({len(entries)} entries){' [written]' if write else ' [dry-run]'}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--log-dir", default="expr/output/expr_single_pipeline/script_output/mudnacsim", help="Directory containing stdout logs")
    parser.add_argument("--project-root", default=".", help="Project root (for mapping path reconstruction)")
    args = parser.parse_args()

    log_dir = Path(args.log_dir)
    project_root = Path(args.project_root).resolve()

    if not log_dir.exists():
        print(f"log-dir not found: {log_dir}", file=sys.stderr)
        sys.exit(1)

    profile_buckets, bad_files = collect(log_dir, project_root)
    if bad_files:
        print("Warning: some logs could not be parsed for latency:")
        for lp, msg in bad_files:
            print(f"  {lp}: {msg}")

    write_profiles(profile_buckets, True)


if __name__ == "__main__":
    main()
