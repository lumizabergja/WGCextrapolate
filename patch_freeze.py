#!/usr/bin/env python3
"""Apply the exact, SHA256-verified DXGI Desktop Duplication freeze fix."""
from pathlib import Path
import hashlib,base64,zlib,json
root=Path(__file__).resolve().parent
src=root/"src"/"main.cpp"
v1="edf9012b454e2638660806495065a41309cd7a925eb4cd1f5ced465b24220f02"
v2="a33b0dcc3dfa42461433bdadf4528b59387c44f434396f8228d45a954404c3c2"
raw=src.read_bytes()
if hashlib.sha256(raw).hexdigest()!=v1:
    raise SystemExit("STOP: DXGI freeze patch base SHA mismatch")
ops=json.loads(zlib.decompress(base64.b64decode((root/"src"/"freeze_fix_v2.b64").read_text(encoding="ascii"))))
lines=raw.decode("utf-8").splitlines(keepends=True)
for i,j,replacement in sorted(ops,key=lambda op:op[0],reverse=True):
    lines[i:j]=[replacement] if replacement else []
out="".join(lines).encode("utf-8")
h=hashlib.sha256(out).hexdigest()
if h!=v2:
    raise SystemExit("STOP: freeze fix patched-source hash mismatch "+h)
src.write_bytes(out)
print("Verified V15 Looking Glass DXGI freeze fix v2:",h)
