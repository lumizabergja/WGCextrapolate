from pathlib import Path
from hashlib import sha256

path = Path('src/main.cpp')
expected_before = 'fb445337af61c569883a67b83fa27587387745425c02775bd1165476edbeecee'
raw = path.read_bytes()
assert sha256(raw).hexdigest() == expected_before, 'Refusing to patch unexpected NvFBC source'
s = raw.decode('utf-8')

edits = [
('''    using SetFlags = void (NVFBCAPI*)(DWORD);
''', '''    using SetFlags = void (NVFBCAPI*)(DWORD);
    using Enable = NVFBCRESULT (NVFBCAPI*)(NVFBC_STATE);
'''),
('''        auto createFn=(Create)GetProcAddress(library,"NvFBC_CreateEx");
        if(!statusFn||!createFn)throw Error("NvFBC 7.x ABI exports missing from driver/wrapper");
''', '''        auto createFn=(Create)GetProcAddress(library,"NvFBC_CreateEx");
        auto enableFn=(Enable)GetProcAddress(library,"NvFBC_Enable");
        if(!statusFn||!createFn)throw Error("NvFBC 7.x ABI exports missing from driver/wrapper");
'''),
('''        if(statusCode!=NVFBC_SUCCESS)throw Error("NvFBC_GetStatusEx failed",statusCode);
        // Some community wrappers inject authorization only at CreateEx,
''', '''        if(statusCode!=NVFBC_SUCCESS)throw Error("NvFBC_GetStatusEx failed",statusCode);
        // NVIDIA's documented Windows 7.x enable step, used by nvfbc-relay.
        // This is NOT a Linux SDK 9.0.0 port or an NVIDIA driver bypass.
        // It can require an elevated process and is expected to fail on
        // Windows 10 22H2 drivers that have permanently disabled NvFBC.
        bool triedEnable=false;
        NVFBCRESULT enableResult=static_cast<NVFBCRESULT>(-100); // export unavailable
        if(!status.bIsCapturePossible && !status.bCurrentlyCapturing && enableFn){
            triedEnable=true;
            enableResult=enableFn(NVFBC_STATE_ENABLE);
            if(enableResult==NVFBC_SUCCESS){
                // Enable may reset the graphics device; do not reuse the
                // D3D9Ex device and GPU allocations created before enabling.
                throw Error("NvFBC_Enable accepted; close FG and launch again",0);
            }
        }
        // Some community wrappers inject authorization only at CreateEx,
'''),
('''                    "NvFBC CreateEx rejected: standard=%ld legacy=%ld; possible=%u canCreate=%u version=%lu. "
                    "GeForce 5060Ti/Win10 22H2 may be unsupported",
                    static_cast<long>(r),static_cast<long>(retry),
                    (unsigned)status.bIsCapturePossible,(unsigned)status.bCanCreateNow,
                    (unsigned long)status.dwNvFBCVersion);
''', '''                    "NvFBC driver refused: standard=%ld legacy=%ld enable=%ld tried=%u; possible=%u canCreate=%u ver=%lu. "
                    "-15 requires Administrator; -5 often means driver unsupported on Win10 22H2",
                    static_cast<long>(r),static_cast<long>(retry),static_cast<long>(enableResult),
                    unsigned(triedEnable),
                    (unsigned)status.bIsCapturePossible,(unsigned)status.bCanCreateNow,
                    (unsigned long)status.dwNvFBCVersion);
''')
]
for old, new in edits:
    if s.count(old)!=1:
        raise SystemExit(f'Patch target occurs {s.count(old)} times: {old[:85]}')
    s=s.replace(old,new)
result=s.encode('utf-8')
expected_after = '289ae2d4441f4ddf010becd5aebf26b0364a7aa1474094d879a4f95bfcb5a739'
if sha256(result).hexdigest()!=expected_after:
    raise SystemExit('Unexpected patched source hash: '+sha256(result).hexdigest())
path.write_bytes(result)
print('Verified NvFBC-enable probe source SHA-256:',sha256(result).hexdigest())
