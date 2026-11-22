#!/usr/bin/env python3
"""Replace the earlier approximate V35 scheduler with V35's original cadence rules.

Based on uploaded V35 main.cpp: source interval 0.75/0.25 filtering,
post-real prediction anchor, live capture preemption, 10-ms bounded slot wait,
and predictionMs / realIntervalMs extrapolation fraction.
"""
import base64
import gzip
import hashlib
from pathlib import Path
import subprocess

source=Path("src/main.cpp")
old_hash="57f92e1c44b8f426ffa850f3bedfd11974ee6690edd46745ce9c6e85e390efc2"
new_hash="89c6ca3c6cb92c0b4360107ac3f22f51d5e69a92dd23b905ec429577cb01bc9c"
patch_hash="9be6e3093f978252dab4717a17276ec2d9e47d1fec250ffbbcc87e370a1e88ef"
current_hash=hashlib.sha256(source.read_bytes()).hexdigest()
if current_hash != old_hash:
    raise SystemExit("Unexpected V15 baseline before exact V35 scheduler: "+current_hash)
patch=gzip.decompress(base64.b64decode(Path("v35-exact-pacing.patch.gz.b64").read_text(encoding="ascii").strip()))
if hashlib.sha256(patch).hexdigest() != patch_hash:
    raise SystemExit("Patch checksum mismatch")
subprocess.run(["git","apply","--check","-"],input=patch,check=True)
subprocess.run(["git","apply","-"],input=patch,check=True)
current_hash=hashlib.sha256(source.read_bytes()).hexdigest()
if current_hash != new_hash:
    raise SystemExit("Exact V35 scheduler source checksum mismatch: "+current_hash)
print("Verified V15 DXGI using V35 original pacing rules:",current_hash)
