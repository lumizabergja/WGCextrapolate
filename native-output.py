# Apply after original reconstruction, live warp and adaptive late input.
from pathlib import Path
root=Path(__file__).resolve().parent
fg=root/'fg'
def edit(path,pairs):
    p=fg/path
    s=p.read_text(encoding='utf-8')
    for old,new in pairs:
        if s.count(old)!=1: raise RuntimeError(f'Native output patch {path}: expected one {old[:100]!r}, got {s.count(old)}')
        s=s.replace(old,new,1)
    p.write_text(s,encoding='utf-8',newline='\n')

edit('src/main.cpp',[
 ('VkDevices dx; dx.Init(overlay.hwnd,overlay.width,overlay.height,cuda);',
  '''if(overlay.width!=gameW||overlay.height!=gameH)
            throw Error("Native output requires matching fullscreen display mode: monitor "+
                std::to_string(overlay.width)+"x"+std::to_string(overlay.height)+", game "+
                std::to_string(gameW)+"x"+std::to_string(gameH));
        VkDevices dx; dx.Init(overlay.hwnd,gameW,gameH,cuda);'''),
 ('                    if(usable)gpu.NormalizeCapture(available,gameW,gameH);',
  '                    // Never resample capture to manufacture native game pixels.'),
 ('                const auto& incoming=capture.frames[next];',
  '''                const auto& incoming=capture.frames[next];
                if(incoming.width!=gameW||incoming.height!=gameH)
                    throw Error("NvFBC capture is "+std::to_string(incoming.width)+"x"+std::to_string(incoming.height)+
                        "; game client is "+std::to_string(gameW)+"x"+std::to_string(gameH)+
                        ". True native source unavailable; use fullscreen at the game resolution.");
                if(dx.vk.extent.width!=gameW||dx.vk.extent.height!=gameH)
                    throw Error("Vulkan output resolution differs from game client");'''),
 ('                // CS2 may change the logical scanout resolution on focus gain.',
  '''                RECT liveClient{};
                if(GetClientRect(game.window,&liveClient)&&
                   (UINT(liveClient.right-liveClient.left)!=gameW||UINT(liveClient.bottom-liveClient.top)!=gameH))
                    return SessionOutcome::Recreate;
                // CS2 may change the logical scanout resolution on focus gain.'''),
 ('Status(L"Native source: "+std::to_wstring(incoming.width)+L"x"+std::to_wstring(incoming.height));',
  'Status(L"Native source: "+std::to_wstring(incoming.width)+L"x"+std::to_wstring(incoming.height)+L" | Vulkan output: "+std::to_wstring(dx.vk.extent.width)+L"x"+std::to_wstring(dx.vk.extent.height));'),
])

# Use the previously working standard Vulkan WSI path. No surface/swapchain
# maintenance extensions, feature-chain changes, or driver present-scaling structs.
edit('src/vulkan_presenter.h',[
 ('    bool windowedExclusiveControl=false;',
  '''    bool windowedExclusiveControl=false;
    VkExtent2D requestedNativeExtent{};'''),
 ('    void Init(HWND hwnd,LUID luid,uint32_t w,uint32_t h){',
  '''    void Init(HWND hwnd,LUID luid,uint32_t w,uint32_t h){
        requestedNativeExtent={w,h};'''),
 ('extent=caps.currentExtent.width==UINT32_MAX?VkExtent2D{std::clamp(w,caps.minImageExtent.width,caps.maxImageExtent.width),std::clamp(h,caps.minImageExtent.height,caps.maxImageExtent.height)}:caps.currentExtent;if(!extent.width||!extent.height)throw Error("Vulkan surface minimized");',
  '''extent=caps.currentExtent.width==UINT32_MAX?VkExtent2D{std::clamp(w,caps.minImageExtent.width,caps.maxImageExtent.width),std::clamp(h,caps.minImageExtent.height,caps.maxImageExtent.height)}:caps.currentExtent;
        if(!extent.width||!extent.height)throw Error("Vulkan surface minimized");
        if(extent.width!=requestedNativeExtent.width||extent.height!=requestedNativeExtent.height)
            throw Error("Native output requires matching Windows/game fullscreen display mode: surface "+
                std::to_string(extent.width)+"x"+std::to_string(extent.height)+", game "+
                std::to_string(requestedNativeExtent.width)+"x"+std::to_string(requestedNativeExtent.height));'''),
 ('        sourceExtent={width,height};',
  '''        if(width!=extent.width||height!=extent.height)throw Error("Source and native output extent mismatch");
        sourceExtent={width,height};'''),
])

