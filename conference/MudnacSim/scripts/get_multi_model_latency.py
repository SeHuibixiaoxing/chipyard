#!/usr/bin/env python3
"""Parse Total Latency from MudnacSim single model output files.

Scans `expr/output/expr_single_pipeline/script_output/mudnacsim` for files matching the pattern
{modelName}_{system}_dram-{channels}_noc-{noc}-{noc_flit_size}_{acc_num}acc_{spm_bank_size*acc_num}MB_{pipeSpmStrategy}_{pipeAccStrategy}_batch-{totalBatch}_method-{method}
and extracts Total Latency from each file. Outputs to CSV and console, sorted by the specified fields.
"""
import argparse
import re
from pathlib import Path
import csv

# Regex for filename parsing
FILENAME_RE = re.compile(
    # modelName: anything up to the first underscore (allows commas, e.g. "m1,m2")
    # optional suffix: -std.log or -err.log
    r'^(?P<modelName>[^_]+)_(?P<system>\w+)_dram-(?P<channels>\d+)_noc-(?P<noc>[^-]+)-(?P<noc_flit_size>\d+)_(?P<acc_num>\d+)acc_(?P<spm_total_mb>\d+)MB_(?P<pipeSpmStrategy>[^_]+)_(?P<pipeAccStrategy>[^_]+)_batch-(?P<totalBatch>\d+)_method-(?P<method>[^._-]+)(?:-(?:std|err)\.log)?$'
)

LATENCY_RE = re.compile(r'Total Latency:\s*(\d+)', re.IGNORECASE)


def parse_filename(filename: str):
    """Parse filename to extract fields."""
    match = FILENAME_RE.match(filename)
    if not match:
        return None
    data = match.groupdict()
    # Compute spm_bank_size from spm_total_mb and acc_num
    spm_total_mb = int(data['spm_total_mb'])
    acc_num = int(data['acc_num'])
    data['spm_bank_size'] = spm_total_mb // acc_num  # Assuming MB per bank
    return data


def parse_file(path: Path):
    """Extract Total Latency from file."""
    text = path.read_text(errors='ignore')
    m = LATENCY_RE.search(text)
    return int(m.group(1)) if m else None


def collect_data(directory: Path):
    """Collect data from files in directory."""
    data = []
    if not directory.exists():
        raise SystemExit(f"Directory not found: {directory}")

    for p in directory.iterdir():
        if not p.is_file():
            continue
        filename = p.name
        fields = parse_filename(filename)
        if not fields:
            continue
        latency = parse_file(p)
        if latency is None:
            continue
        fields['latency'] = latency
        # Keep only the fields used in CSV and sorting
        fields = {k: fields[k] for k in ['modelName', 'channels', 'noc_flit_size', 'acc_num', 'spm_bank_size', 'pipeSpmStrategy', 'pipeAccStrategy', 'totalBatch', 'method', 'latency']}
        data.append(fields)

    return data


def sort_data(data):
    """Sort data by modelName, channels, noc_flit_size, acc_num, spm_bank_size, pipeSpmStrategy, pipeAccStrategy, totalBatch, method."""
    return sorted(data, key=lambda x: (
        x['modelName'],
        int(x['channels']),
        int(x['noc_flit_size']),
        int(x['acc_num']),
        int(x['spm_bank_size']),
        x['pipeSpmStrategy'],
        x['pipeAccStrategy'],
        int(x['totalBatch']),
        x['method']
    ))


def print_to_console(data):
    """Print data to console."""
    if not data:
        print('No matching files or latencies found.')
        return

    print('Parsed latencies:')
    print('-' * 120)
    print(f"{'modelName':<10} {'channels':<8} {'noc_flit_size':<12} {'acc_num':<7} {'spm_bank_size':<12} {'pipeSpmStrategy':<15} {'pipeAccStrategy':<15} {'totalBatch':<10} {'method':<8} {'latency':<10}")
    print('-' * 120)
    for row in data:
        print(f"{row['modelName']:<10} {row['channels']:<8} {row['noc_flit_size']:<12} {row['acc_num']:<7} {row['spm_bank_size']:<12} {row['pipeSpmStrategy']:<15} {row['pipeAccStrategy']:<15} {row['totalBatch']:<10} {row['method']:<8} {row['latency']:<10}")


def write_to_csv(data, csv_path):
    """Write data to CSV file."""
    if not data:
        return

    fieldnames = ['modelName', 'channels', 'noc_flit_size', 'acc_num', 'spm_bank_size', 'pipeSpmStrategy', 'pipeAccStrategy', 'totalBatch', 'method', 'latency']
    with open(csv_path, 'w', newline='') as csvfile:
        writer = csv.DictWriter(csvfile, fieldnames=fieldnames)
        writer.writeheader()
        for row in data:
            writer.writerow(row)


def main():
    parser = argparse.ArgumentParser(description='Collect Total Latency from MudnacSim single model outputs')
    parser.add_argument('--dir', '-d', default='expr/output/expr_multi_pipeline/script_output/mudnacsim',
                        help='Directory to scan')
    parser.add_argument('--csv', '-c', default='multi_model_latencies.csv',
                        help='Output CSV file path')
    args = parser.parse_args()

    directory = Path(args.dir)
    data = collect_data(directory)
    sorted_data = sort_data(data)

    print_to_console(sorted_data)
    write_to_csv(sorted_data, args.csv)
    print(f"\nData written to {args.csv}")


if __name__ == '__main__':
    main()