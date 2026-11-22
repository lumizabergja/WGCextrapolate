#!/usr/bin/env python3
"""Apply the safe single-thread capture-aware V35 cadence patch to V15 DXGI."""
import base64
import gzip
import hashlib
from pathlib import Path
import subprocess

path=Path("src/main.cpp")
original="89c6ca3c6cb92c0b4360107ac3f22f51d5e69a92dd23b905ec429577cb01bc9c"
updated="adbd5a9522c7a15beed66cb5607fe121d7c1cd91526c867f01f5ee43656791dc"
expected_patch="c347e4f58f9f9b2bdf6fa4e4808b72ff894d1e42b8f9353a822d9b37107bc1d9"

oldhash=hashlib.sha256(path.read_bytes()).hexdigest()
if oldhash!=original:
    raise SystemExit(f"Unexpected original V15 DXGI exact V35 source {oldhash}")
patch=gzip.decompress(base64.b64decode(Path("v35-safe-pacing.patch.gz.b64").read_text(encoding="ascii").strip()))
digest=hashlib.sha256(patch).hexdigest()
if digest!=expected_patch:
    raise SystemExit(f"Source patch checksum mismatch {digest}")
subprocess.run(["git","apply","--check","-"],input=patch,check=True)
subprocess.run(["git","apply","-"],input=patch,check=True)
newhash=hashlib.sha256(path.read_bytes()).hexdigest()
if newhash!=updated:
    raise SystemExit(f"Non-blocking V35 pacing source checksum mismatch {newhash}")
print("Verified non-blocking V35-style scheduler on original V15 DXGI:",newhash)
