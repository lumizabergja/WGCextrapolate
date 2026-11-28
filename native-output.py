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
  'VkDevices dx; dx.Init(overlay.hwnd,gameW,gameH,cuda);'),
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

edit('src/vulkan_presenter.h',[
 ('    bool windowedExclusiveControl=false;',
  '''    bool windowedExclusiveControl=false;
    bool surfaceScalingAvailable=false,swapScalingAvailable=false;
    VkExtent2D requestedNativeExtent{};'''),
 ('    void Init(HWND hwnd,LUID luid,uint32_t w,uint32_t h){',
  '''    void Init(HWND hwnd,LUID luid,uint32_t w,uint32_t h){
        requestedNativeExtent={w,h};'''),
 ('        VkInstanceCreateInfo ic{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};',
  '''        surfaceScalingAvailable=Has(iex,VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME)&&Has(iex,VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
        if(surfaceScalingAvailable&&std::none_of(ie.begin(),ie.end(),[](const char* s){return !strcmp(s,VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);}))ie.push_back(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
        VkInstanceCreateInfo ic{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};'''),
 ('        float priority=1;VkDeviceQueueCreateInfo qi',
  '''        VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT scalingFeature{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT};
        if(surfaceScalingAvailable&&Has(ex,VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME)){
            VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            query.pNext=&scalingFeature;vkGetPhysicalDeviceFeatures2(physical,&query);
            if(scalingFeature.swapchainMaintenance1){
                swapScalingAvailable=true;
                pid.pNext=&scalingFeature;
                if(std::none_of(de.begin(),de.end(),[](const char* s){return !strcmp(s,VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);}))de.push_back(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
            }
        }
        float priority=1;VkDeviceQueueCreateInfo qi'''),
 ('extent=caps.currentExtent.width==UINT32_MAX?VkExtent2D{std::clamp(w,caps.minImageExtent.width,caps.maxImageExtent.width),std::clamp(h,caps.minImageExtent.height,caps.maxImageExtent.height)}:caps.currentExtent;if(!extent.width||!extent.height)throw Error("Vulkan surface minimized");',
  '''const auto surfaceExtent=caps.currentExtent.width==UINT32_MAX?VkExtent2D{w,h}:caps.currentExtent;
        if(!surfaceExtent.width||!surfaceExtent.height)throw Error("Vulkan surface minimized");
        extent=requestedNativeExtent;
        const bool needsScaling=extent.width!=surfaceExtent.width||extent.height!=surfaceExtent.height;
        if(!extent.width||!extent.height)throw Error("Invalid native Vulkan output extent");'''),
 ('        uint32_t count=std::max(2u,caps.minImageCount);',
  '''        VkSwapchainPresentScalingCreateInfoEXT scaling{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_SCALING_CREATE_INFO_EXT};
        if(needsScaling){
            if(!swapScalingAvailable)throw Error("Native-size Vulkan swapchain with fullscreen stretch is unsupported; use matching fullscreen display mode");
            auto getCaps=reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR>(vkGetInstanceProcAddr(instance,"vkGetPhysicalDeviceSurfaceCapabilities2KHR"));
            if(!getCaps)throw Error("Vulkan present scaling capability query missing");
            VkSurfacePresentModeEXT queryMode{VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_EXT};queryMode.presentMode=mode;
            VkPhysicalDeviceSurfaceInfo2KHR query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR};query.surface=surface;query.pNext=&queryMode;
            VkSurfaceFullScreenExclusiveInfoEXT queryExclusive{VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT};queryExclusive.fullScreenExclusive=VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT;
            if(windowedExclusiveControl)queryMode.pNext=&queryExclusive;
            VkSurfacePresentScalingCapabilitiesEXT scaleCaps{VK_STRUCTURE_TYPE_SURFACE_PRESENT_SCALING_CAPABILITIES_EXT};
            VkSurfaceCapabilities2KHR result{VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR};result.pNext=&scaleCaps;
            V(getCaps(physical,&query,&result),"Query native-size present scaling");
            if(!(scaleCaps.supportedPresentScaling&VK_PRESENT_SCALING_STRETCH_BIT_EXT)||
                extent.width<scaleCaps.minScaledImageExtent.width||extent.height<scaleCaps.minScaledImageExtent.height||
                extent.width>scaleCaps.maxScaledImageExtent.width||extent.height>scaleCaps.maxScaledImageExtent.height)
                throw Error("Driver does not support fullscreen stretch of this native Vulkan extent; use matching fullscreen display mode");
            scaling.scalingBehavior=VK_PRESENT_SCALING_STRETCH_BIT_EXT;
            caps=result.surfaceCapabilities;
        }else if(extent.width<caps.minImageExtent.width||extent.height<caps.minImageExtent.height||
                 extent.width>caps.maxImageExtent.width||extent.height>caps.maxImageExtent.height)
            throw Error("Native Vulkan extent outside surface limits");
        uint32_t count=std::max(2u,caps.minImageCount);'''),
 ('        if(windowedExclusiveControl)ll.pNext=&exclusive;',
  '''        if(windowedExclusiveControl)ll.pNext=&exclusive;
        if(needsScaling){scaling.pNext=ll.pNext;ll.pNext=&scaling;}'''),
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
    f.write('\n## Strict native output and NvOFA\nCapture, CUDA output, NGX and Vulkan swapchain must match the game client dimensions. No capture normalization or final shader resampling. Vulkan uses a one-to-one swapchain when the logical display mode matches; on a larger fullscreen surface it requests driver stretch through VK_EXT_swapchain_maintenance1 only when supported for the chosen present mode and dimensions. Otherwise FG stops with an explicit error rather than silently upscaling or changing Windows display settings. Use actual fullscreen game resolution, not a borderless internal render scale. Native (100%) NvOFA sets input width and height directly from the captured frame, bypasses percentage/capability arithmetic and uses the original one-to-one BGRA-to-luma stage. Reduced percentages keep the existing scaled branch. Required hardware optical-flow format conversion and native NGX same-size final format blit remain. Adaptive Reflex 2, UI and pacing are unchanged. Windows compilation and target driver support for present scaling must be verified.\n')
print('Strict game-native Vulkan output and direct Native NvOFA path applied.')
