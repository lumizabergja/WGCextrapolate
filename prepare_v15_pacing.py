#!/usr/bin/env python3
"""Reconstruct exact patched V15 DXGI source provided by the user."""
from pathlib import Path
import io
import base64
import hashlib
import lzma
import tarfile

expected = "c99b03ff51395fc2bfc176357539aa43128d68d569298f0282da37f1ddd6bb82"
bundle = base64.b64decode(Path("v15-pacing.tar.xz.b64").read_text(encoding="ascii").strip())
digest = hashlib.sha256(bundle).hexdigest()
if digest != expected:
    raise SystemExit(f"Source bundle SHA-256 mismatch: {digest}")
archive = lzma.decompress(bundle)
with tarfile.open(fileobj=io.BytesIO(archive), mode="r:") as tf:
    names = set(tf.getnames())
    required = {"Build.cmd", "README.md", "src/main.cpp", "src/FG.ico", "src/FG.rc",
                "vendor/headers/nvOpticalFlowCommon.h", "vendor/headers/nvOpticalFlowD3D11.h"}
    if names != required:
        raise SystemExit(f"Unexpected archive contents: {sorted(names ^ required)}")
    tf.extractall(".")
source_hash = hashlib.sha256(Path("src/main.cpp").read_bytes()).hexdigest()
if source_hash != "57f92e1c44b8f426ffa850f3bedfd11974ee6690edd46745ce9c6e85e390efc2":
    raise SystemExit(f"Patched V15 source hash mismatch: {source_hash}")
print("Verified original V15 DXGI + V35-style pacing source", source_hash)
