#!/usr/bin/env python3
"""Rebuild verified low-latency WGC source and real/FG pacing fix."""
from pathlib import Path
import hashlib
import json
import base64
import zlib

root = Path(__file__).resolve().parent
src = root / "src" / "main.cpp"
baseline_sha = "d3cf8bef18450fece42fa9e4500030ee8f3f4a744a15a6fa6653598953fcc900"
optimized_sha = "70828e2e2e0ad03630513d76c9dc3a41cd45d5d29b413b2cabc34cbb8dc5b67a"
fixed_sha = "2b78156a17e8789dba158d8c9bc265ba330343dfe388e097d36952b9ee3e44a0"

def digest(data):
    return hashlib.sha256(data).hexdigest()

def apply_delta(data, delta_path):
    operations = json.loads(zlib.decompress(base64.b64decode(delta_path.read_text(encoding="ascii"))))
    lines = data.decode("utf-8").splitlines(keepends=True)
    for begin, end, replacement in sorted(operations, key=lambda op: op[0], reverse=True):
        lines[begin:end] = [replacement] if replacement else []
    return "".join(lines).encode("utf-8")

original = src.read_bytes()
actual = digest(original)
if actual == fixed_sha:
    print("Already patched and verified:", actual)
    raise SystemExit(0)
if actual == baseline_sha:
    intermediate = apply_delta(original, root / "src" / "ops.b64")
    if digest(intermediate) != optimized_sha:
        raise SystemExit("STOP: original optimization delta failed integrity check")
elif actual == optimized_sha:
    intermediate = original
else:
    raise SystemExit("STOP: unknown source SHA256 " + actual)

fixed = apply_delta(intermediate, root / "src" / "pacing_fix.b64")
if digest(fixed) != fixed_sha:
    raise SystemExit("STOP: frame generation pacing fix SHA256 mismatch " + digest(fixed))
src.write_bytes(fixed)
print("Verified WGC D3D11 generated-frame fix:", fixed_sha)
