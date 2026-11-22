"""Fix the NvFBC CreateEx INVALID_PARAM (-2) in V15 without capture/presenter changes."""
from pathlib import Path
import hashlib

source=Path("src/main.cpp")
data=source.read_bytes()
expected="4bb76cb492d1d95004636c05fb1161504b45cb796a0c682674da77ab1768ffea"
if hashlib.sha256(data).hexdigest()!=expected:
    raise SystemExit("Unexpected V15 WDA source: refusing to patch")
old='''        NvFBCCreateParams cp{};cp.dwVersion=NVFBC_CREATE_PARAMS_VER;
        cp.dwInterfaceType=NVFBC_TO_DX9_VID;cp.pDevice=device;
        // Match the existing application's game-PID capture selection.
        struct PidPrivate {DWORD version,enable,pid,reserved[47];};
        PidPrivate privatePid{};privatePid.version=sizeof(PidPrivate)|(1u<<16)|(0x70u<<24);
        privatePid.enable=1;privatePid.pid=pid;
        cp.pPrivateData2=&privatePid;cp.dwPrivateData2Size=sizeof(privatePid);
        constexpr DWORD kCompat[4]={0xaef57ac5u,0x401d1a39u,0x1b856bbeu,0x9ed0cebau};
        cp.pPrivateData=(void*)kCompat;cp.dwPrivateDataSize=sizeof(kCompat);
        NVFBCRESULT r=createFn(&cp);
        if(r!=NVFBC_SUCCESS||!cp.pNvFBC)throw Error("NvFBC DX9 CreateEx failed",r);
'''
new='''        // NvFBC is display/head capture; the selected game's PID is used to
        // locate its HWND/monitor and to stop capture on exit, not as an
        // undocumented NvFBC_CreateEx private data extension.  Some drivers
        // reject the old extra private-PID block with INVALID_PARAM (-2).
        (void)pid;
        constexpr DWORD kCompat[4]={0xaef57ac5u,0x401d1a39u,0x1b856bbeu,0x9ed0cebau};
        NvFBCCreateParams cp{};
        NVFBCRESULT r=NVFBC_ERROR_INVALID_PARAM;
        // First try SDK v2 with the existing GeForce key.  If only the input
        // parameters/version are rejected, retry without the key (community
        // wrappers may insert it internally), then SDK v1 with/without key.
        // No capture or renderer changes and no API fallback.
        for(int attempt=0;attempt<4;++attempt) {
            NvFBCCreateParams candidate{};
            candidate.dwVersion=attempt<2?NVFBC_CREATE_PARAMS_VER_2:NVFBC_CREATE_PARAMS_VER_1;
            candidate.dwInterfaceType=NVFBC_TO_DX9_VID;
            candidate.pDevice=device;
            if((attempt&1)==0) {
                candidate.pPrivateData=(void*)kCompat;
                candidate.dwPrivateDataSize=sizeof(kCompat);
            }
            r=createFn(&candidate);
            if(r==NVFBC_SUCCESS && candidate.pNvFBC) {cp=candidate;break;}
            if(r==NVFBC_SUCCESS)r=NVFBC_ERROR_INVALID_PTR;
            if(r!=NVFBC_ERROR_INVALID_PARAM && r!=NVFBC_ERROR_INCOMPATIBLE_VERSION)
                break;
        }
        if(r!=NVFBC_SUCCESS||!cp.pNvFBC)throw Error("NvFBC DX9 CreateEx failed (standard parameters)",r);
'''
text=data.decode("utf-8")
if text.count(old)!=1:
    raise SystemExit("Could not identify original CreateEx block")
updated=text.replace(old,new).encode("utf-8")
sha=hashlib.sha256(updated).hexdigest()
if sha!="10372f2538195d9e5a2f4c65a833981160baf12d66754a591e53cb590cfe8373":
    raise SystemExit("NvFBC CreateEx source SHA mismatch: "+sha)
source.write_bytes(updated)
print("Validated NvFBC CreateEx compatibility source:",sha)
