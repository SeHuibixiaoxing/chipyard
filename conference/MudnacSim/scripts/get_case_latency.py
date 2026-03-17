#!/usr/bin/env python3
"""Parse Total Latency from MudnacSim script output files.

Searches files under `expr/output/expr_single_pipeline/script_output/mudnacsim` (or custom dir)
for filenames containing `HomoTileMesh-{batch_size}` and `_case{N}` and extracts the
`Total Latency: <number>` value from the file contents. Prints a grouped summary by
case and batch_size to stdout.
"""
import argparse
import re
from pathlib import Path
from collections import defaultdict
import statistics


FILENAME_BATCH_RE = re.compile(r'_batch-(\d+)')
FILENAME_CASE_RE = re.compile(r'_case(\d+)')
LATENCY_RE = re.compile(r'Total Latency:\s*(\d+)', re.IGNORECASE)


def parse_file(path: Path):
    text = path.read_text(errors='ignore')
    m = LATENCY_RE.search(text)
    if not m:
        return None
    return int(m.group(1))


def extract_meta_from_name(name: str):
    b = FILENAME_BATCH_RE.search(name)
    c = FILENAME_CASE_RE.search(name)
    batch = int(b.group(1)) if b else None
    case = int(c.group(1)) if c else None
    return batch, case


def collect_latencies(directory: Path):
    results = defaultdict(list)  # key: (case, batch) -> list of latencies
    if not directory.exists():
        raise SystemExit(f"Directory not found: {directory}")

    for p in sorted(directory.iterdir()):
        if not p.is_file():
            continue
        name = p.name
        batch, case = extract_meta_from_name(name)
        if batch is None and case is None:
            # skip files that don't look like our target pattern
            continue
        latency = parse_file(p)
        if latency is None:
            # skip if latency not found
            continue
        results[(case, batch)].append((p.name, latency))

    return results


def print_summary(results):
    if not results:
        print('No matching files or latencies found.')
        return

    # Group by case then batch
    grouped = defaultdict(lambda: defaultdict(list))
    for (case, batch), items in results.items():
        for _, latency in items:
            grouped[case][batch].append(latency)

    print('Summary of Total Latency by case and batch_size:')
    print('-' * 60)
    print(f"{'case':>6} {'batch':>6} {'count':>6} {'min':>10} {'mean':>10} {'median':>10} {'max':>10}")
    print('-' * 60)
    # Iterate by batch first, then by case
    all_batches = set()
    for case_key, batches in grouped.items():
        all_batches.update(batches.keys())

    for batch in sorted(all_batches, key=lambda x: (x is None, x)):
        for case in sorted(grouped.keys(), key=lambda x: (x is None, x)):
            if batch not in grouped[case]:
                continue
            vals = grouped[case][batch]
            cnt = len(vals)
            mn = min(vals)
            mx = max(vals)
            mean = int(statistics.mean(vals))
            med = int(statistics.median(vals))
            case_label = str(case) if case is not None else 'N/A'
            batch_label = str(batch) if batch is not None else 'N/A'
            print(f"{case_label:>6} {batch_label:>6} {cnt:6d} {mn:10d} {mean:10d} {med:10d} {mx:10d}")


def main():
    p = argparse.ArgumentParser(description='Collect Total Latency from MudnacSim outputs')
    p.add_argument('--dir', '-d', default='expr/output/expr_single_pipeline/script_output/mudnacsim',
                   help='Directory to scan')
    p.add_argument('--show-files', action='store_true', help='Also print per-file latencies')
    args = p.parse_args()

    directory = Path(args.dir)
    results = collect_latencies(directory)

    if args.show_files and results:
        print('Per-file latencies:')
        for (case, batch), items in sorted(
            results.items(),
            key=lambda kv: (
                (kv[0][1] is None, kv[0][1] if kv[0][1] is not None else 0),
                (kv[0][0] is None, kv[0][0] if kv[0][0] is not None else 0),
            ),
        ):
            for fname, lat in items:
                print(f"case={case} batch={batch} file={fname} latency={lat}")
        print()

    print_summary(results)


if __name__ == '__main__':
    main()
