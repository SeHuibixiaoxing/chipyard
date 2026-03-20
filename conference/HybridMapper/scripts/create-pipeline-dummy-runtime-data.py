#!/usr/bin/env python3
"""
Generate dummy runtime artifacts for pipeline-runtime validation.

Outputs under:
  output/pipeline/<model>/dummy_weight/model.bin
  output/pipeline/<model>/dummy_input/input.bin
  output/pipeline/<model>/dummy_input/golden/golden.bin
"""

import argparse
import json
import os
import random
import struct
import sys
from pathlib import Path

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
  import yaml as pyyaml


def load_yaml(path: Path):
  with path.open("r", encoding="utf-8") as f:
    if RuamelYAML is not None:
      return RuamelYAML(typ="safe").load(f)
    return pyyaml.safe_load(f)


def to_int_list(v):
  if isinstance(v, list):
    return [int(x) for x in v]
  return []


def signed_i8(x: int) -> int:
  return x - 256 if x > 127 else x


def sat_i8(v: int) -> int:
  if v > 127:
    return 127
  if v < -128:
    return -128
  return v


def normalize_bytes(data, dst_size: int):
  out = bytearray(max(0, int(dst_size)))
  if not out:
    return out
  src = bytearray(data or b"")
  n = min(len(src), len(out))
  if n:
    out[:n] = src[:n]
  return out


def get_local_tensor_size(layer, tensor_index: int, global_tensor_sizes):
  tensor_ids = to_int_list(layer.get("tensorIds", []))
  tensor_sizes = to_int_list(layer.get("tensorSize", []))
  if tensor_index < len(tensor_sizes) and tensor_sizes[tensor_index] > 0:
    return int(tensor_sizes[tensor_index])
  if tensor_index < len(tensor_ids):
    return int(global_tensor_sizes.get(tensor_ids[tensor_index], 0))
  return 0


def infer_source_sink_ids(layers):
  produced = set()
  consumed = set()
  for layer in layers:
    ltype = str(layer.get("type", ""))
    tensor_ids = to_int_list(layer.get("tensorIds", []))
    if ltype == "conv" and len(tensor_ids) >= 4:
      consumed.add(tensor_ids[2])
      produced.add(tensor_ids[3])
    elif ltype == "resadd" and len(tensor_ids) >= 3:
      consumed.add(tensor_ids[0])
      consumed.add(tensor_ids[1])
      produced.add(tensor_ids[2])

    for tid in to_int_list(layer.get("output_ids", [])):
      produced.add(tid)
    for tid in to_int_list(layer.get("input_ids", [])):
      consumed.add(tid)
  source = sorted(consumed - produced)
  sink = sorted(produced - consumed)
  if not sink and layers:
    sink = to_int_list(layers[-1].get("output_ids", []))
  if layers:
    tail = to_int_list(layers[-1].get("output_ids", []))
    if tail:
      tail_id = tail[0]
      sink = [tail_id] + [x for x in sink if x != tail_id]
  return source, sink


def encode_i8_list(vals):
  out = bytearray(len(vals))
  for i, v in enumerate(vals):
    out[i] = int(v) & 0xFF
  return out


def random_i8_bytes(n: int, rng: random.Random):
  vals = [rng.randint(-8, 8) for _ in range(n)]
  return encode_i8_list(vals)


def random_i32_bytes(nbytes: int, rng: random.Random):
  if nbytes <= 0:
    return bytearray()
  count = nbytes // 4
  out = bytearray(count * 4)
  for i in range(count):
    struct.pack_into("<i", out, i * 4, rng.randint(-32, 32))
  if len(out) < nbytes:
    out.extend(random_i8_bytes(nbytes - len(out), rng))
  return out


def parse_bias_i32(bias_buf: bytearray):
  if not bias_buf:
    return [0]
  if len(bias_buf) < 4:
    return [signed_i8(int(bias_buf[0]))]
  n = len(bias_buf) // 4
  vals = []
  for i in range(n):
    vals.append(struct.unpack_from("<i", bias_buf, i * 4)[0])
  return vals if vals else [0]


