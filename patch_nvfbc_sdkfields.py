"""Set mandatory NVIDIA SDK CreateEx fields on verified V15 CreateFix source."""
from pathlib import Path
import hashlib

source=Path("src/main.cpp")
data=source.read_bytes()
if hashlib.sha256(data).hexdigest()!="10372f2538195d9e5a2f4c65a833981160baf12d66754a591e53cb590cfe8373":
    raise SystemExit("Unexpected baseline source; no changes made")
old="""            candidate.dwInterfaceType=NVFBC_TO_DX9_VID;
            candidate.pDevice=device;
"""
new="""            candidate.dwInterfaceType=NVFBC_TO_DX9_VID;
            // Required by NVIDIA Capture SDK 7.1 CreateEx sample.
            // 0 leaves an unspecified interface version on modern drivers.
            candidate.dwInterfaceVersion=NVFBC_DLL_VERSION;
            candidate.dwMaxDisplayWidth=static_cast<NvU32>(-1);
            candidate.dwMaxDisplayHeight=static_cast<NvU32>(-1);
            candidate.pDevice=device;
"""
s=data.decode("utf-8")
if s.count(old)!=1:
    raise SystemExit("Expected NvFBC CreateEx block not found")
updated=s.replace(old,new).encode("utf-8")
sha=hashlib.sha256(updated).hexdigest()
if sha!="4e00efe975d20bfb49034b0a7b44b71b5a475d8b43b84b74824759d6ae0556b0":
    raise SystemExit("Expected SDK-patched SHA mismatch: "+sha)
source.write_bytes(updated)
print("Verified V15 Clean CreateEx SDK field source:",sha)