# Native mode bypasses all resolution percentage computation and staging resampling.
edit('src/cuda_pipeline.h',[
 ('    UINT flowWidth{},flowHeight{};',
  '    UINT flowWidth{},flowHeight{};\n    bool nativeFlowResolution=true;'),
 ('        if(!api.nvOFGetCaps)throw Error("NvOFA capability API missing");\n        const UINT percent=g_flowPercent.load(std::memory_order_acquire);',
  '''        const UINT percent=g_flowPercent.load(std::memory_order_acquire);
        nativeFlowResolution=(percent==100);
        if(nativeFlowResolution){
            // Pass exact captured width/height to NvOFA; its Init validates support.
            flowWidth=width;flowHeight=height;
        }else{
        if(!api.nvOFGetCaps)throw Error("NvOFA capability API missing");'''),
 ('        width=flowWidth;height=flowHeight;\n        NV_OF_INIT_PARAMS init{};',
  '        width=flowWidth;height=flowHeight;\n        }\n        NV_OF_INIT_PARAMS init{};'),
 ('        if(width==flowWidth&&height==flowHeight){',
  '''        if(nativeFlowResolution&&(width!=flowWidth||height!=flowHeight))
            throw Error("Native NvOFA source changed dimensions");
        if(nativeFlowResolution||(width==flowWidth&&height==flowHeight)){'''),
])

# Only one-to-one color reads: the driver, not FG, stretches for display if required.
(fg/'src'/'fg.frag').write_text(r'''#version 450
layout(set=0,binding=0,std430) readonly buffer Pixels { uint pixels[]; };
layout(push_constant) uniform P { uint sourceWidth; uint sourceHeight; uint outputWidth; uint outputHeight; } p;
layout(location=0) out vec4 color;
void main(){
    uvec2 xy=uvec2(gl_FragCoord.xy);
    uint v=pixels[xy.y*p.sourceWidth+xy.x];
    color=vec4(float((v>>16)&255u),float((v>>8)&255u),float(v&255u),255.0)/255.0;
}
''',encoding='utf-8',newline='\n')
edit('src/native_live.h',[
 ('void Record(VkCommandBuffer cmd,VkImage swapImage,uint32_t displayW,uint32_t displayH){',
  '''void Record(VkCommandBuffer cmd,VkImage swapImage,uint32_t displayW,uint32_t displayH){
        if(displayW!=w||displayH!=h)throw Error("Native Frame Warp output extent mismatch");'''),
])
with (fg/'README.md').open('a',encoding='utf-8') as f:
    f.write('\n## Strict native output and NvOFA\nCapture, CUDA output, NGX and Vulkan swapchain must match the game client dimensions. No capture normalization or final shader resampling. Vulkan uses the original standard WSI swapchain with a one-to-one extent; no experimental present-scaling extension. FG stops with an explicit error if the game, capture or logical display mode differs rather than silently upscaling or changing Windows display settings. Configure NVIDIA GPU full-screen scaling and a matching fullscreen display mode before starting FG. Use actual fullscreen game resolution, not a borderless internal render scale. Native (100%) NvOFA sets input width and height directly from the captured frame, bypasses percentage/capability arithmetic and uses the original one-to-one BGRA-to-luma stage. Reduced percentages keep the existing scaled branch. Required hardware optical-flow format conversion and native NGX same-size final format blit remain. Adaptive Reflex 2, UI and pacing are unchanged. Windows compilation and target GPU behavior must be verified.\n')
print('Strict game-native Vulkan output and direct Native NvOFA path applied.')
