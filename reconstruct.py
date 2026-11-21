#!/usr/bin/env python3
"""Rebuild the SHA256-verified optimized WGC source from its baseline and delta."""
from pathlib import Path
import hashlib
import json
import base64
import zlib

root = Path(__file__).resolve().parent
src = root / "src" / "main.cpp"
ops_path = root / "src" / "ops.b64"
base = src.read_bytes()
expected_base = "d3cf8bef18450fece42fa9e4500030ee8f3f4a744a15a6fa6653598953fcc900"
expected_output = "70828e2e2e0ad03630513d76c9dc3a41cd45d5d29b413b2cabc34cbb8dc5b67a"
if hashlib.sha256(base).hexdigest() != expected_base:
    raise SystemExit("STOP: unexpected base source SHA256; refusing to modify source")
ops = json.loads(zlib.decompress(base64.b64decode(ops_path.read_text(encoding="ascii"))))
lines = base.decode("utf-8").splitlines(keepends=True)
for i, j, replacement in sorted(ops, key=lambda op: op[0], reverse=True):
    lines[i:j] = [replacement] if replacement else []
optimized = "".join(lines).encode("utf-8")
actual = hashlib.sha256(optimized).hexdigest()
if actual != expected_output:
    raise SystemExit("STOP: reconstructed source hash mismatch: " + actual)
src.write_bytes(optimized)
print("Optimized WGC D3D11 source verified: " + actual)
