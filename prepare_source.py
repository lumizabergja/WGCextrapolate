#!/usr/bin/env python3
"""Reconstruct exact capture-only C++ source from the committed, checked payload."""
from pathlib import Path
import hashlib
import base64
import zlib

root = Path(__file__).resolve().parent
payload = root / "src" / "capture_source.zlib.b64"
target = root / "src" / "main.cpp"

def checksum(data):
    return hashlib.sha256(data).hexdigest()

try:
    data = zlib.decompress(base64.b64decode(payload.read_text(encoding="ascii").strip(), validate=True))
except Exception as exc:
    raise SystemExit(f"Invalid capture-only source payload: {exc}")

base = "a0da13a7ef576ce945f12a9710790ceb3466b1b235ab9e0d58cb27281c4a561a"
if checksum(data) != base:
    raise SystemExit("STOP: unexpected original capture source SHA256 " + checksum(data))
# Repair the compile-time HLSL length: sizeof(pointer) is not the source length.
old = b'const char* const kShader = R"HLSL('
new = b'const char kShader[] = R"HLSL('
if data.count(old) != 1:
    raise SystemExit("STOP: expected HLSL declaration not found exactly once")
data = data.replace(old, new)
expected = "2685a236945b88e75e46de5dc20063929d9456fb99367b6af940c8f54eaeb9f1"
if checksum(data) != expected:
    raise SystemExit("STOP: final capture-only C++ source SHA256 mismatch " + checksum(data))
target.write_bytes(data)
print("Verified standalone Looking Glass DXGI capture-only source:", expected)
# Patch the SHA-verified v1 source to fullscreen, click-through v2.
import json
ops = json.loads(zlib.decompress(base64.b64decode(
    (root / "src" / "fullscreen_delta.b64").read_text(encoding="ascii").strip())))
lines = data.decode("utf-8").splitlines(keepends=True)
for start, stop, replacement in sorted(ops, key=lambda item: item[0], reverse=True):
    lines[start:stop] = [replacement] if replacement else []
new_data = "".join(lines).encode("utf-8")
v2_sha = "34eaa13afd0ebb7020a150459854194886bd5a0a0f78f34d66a619796637df66"
if checksum(new_data) != v2_sha:
    raise SystemExit("STOP: fullscreen click-through patched C++ source hash mismatch "
                     + checksum(new_data))
target.write_bytes(new_data)
print("Verified fullscreen click-through DXGI capture source:", v2_sha)
# DXGI requires the HWND flip swapchain before topmost fullscreen placement.
ops2 = json.loads(zlib.decompress(base64.b64decode(
    (root / "src" / "swapchain_startup_fix.b64").read_text(encoding="ascii").strip())))
lines = new_data.decode("utf-8").splitlines(keepends=True)
for start, stop, replacement in sorted(ops2, key=lambda item: item[0], reverse=True):
    lines[start:stop] = [replacement] if replacement else []
final_data = "".join(lines).encode("utf-8")
final_sha = "ed3ef771adfbc4f39b2d2aba97254cef4cca62ecef4d699366491f4c61fcdb2f"
if checksum(final_data) != final_sha:
    raise SystemExit("STOP: DXGI swapchain startup fix hash mismatch "+checksum(final_data))
target.write_bytes(final_data)
print("Verified fullscreen click-through v2 startup source:", final_sha)
