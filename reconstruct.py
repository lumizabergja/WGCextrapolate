#!/usr/bin/env python3
"""Reconstruct the exact v4 WGC D3D11 source from verified successive deltas."""
from pathlib import Path
import hashlib
import json
import base64
import zlib

root = Path(__file__).resolve().parent
source = root / "src" / "main.cpp"
BASE_SHA = "d3cf8bef18450fece42fa9e4500030ee8f3f4a744a15a6fa6653598953fcc900"
OPT_SHA = "70828e2e2e0ad03630513d76c9dc3a41cd45d5d29b413b2cabc34cbb8dc5b67a"
V2_SHA = "2b78156a17e8789dba158d8c9bc265ba330343dfe388e097d36952b9ee3e44a0"
V3_SHA = "b52023fae6eeed1792e3a0a24b1946b9f00ddcb7cf3213e77539f5f6175ac366"
V4_SHA = "80f95a236ee353c069971061f5fe6a4415fb4b8915dc25ca4fa39f5cdd249d12"

def hash_of(data):
    return hashlib.sha256(data).hexdigest()

def apply(data, filename):
    instructions = json.loads(zlib.decompress(base64.b64decode((root / "src" / filename).read_text(encoding="ascii"))))
    lines = data.decode("utf-8").splitlines(keepends=True)
    for start, end, replacement in sorted(instructions, key=lambda item: item[0], reverse=True):
        lines[start:end] = [replacement] if replacement else []
    return "".join(lines).encode("utf-8")

def checked(data, expected, stage):
    actual = hash_of(data)
    if actual != expected:
        raise SystemExit(f"STOP: {stage}: SHA256 mismatch {actual}, expected {expected}")
    return data

code = source.read_bytes()
current = hash_of(code)
if current == BASE_SHA:
    code = checked(apply(code, "ops.b64"), OPT_SHA, "baseline optimization")
    current = OPT_SHA
if current == OPT_SHA:
    code = checked(apply(code, "pacing_fix.b64"), V2_SHA, "visible-FG scheduler")
    current = V2_SHA
if current == V2_SHA:
    code = checked(apply(code, "motion_fix.b64"), V3_SHA, "motion shader and pacing v3")
    current = V3_SHA
if current == V3_SHA:
    code = checked(apply(code, "latency_v4.b64"), V4_SHA, "low-overhead v4")
    current = V4_SHA
if current != V4_SHA:
    raise SystemExit(f"STOP: unrecognized input C++ source SHA256 {current}")
source.write_bytes(code)
print("WGC low-overhead v4 source verified:", V4_SHA)