def pseudo_conv(layer, tensor_data, global_tensor_sizes):
  tensor_ids = to_int_list(layer.get("tensorIds", []))
  if len(tensor_ids) < 4:
    return
  bias_id, weight_id, input_id, output_id = tensor_ids[:4]
  out_size = get_local_tensor_size(layer, 3, global_tensor_sizes)
  if out_size <= 0:
    return

  in_buf = normalize_bytes(tensor_data.get(input_id, bytearray([0])), get_local_tensor_size(layer, 2, global_tensor_sizes))
  w_buf = normalize_bytes(tensor_data.get(weight_id, bytearray([0])), get_local_tensor_size(layer, 1, global_tensor_sizes))
  b_buf = normalize_bytes(tensor_data.get(bias_id, bytearray([0, 0, 0, 0])), get_local_tensor_size(layer, 0, global_tensor_sizes))
  bias_vals = parse_bias_i32(b_buf)

  params = to_int_list(layer.get("param", []))
  oc = params[2] if len(params) > 2 and params[2] > 0 else 1
  ch_stride = max(1, out_size // max(1, oc))

  out = bytearray(out_size)
  ilen = max(1, len(in_buf))
  wlen = max(1, len(w_buf))
  blen = max(1, len(bias_vals))
  for i in range(out_size):
    ch = (i // ch_stride) % max(1, oc)
    a = signed_i8(in_buf[(i + ch * 29) % ilen])
    w = signed_i8(w_buf[(i * 131 + ch * 17) % wlen])
    b = int(bias_vals[ch % blen]) >> 4
    out[i] = sat_i8(a + w + b) & 0xFF
  tensor_data[output_id] = normalize_bytes(out, int(global_tensor_sizes.get(output_id, out_size)))


def pseudo_resadd(layer, tensor_data, global_tensor_sizes):
  tensor_ids = to_int_list(layer.get("tensorIds", []))
  if len(tensor_ids) < 3:
    return
  in0, in1, out_id = tensor_ids[:3]
  out_size = get_local_tensor_size(layer, 2, global_tensor_sizes)
  if out_size <= 0:
    return

  a_buf = normalize_bytes(tensor_data.get(in0, bytearray([0])), out_size)
  b_buf = normalize_bytes(tensor_data.get(in1, bytearray([0])), out_size)
  out = bytearray(out_size)
  for i in range(out_size):
    out[i] = sat_i8(signed_i8(a_buf[i]) + signed_i8(b_buf[i])) & 0xFF
  tensor_data[out_id] = normalize_bytes(out, int(global_tensor_sizes.get(out_id, out_size)))


def parse_pipeline_core_hint(pipeline_yaml):
  if not pipeline_yaml or not pipeline_yaml.exists():
    return 0
  data = load_yaml(pipeline_yaml)
  mx = 0
  for seg in data.get("segments", []) or []:
    if not isinstance(seg, dict):
      continue
    mx = max(mx, int(seg.get("acc_util", 0) or 0))
    for stage_group in seg.get("stages", []) or []:
      if isinstance(stage_group, list):
        for st in stage_group:
          if isinstance(st, dict):
            mx = max(mx, int(st.get("accUtil", 0) or 0))
      elif isinstance(stage_group, dict):
        mx = max(mx, int(stage_group.get("accUtil", 0) or 0))
  return mx


def write_blob_at(blob: bytearray, base: int, end: int, addr: int, data: bytearray):
  span = end - base
  if 0 <= addr < span:
    off = addr
  elif addr >= base and (addr - base) < span:
    off = addr - base
  else:
    raise ValueError(f"address {addr} out of model span [0,{span}) and compat base {base}")
  hi = off + len(data)
  if hi > len(blob):
    raise ValueError(f"write overflow addr={addr} off={off} size={len(data)} blob={len(blob)}")
  blob[off:hi] = data


def main():
  parser = argparse.ArgumentParser(description="Generate dummy model/input/golden from HybridMapper YAML.")
  parser.add_argument("--model", required=True, help="model name under output/pipeline, e.g. bertmini")
  parser.add_argument("--layers-yaml", default="", help="optional layers.yaml path override")
  parser.add_argument("--pipeline-yaml", default="", help="optional pipeline yaml path (for metadata only)")
  parser.add_argument("--output-root", default="output/pipeline", help="pipeline output root")
  parser.add_argument("--seed", type=int, default=20260228, help="random seed")
  args = parser.parse_args()

  repo_root = Path(__file__).resolve().parents[1]
  model_dir = repo_root / args.output_root / args.model
  default_layers_yaml = model_dir / "layers_gemmini.yaml"
  if not default_layers_yaml.exists():
    default_layers_yaml = model_dir / "layers.yaml"
  layers_yaml = Path(args.layers_yaml) if args.layers_yaml else default_layers_yaml
  pipeline_yaml = Path(args.pipeline_yaml) if args.pipeline_yaml else None

  if not layers_yaml.exists():
    raise SystemExit(f"layers.yaml not found: {layers_yaml}")

  model_doc = load_yaml(layers_yaml)
  layers = model_doc.get("layers", []) or []
  addr_range = to_int_list(model_doc.get("address", []))
  if len(addr_range) < 2:
    raise SystemExit(f"invalid top-level address range in {layers_yaml}")

  addr_base = int(addr_range[0])
  addr_end = int(addr_range[1])
  if addr_end <= addr_base:
    raise SystemExit(f"invalid model address range: base={addr_base}, end={addr_end}")
  blob_span = addr_end - addr_base

  rng = random.Random(args.seed)

  tensor_sizes = {}
  for layer in layers:
    ids = to_int_list(layer.get("tensorIds", []))
    sizes = to_int_list(layer.get("tensorSize", []))
    for i, tid in enumerate(ids):
      if i < len(sizes):
        tensor_sizes[tid] = max(tensor_sizes.get(tid, 0), int(sizes[i]))

  tensor_data = {}
  for layer in layers:
    ltype = str(layer.get("type", ""))
    ids = to_int_list(layer.get("tensorIds", []))
    for idx, tid in enumerate(ids):
      if tid in tensor_data:
        continue
      nbytes = int(tensor_sizes.get(tid, 0))
      if nbytes <= 0:
        continue
      if ltype == "conv" and idx == 0:
        tensor_data[tid] = random_i32_bytes(nbytes, rng)
      else:
        tensor_data[tid] = random_i8_bytes(nbytes, rng)

  for layer in layers:
    ltype = str(layer.get("type", ""))
    if ltype == "conv":
      pseudo_conv(layer, tensor_data, tensor_sizes)
    elif ltype == "resadd":
      pseudo_resadd(layer, tensor_data, tensor_sizes)
    else:
      ids = to_int_list(layer.get("tensorIds", []))
      if len(ids) >= 2:
        src = tensor_data.get(ids[-2], bytearray())
        out_id = ids[-1]
        out_local_size = get_local_tensor_size(layer, len(ids) - 1, tensor_sizes)
        tensor_data[out_id] = normalize_bytes(src, int(tensor_sizes.get(out_id, out_local_size)))

  model_blob = bytearray(blob_span)
  for layer in layers:
    ids = to_int_list(layer.get("tensorIds", []))
    addr = to_int_list(layer.get("address", []))
    addr2 = to_int_list(layer.get("address2", []))
    sizes = to_int_list(layer.get("tensorSize", []))
    for i, tid in enumerate(ids):
      data = tensor_data.get(tid, bytearray())
      local_size = sizes[i] if i < len(sizes) and sizes[i] > 0 else len(data)
      normalized = normalize_bytes(data, local_size)
      if not normalized:
        continue
      if i < len(addr):
        write_blob_at(model_blob, addr_base, addr_end, int(addr[i]), normalized)
      if i < len(addr2):
        write_blob_at(model_blob, addr_base, addr_end, int(addr2[i]), normalized)

  source_ids, sink_ids = infer_source_sink_ids(layers)
  if not source_ids:
    for layer in layers:
      for tid in to_int_list(layer.get("input_ids", [])):
        source_ids.append(tid)
      if source_ids:
        break
  if not sink_ids and layers:
    sink_ids = to_int_list(layers[-1].get("output_ids", []))

  dummy_weight_dir = model_dir / "dummy_weight"
  dummy_input_dir = model_dir / "dummy_input"
  golden_dir = dummy_input_dir / "golden"
  dummy_weight_dir.mkdir(parents=True, exist_ok=True)
  dummy_input_dir.mkdir(parents=True, exist_ok=True)
  golden_dir.mkdir(parents=True, exist_ok=True)

  (dummy_weight_dir / "model.bin").write_bytes(model_blob)

  first_input = None
  for tid in source_ids:
    data = bytes(normalize_bytes(tensor_data.get(tid, bytearray()), int(tensor_sizes.get(tid, 0))))
    path = dummy_input_dir / f"input_{tid}.bin"
    path.write_bytes(data)
    if first_input is None:
      first_input = data
  if first_input is None:
    first_input = b""
  (dummy_input_dir / "input.bin").write_bytes(first_input)

  first_golden = None
  for tid in sink_ids:
    data = bytes(normalize_bytes(tensor_data.get(tid, bytearray()), int(tensor_sizes.get(tid, 0))))
    path = golden_dir / f"golden_{tid}.bin"
    path.write_bytes(data)
    if first_golden is None:
      first_golden = data
  if first_golden is None:
    first_golden = b""
  (golden_dir / "golden.bin").write_bytes(first_golden)

  core_hint = parse_pipeline_core_hint(pipeline_yaml) if pipeline_yaml else 0
  manifest = {
    "model": args.model,
    "seed": args.seed,
    "layers_yaml": str(layers_yaml),
    "pipeline_yaml": str(pipeline_yaml) if pipeline_yaml else "",
    "model_offset": 0,
    "addr_base": addr_base,
    "addr_end": addr_end,
    "model_blob_size": len(model_blob),
    "source_tensor_ids": source_ids,
    "sink_tensor_ids": sink_ids,
    "pipeline_core_hint": core_hint,
    "tensor_size_alignment_policy": {
      "crop": "prefix",
      "pad": "zero_tail",
    },
    "paths": {
      "model_bin": str(dummy_weight_dir / "model.bin"),
      "input_bin": str(dummy_input_dir / "input.bin"),
      "golden_bin": str(golden_dir / "golden.bin"),
    },
  }
  (dummy_weight_dir / "manifest.json").write_text(
    json.dumps(manifest, indent=2, ensure_ascii=True) + "\n",
    encoding="utf-8",
  )

  print(f"generated model_bin: {dummy_weight_dir / 'model.bin'} ({len(model_blob)} bytes)")
  print(f"generated input_bin: {dummy_input_dir / 'input.bin'}")
  print(f"generated golden_bin: {golden_dir / 'golden.bin'}")
  print(f"pipeline core hint: {core_hint}")
  print("tensor size alignment: prefix crop + zero tail pad")
  print("model_offset fixed to 0")


if __name__ == "__main__":
  main()
