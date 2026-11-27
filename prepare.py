from pathlib import Path
import base64, hashlib, zipfile, shutil
root=Path(__file__).resolve().parent
data=base64.b64decode((root/'cuda-vulkan-source.b64').read_text(),validate=False)
assert hashlib.sha256(data).hexdigest()=='0fcb8e92e8840be250074d058aa86f1927126c463834d66c6e14a98576e86d37'
(root/'source.zip').write_bytes(data)
fg=root/'fg'
with zipfile.ZipFile(root/'source.zip') as z:
    assert z.testzip() is None
    z.extractall(fg)
def edit(path, changes):
    p=fg/path
    s=p.read_text(encoding='utf-8')
    for a,b in changes:
        if s.count(a)!=1: raise RuntimeError(f'Expected one match: {path}: {a[:80]}')
        s=s.replace(a,b,1)
    p.write_text(s,encoding='utf-8',newline='\n')
edit('src/main.cpp',[
 ('#include "pixel_types.h"','#include "pixel_types.h"\n#include "frame_kind.h"'),
 ('        if(tracked.Exited())return SessionOutcome::Stop;\n        // CUDA/Vulkan','        if(tracked.Exited())return SessionOutcome::Stop;\n        wchar_t requireWarp[2]{};\n        if(GetEnvironmentVariableW(L"FG_REQUIRE_NATIVE_FRAME_WARP",requireWarp,2)>0)\n            throw Error("Native Reflex 2 Frame Warp unavailable: backend required");\n        // CUDA/Vulkan'),
 ('                    gpu.Draw(dx,current,previous>=0?previous:current,prediction,','                    const auto kind=ClassifyFrame(realFramePending,prediction);\n                    gpu.Draw(dx,current,previous>=0?previous:current,prediction,kind,')
])
edit('src/cuda_pipeline.h',[
 ('float horizon,float radius,float feather) {','float horizon,FrameKind kind,float radius,float feather) {'),
 ('dx.vk.Present(signal.params.fence.value);','dx.vk.Present(signal.params.fence.value,kind);')
])
edit('src/vulkan_presenter.h',[
 ('    PFN_vkSetLatencyMarkerNV marker{};','    PFN_vkSetLatencyMarkerNV marker{};\n    using NativeLatewarpRecorder = void (*)(VkCommandBuffer, VkImage, VkExtent2D, VkFormat, uint64_t);\n    NativeLatewarpRecorder recordNativeLatewarp=nullptr;'),
 ('void Present(uint64_t inValue){','void Present(uint64_t inValue,FrameKind kind){'),
 ('vkCmdEndRenderPass(cmd);Barrier(false);','vkCmdEndRenderPass(cmd);\n        if(ShouldRecordNativeLatewarp(kind,recordNativeLatewarp!=nullptr))\n            recordNativeLatewarp(cmd,images[ix],extent,format,id);\n        Barrier(false);')
])
for p in ('src/frame_kind.h','tests/frame_kind.cpp','native_ngx/latewarp_vk.h','native_ngx/compile_probe.cpp','math-reference/camera_depth.py','math-reference/test_camera_depth.py','ci-build.cmd','README.md'):
    dst=fg/p
    dst.parent.mkdir(parents=True,exist_ok=True)
    shutil.copyfile(root/p,dst)
print('Source archive verified and real-frame provenance patch applied.')
