from pathlib import Path
root=Path(__file__).resolve().parent
fg=root/'fg'
def edit(path,pairs):
    p=fg/path
    s=p.read_text(encoding='utf-8')
    for old,new in pairs:
        if s.count(old)!=1: raise RuntimeError(f'Zero-copy patch {path}: expected one {old[:120]!r}, got {s.count(old)}')
        s=s.replace(old,new,1)
    p.write_text(s,encoding='utf-8',newline='\n')

# Capture slots are backed by Vulkan device-local memory mapped into the
# NvFBC-owned CUDA context. The capture object never frees those mappings.
edit('src/capture_cuda.h',[
 ('    CUcontext context{};',
  '    CUcontext context{};\n    DWORD CaptureCapacity()const{return capacity;}'),
 ('            cu.check(cu.allocate(&frame.rawPixels,capacity),"Allocate native NvFBC capture slot");frame.pixels=frame.rawPixels;',
  '            // Bound to external GPU memory by CudaPipeline::Init before capture starts.'),
 ('slot.pixels=slot.rawPixels;request.waitMs=16; request.buffer=slot.rawPixels; request.info=&info;',
  '''if(!slot.rawPixels)throw Error("Shared NvFBC CUDA capture destination not initialized");
        slot.pixels=slot.rawPixels;request.waitMs=16; request.buffer=slot.rawPixels; request.info=&info;'''),
 ('            if (frame.rawPixels) cu.freeMemory(frame.rawPixels);',
  '            // CudaPipeline owns and releases the external memory mapping.'),
])
edit('src/vulkan_presenter.h',[
 ('    VkBuffer output{}; VkDeviceMemory outputMemory{}; VkDeviceSize outputBytes{}, allocationBytes{};',
  '''    VkBuffer output{}; VkDeviceMemory outputMemory{}; VkDeviceSize outputBytes{}, allocationBytes{};
    struct SharedCapture {VkBuffer buffer{};VkDeviceMemory memory{};VkDeviceSize bytes{},allocation{};};
    std::array<SharedCapture,kNumCapture> sharedCapture{};'''),
 ('VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT,0,16};',
  'VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT,0,20};'),
 ('    void CreateOutput(uint32_t width,uint32_t height){',
  '''    void CreateSharedCapture(unsigned slot,VkDeviceSize bytes){
        auto& c=sharedCapture.at(slot);
        VkPhysicalDeviceProperties props{};vkGetPhysicalDeviceProperties(physical,&props);
        if(!bytes||bytes>props.limits.maxStorageBufferRange)throw Error("NvFBC shared capacity exceeds Vulkan storage range");
        c.bytes=bytes;
        VkPhysicalDeviceExternalBufferInfo e{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO};
        e.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;e.handleType=handleType;
        VkExternalBufferProperties ep{VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
        vkGetPhysicalDeviceExternalBufferProperties(physical,&e,&ep);
        if(!(ep.externalMemoryProperties.externalMemoryFeatures&VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT))
            throw Error("Native shared capture storage export unavailable");
        VkExternalMemoryBufferCreateInfo ex{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
        ex.handleTypes=handleType;
        VkBufferCreateInfo b{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        b.pNext=&ex;b.size=bytes;b.usage=e.usage;b.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        V(vkCreateBuffer(device,&b,nullptr,&c.buffer),"Create native shared capture buffer");
        VkMemoryRequirements req{};vkGetBufferMemoryRequirements(device,c.buffer,&req);c.allocation=req.size;
        VkPhysicalDeviceMemoryProperties mem{};vkGetPhysicalDeviceMemoryProperties(physical,&mem);
        uint32_t type=UINT32_MAX;
        for(uint32_t i=0;i<mem.memoryTypeCount;++i)
            if((req.memoryTypeBits&(1u<<i))&&(mem.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)){type=i;break;}
        if(type==UINT32_MAX)throw Error("No device-local native capture memory");
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};dedicated.buffer=c.buffer;
        VkExportMemoryAllocateInfo exp{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};exp.pNext=&dedicated;exp.handleTypes=handleType;
        VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};a.pNext=&exp;a.allocationSize=req.size;a.memoryTypeIndex=type;
        V(vkAllocateMemory(device,&a,nullptr,&c.memory),"Allocate native shared capture memory");
        V(vkBindBufferMemory(device,c.buffer,c.memory,0),"Bind native shared capture");
        V(vkResetCommandBuffer(cmd,0),"Reset shared capture ownership command");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        V(vkBeginCommandBuffer(cmd,&begin),"Begin native capture ownership command");
        Barrier(false,c.buffer);
        V(vkEndCommandBuffer(cmd),"End native capture ownership command");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&cmd;
        V(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE),"Release native capture ownership");
        V(vkQueueWaitIdle(queue),"Complete native capture ownership release");
    }
    HANDLE ExportCapture(unsigned slot){
        VkMemoryGetWin32HandleInfoKHR info{VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR};
        info.memory=sharedCapture.at(slot).memory;info.handleType=handleType;HANDLE h{};
        V(Proc<PFN_vkGetMemoryWin32HandleKHR>("vkGetMemoryWin32HandleKHR")(device,&info,&h),"Export native capture memory");return h;
    }
    void BindReadBuffer(VkBuffer buffer,VkDeviceSize bytes){
        VkDescriptorBufferInfo di{buffer,0,bytes};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=set;
        write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;write.pBufferInfo=&di;
        vkUpdateDescriptorSets(device,1,&write,0,nullptr);
    }
    void CreateOutput(uint32_t width,uint32_t height){'''),
 ('void Barrier(bool acquire){VkBufferMemoryBarrier b',
  'void Barrier(bool acquire,VkBuffer selected=VK_NULL_HANDLE){VkBufferMemoryBarrier b'),
 ('b.buffer=output;b.size=VK_WHOLE_SIZE;',
  'b.buffer=selected?selected:output;b.size=VK_WHOLE_SIZE;'),
 ('void Present(uint64_t inValue,FrameKind kind){',
  'void Present(uint64_t inValue,FrameKind kind,int captureSlot=-1,uint32_t capturePitch=0){'),
 ('if(!acquiredFrame)throw Error("No acquired swapchain image");uint32_t ix=imageIndex;',
  '''if(!acquiredFrame)throw Error("No acquired swapchain image");uint32_t ix=imageIndex;
        const bool direct=captureSlot>=0;
        VkBuffer selected=direct?sharedCapture.at(unsigned(captureSlot)).buffer:output;
        VkDeviceSize bytes=direct?sharedCapture.at(unsigned(captureSlot)).bytes:outputBytes;
        uint32_t pitch=direct?capturePitch:sourceExtent.width;
        if(!selected||pitch<sourceExtent.width||VkDeviceSize(pitch)*4*(sourceExtent.height-1)+VkDeviceSize(sourceExtent.width)*4>bytes)
            throw Error("Invalid native shared buffer pitch");
        if(sourceExtent.width!=extent.width||sourceExtent.height!=extent.height)
            throw Error("Native zero-copy output requires matching game/display dimensions");
        // Begin() has already verified the only in-flight descriptor use is complete.
        BindReadBuffer(selected,bytes);'''),
 ('V(vkBeginCommandBuffer(cmd,&bi),"Begin command buffer");Barrier(true);',
  'V(vkBeginCommandBuffer(cmd,&bi),"Begin command buffer");Barrier(true,selected);'),
 ('const uint32_t dims[]={sourceExtent.width,sourceExtent.height,drawExtent.width,drawExtent.height};',
  'const uint32_t dims[]={sourceExtent.width,sourceExtent.height,drawExtent.width,drawExtent.height,pitch};'),
 ('Barrier(false);V(vkEndCommandBuffer(cmd),"End command buffer");',
  'Barrier(false,selected);V(vkEndCommandBuffer(cmd),"End command buffer");'),
 ('DropSwap();DropOutput();if(pool)',
  '''DropSwap();DropOutput();
        for(auto& c:sharedCapture){if(c.buffer)vkDestroyBuffer(device,c.buffer,nullptr);if(c.memory)vkFreeMemory(device,c.memory,nullptr);}
        if(pool)'''),
])
edit('src/cuda_pipeline.h',[
 ('    const CaptureCuda& capture;',
  '    CaptureCuda& capture;\n    std::array<CUexternalMemory,kNumCapture> captureMemory{};'),
 ('CUevent ready{},lastDisplayRead{};',
  'CUevent ready{},lastDisplayRead{},directRelease{};'),
 ('CudaPipeline(const CudaDriver& driver,const CaptureCuda& source)',
  'CudaPipeline(const CudaDriver& driver,CaptureCuda& source)'),
 ('            state.lastDisplayRead=cu.event();',
  '            state.lastDisplayRead=cu.event();state.directRelease=cu.event();'),
 ('        presenter=&dx;\n    }\n    void CreateOutputs',
  '''        presenter=&dx;
        // Persistent CUDA mappings of the exact Vulkan-exported allocations.
        // No cudaMemAlloc capture surface, no intermediate capture copy.
        for(unsigned i=0;i<kNumCapture;++i){
            dx.vk.CreateSharedCapture(i,capture.CaptureCapacity());
            CUDA_EXTERNAL_MEMORY_HANDLE_DESC md{};md.type=CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32;
            md.handle.win32.handle=dx.vk.ExportCapture(i);md.size=dx.vk.sharedCapture[i].allocation;md.flags=CUDA_EXTERNAL_MEMORY_DEDICATED;
            auto result=cu.importMemory(&captureMemory[i],&md);CloseHandle(md.handle.win32.handle);
            cu.check(result,"Import native NvFBC capture allocation into CUDA");
            CUDA_EXTERNAL_MEMORY_BUFFER_DESC bd{};bd.size=dx.vk.sharedCapture[i].bytes;
            cu.check(cu.mapMemory(&capture.frames[i].rawPixels,captureMemory[i],&bd),"Map native NvFBC capture allocation");
            capture.frames[i].pixels=capture.frames[i].rawPixels;
        }
    }
    void CreateOutputs'''),
 ('        stage(previous);\n        stage(current);',
  '''        // Wait on the returned external ownership before any CUDA reader
        // touches either source. Unrecorded events are initially complete.
        for(int slot:{previous,current}){
            cu.check(cu.waitEvent(flowInputStream,states[slot].directRelease,0),"Wait for direct source return to CUDA (input)");
            cu.check(cu.waitEvent(flowOutputStream,states[slot].directRelease,0),"Wait for direct source return to CUDA (output)");
        }
        stage(previous);
        stage(current);'''),
 ('        cu.launch(renderKernel,displayStream,target.width,target.height,args);',
  '''        const bool direct=kind==FrameKind::Real;
        if(!direct)cu.launch(renderKernel,displayStream,target.width,target.height,args);'''),
 ('        dx.vk.Present(signal.params.fence.value,kind);',
  '''        dx.vk.Present(signal.params.fence.value,kind,direct?current:-1,direct?images.currentPitch:0);
        if(direct){
            // Vulkan signals only after releasing ownership of the actual
            // captured buffer. Retire and optical flow wait for this event.
            CUDA_EXTERNAL_SEMAPHORE_WAIT_PARAMS returned{};returned.params.fence.value=dx.vk.interopValue;
            cu.check(cu.waitSemaphore(&interopSemaphore,&returned,1,displayStream),"Return native capture ownership to CUDA");
            cu.check(cu.recordEvent(state.directRelease,displayStream),"Publish returned native capture ownership");
        }'''),
 ('            if (state.lastDisplayRead) cu.destroyEvent(state.lastDisplayRead);',
  '            if (state.lastDisplayRead) cu.destroyEvent(state.lastDisplayRead);\n            if(state.directRelease)cu.destroyEvent(state.directRelease);'),
 ('        if (flow) api.nvOFDestroy(flow);',
  '''        if (flow) api.nvOFDestroy(flow);
        for(unsigned i=0;i<kNumCapture;++i){
            if(capture.frames[i].rawPixels)cu.freeMemory(capture.frames[i].rawPixels);
            capture.frames[i].rawPixels=0;capture.frames[i].pixels=0;
            if(captureMemory[i])cu.destroyMemory(captureMemory[i]);
        }'''),
])
edit('src/main.cpp',[
 ('        OverlayWindow overlay;overlay.Init(game.window);',
  '''        // Do not lock in the launcher's desktop dimensions while focus is
        // returning to the game. No forced mode change or scaling fallback.
        MarkCrashStage(CrashStage::WaitMode);
        const ULONGLONG until=GetTickCount64()+3000;unsigned stable=0;
        UINT lastW=0,lastH=0;
        while(stable<3){
            if(tracked.Exited()||g_userStopRequested.load())return SessionOutcome::Stop;
            HWND foreground=GetForegroundWindow();DWORD owner=0;
            if(foreground)GetWindowThreadProcessId(foreground,&owner);
            RECT c{};MONITORINFO mi{sizeof(mi)};
            if(owner==game.pid&&!IsIconic(game.window)&&GetClientRect(game.window,&c)&&
                GetMonitorInfoW(MonitorFromWindow(game.window,MONITOR_DEFAULTTONEAREST),&mi)&&
                c.right-c.left==mi.rcMonitor.right-mi.rcMonitor.left&&
                c.bottom-c.top==mi.rcMonitor.bottom-mi.rcMonitor.top&&c.right>0&&c.bottom>0){
                UINT w=UINT(c.right-c.left),h=UINT(c.bottom-c.top);
                stable=(w==lastW&&h==lastH)?stable+1:1;lastW=w;lastH=h;
            }else stable=0;
            if(stable>=3)break;
            if(GetTickCount64()>=until){
                Status(L"FG error: Native zero-copy requires matching game and logical fullscreen display resolution. Set NVIDIA full-screen GPU scaling and matching fullscreen mode.");
                return SessionOutcome::Stop;
            }
            Sleep(20);
        }
        OverlayWindow overlay;overlay.Init(game.window);'''),
 ('        MarkCrashStage(CrashStage::Cuda);\n        CudaDriver cuda;',
  '''        if(overlay.width!=gameW||overlay.height!=gameH){
            Status(L"FG error: Native fullscreen resolution changed before initialization.");
            return SessionOutcome::Stop;
        }
        MarkCrashStage(CrashStage::Cuda);
        CudaDriver cuda;'''),
 ('                    overlay.SetVisible(false);\n                    // Only the display surface changes; preserve native source resources.',
  '''                    overlay.SetVisible(false);
                    if(overlay.width!=gameW||overlay.height!=gameH)return SessionOutcome::Recreate;
                    // Only the display surface changes; preserve native source resources.'''),
 ('            overlay.SetVisible(focused);',
  '            if(!focused)overlay.SetVisible(false);'),
 ('                const auto& incoming=capture.frames[next];',
  '''                const auto& incoming=capture.frames[next];
                if(incoming.width!=gameW||incoming.height!=gameH||
                   dx.vk.extent.width!=gameW||dx.vk.extent.height!=gameH){
                    overlay.SetVisible(false);
                    Status(L"FG error: Zero-copy native extent mismatch: capture "+std::to_wstring(incoming.width)+L"x"+std::to_wstring(incoming.height)+
                        L"; game "+std::to_wstring(gameW)+L"x"+std::to_wstring(gameH)+
                        L"; Vulkan "+std::to_wstring(dx.vk.extent.width)+L"x"+std::to_wstring(dx.vk.extent.height));
                    return SessionOutcome::Stop;
                }'''),
 ('                if(useFlow) flow.Request(current,previous,sequence);',
  '''                // Real capture goes directly to Vulkan first. Starting flow
                // earlier would let CUDA read a Vulkan-owned capture buffer.
                if(useFlow&&!realFramePending) flow.Request(current,previous,sequence);'''),
 ('                    if(!realFramePending && useFlow && !flowReady){',
  '                    if(!realFramePending && (factor<=1 || !flowReady)){'),
 ('                    if(!dx.vk.Begin()){SwitchToThread();continue;}',
  '''                    if(realFramePending&&flow.UsesSlot(current)){SwitchToThread();continue;}
                    if(!dx.vk.Begin()){SwitchToThread();continue;}'''),
 ('                    realFramePending=false;\n                    // Measure',
  '                    overlay.SetVisible(true);\n                    realFramePending=false;\n                    // Measure'),
])
(fg/'src'/'fg.frag').write_text(r'''#version 450
layout(set=0,binding=0,std430) readonly buffer Pixels { uint pixels[]; };
layout(push_constant) uniform P { uint sourceWidth; uint sourceHeight; uint outputWidth; uint outputHeight; uint pitch; } p;
layout(location=0) out vec4 color;
void main(){
    uvec2 xy=uvec2(gl_FragCoord.xy);
    uint v=pixels[xy.y*p.pitch+xy.x];
    color=vec4(float((v>>16)&255u),float((v>>8)&255u),float(v&255u),255.0)/255.0;
}
''',encoding='utf-8',newline='\n')
with (fg/'README.md').open('a',encoding='utf-8') as f:
    f.write('\n## Native zero-copy capture experiment\nNvFBC writes directly into persistent Vulkan-exported, CUDA-mapped device-local capture buffers. Real frames bypass the CUDA render kernel and are sampled directly from the same pitched allocation by Vulkan. Generated frames are computed directly into the existing shared output allocation. No intermediate real-frame copy, vkCmdCopyBufferToImage, capture normalization, or shader resizing. Native NvOFA uses direct captured dimensions; its required luma staging remains. The actual game, captured source and logical monitor-sized swapchain must match or FG stops without fallback. CUDA/Vulkan ownership is transferred with the existing timeline semaphore; flow starts only after real-frame submission and waits for ownership return. This changes flow scheduling and may affect generated-frame cadence. Native NGX still uses its own required color/depth/output processing and final blit; approximate depth is normally unavailable for the freshly submitted real frame. Standard Vulkan still renders into a separate presentable swapchain image, not direct physical scanout of NvFBC memory. The legacy Windows NvFBC runtime has not been verified to accept Vulkan-imported CUDA addresses. Failure is explicit; no copied capture fallback. GPU testing required.\n')
print('Native shared capture allocations and direct real-frame Vulkan reading applied.')

# Capture dimensions are authoritative; the HWND is only a presentation surface.
# A native-size swapchain on Win32 needs a supported present-scaling extension
# when the actual surface extent differs. Do not shader-scale or copy instead.
edit('src/main.cpp',[
 ('                    if(overlay.width!=gameW||overlay.height!=gameH)return SessionOutcome::Recreate;\n                    // Only the display surface changes; preserve native source resources.\n                    dx.vk.Resize(overlay.width,overlay.height);',
  '''                    // Resize the native-size swapchain only after capture has established its extent.
                    if(dx.vk.swap)dx.vk.Resize(dx.vk.sourceExtent.width,dx.vk.sourceExtent.height);'''),
 ('''                if(incoming.width!=gameW||incoming.height!=gameH||
                   dx.vk.extent.width!=gameW||dx.vk.extent.height!=gameH){
                    overlay.SetVisible(false);
                    Status(L"FG error: Zero-copy native extent mismatch: capture "+std::to_wstring(incoming.width)+L"x"+std::to_wstring(incoming.height)+
                        L"; game "+std::to_wstring(gameW)+L"x"+std::to_wstring(gameH)+
                        L"; Vulkan "+std::to_wstring(dx.vk.extent.width)+L"x"+std::to_wstring(dx.vk.extent.height));
                    return SessionOutcome::Stop;
                }''',
  '''                // Never compare the source against the stretched HWND dimensions.
                if(!incoming.width||!incoming.height)throw Error("Invalid native capture extent");'''),
 ('                    MarkCrashStage(CrashStage::OutputAlloc);\n                    gpu.CreateOutputs(dx,incoming.width,incoming.height);',
  '''                    MarkCrashStage(CrashStage::VulkanInit);
                    dx.vk.MakeSwap(incoming.width,incoming.height);
                    MarkCrashStage(CrashStage::OutputAlloc);
                    gpu.CreateOutputs(dx,incoming.width,incoming.height);'''),
])
edit('src/vulkan_presenter.h',[
 ('    bool windowedExclusiveControl=false;',
  '''    bool windowedExclusiveControl=false;
    bool surfaceScalingAvailable=false,swapScalingAvailable=false;'''),
 ('        VkInstanceCreateInfo ic{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};',
  '''        surfaceScalingAvailable=Has(iex,VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME)&&Has(iex,VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
        if(surfaceScalingAvailable&&std::none_of(ie.begin(),ie.end(),[](const char* s){return !strcmp(s,VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);}))
            ie.push_back(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
        VkInstanceCreateInfo ic{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};'''),
 ('        float priority=1;VkDeviceQueueCreateInfo qi',
  '''        VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT scalingFeature{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT};
        if(surfaceScalingAvailable&&Has(ex,VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME)){
            VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            query.pNext=&scalingFeature;vkGetPhysicalDeviceFeatures2(physical,&query);
            if(scalingFeature.swapchainMaintenance1){
                swapScalingAvailable=true;pid.pNext=&scalingFeature;
                if(std::none_of(de.begin(),de.end(),[](const char* s){return !strcmp(s,VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);}))
                    de.push_back(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
            }
        }
        float priority=1;VkDeviceQueueCreateInfo qi'''),
 ('        MakeSwap(w,h);\n    }\n    VkImageView View',
  '''        // Native capture has not yet reported its dimensions. Create only
        // the Vulkan device and resources needed to import the capture slots.
        (void)w;(void)h;
    }
    VkImageView View'''),
 ('extent=caps.currentExtent.width==UINT32_MAX?VkExtent2D{std::clamp(w,caps.minImageExtent.width,caps.maxImageExtent.width),std::clamp(h,caps.minImageExtent.height,caps.maxImageExtent.height)}:caps.currentExtent;if(!extent.width||!extent.height)throw Error("Vulkan surface minimized");',
  '''const auto surfaceExtent=caps.currentExtent.width==UINT32_MAX?VkExtent2D{w,h}:caps.currentExtent;
        if(!surfaceExtent.width||!surfaceExtent.height||!w||!h)throw Error("Vulkan surface minimized or source invalid");
        extent={w,h};
        const bool needsScaling=extent.width!=surfaceExtent.width||extent.height!=surfaceExtent.height;'''),
 ('        uint32_t count=std::max(2u,caps.minImageCount);',
  '''        VkSwapchainPresentScalingCreateInfoEXT scaling{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_SCALING_CREATE_INFO_EXT};
        if(needsScaling){
            // On Win32 the ordinary swapchain extent must equal the HWND.
            // This optional extension allows the presentation engine to
            // stretch a native-sized image without an FG shader/copy pass.
            if(!swapScalingAvailable)throw std::runtime_error("Native "+std::to_string(w)+"x"+std::to_string(h)+
                " capture confirmed; driver lacks Vulkan present scaling. Set Windows display to that resolution with NVIDIA GPU full-screen scaling.");
            auto getCaps=reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR>(
                vkGetInstanceProcAddr(instance,"vkGetPhysicalDeviceSurfaceCapabilities2KHR"));
            if(!getCaps)throw Error("Vulkan present scaling capability query missing");
            bool supported=false;
            for(auto candidate:{VK_PRESENT_MODE_IMMEDIATE_KHR,VK_PRESENT_MODE_MAILBOX_KHR}){
                if(std::find(modes.begin(),modes.end(),candidate)==modes.end())continue;
                VkSurfacePresentModeEXT queryMode{VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_EXT};queryMode.presentMode=candidate;
                VkPhysicalDeviceSurfaceInfo2KHR query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR};
                query.surface=surface;query.pNext=&queryMode;
                VkSurfaceFullScreenExclusiveInfoEXT queryExclusive{VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT};
                queryExclusive.fullScreenExclusive=VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT;
                if(windowedExclusiveControl)queryMode.pNext=&queryExclusive;
                VkSurfacePresentScalingCapabilitiesEXT scaleCaps{VK_STRUCTURE_TYPE_SURFACE_PRESENT_SCALING_CAPABILITIES_EXT};
                VkSurfaceCapabilities2KHR result{VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR};result.pNext=&scaleCaps;
                if(getCaps(physical,&query,&result)!=VK_SUCCESS)continue;
                if(!(scaleCaps.supportedPresentScaling&VK_PRESENT_SCALING_STRETCH_BIT_EXT))continue;
                if(extent.width<scaleCaps.minScaledImageExtent.width||extent.height<scaleCaps.minScaledImageExtent.height||
                   extent.width>scaleCaps.maxScaledImageExtent.width||extent.height>scaleCaps.maxScaledImageExtent.height)continue;
                mode=candidate;caps=result.surfaceCapabilities;supported=true;break;
            }
            if(!supported)throw std::runtime_error("Native "+std::to_string(w)+"x"+std::to_string(h)+
                " capture confirmed; no low-latency fullscreen stretch supported. Set Windows display to capture resolution with NVIDIA GPU full-screen scaling.");
            scaling.scalingBehavior=VK_PRESENT_SCALING_STRETCH_BIT_EXT;
        }else if(w<caps.minImageExtent.width||h<caps.minImageExtent.height||w>caps.maxImageExtent.width||h>caps.maxImageExtent.height)
            throw Error("Native extent outside Vulkan surface limits");
        uint32_t count=std::max(2u,caps.minImageCount);'''),
 ('        if(windowedExclusiveControl)ll.pNext=&exclusive;',
  '''        if(windowedExclusiveControl)ll.pNext=&exclusive;
        if(needsScaling){scaling.pNext=ll.pNext;ll.pNext=&scaling;}'''),
])
with (fg/'README.md').open('a',encoding='utf-8') as f:
    f.write('\n## Capture-authoritative native swapchain\nThe first actual NvFBC capture determines all native image extents. The HWND is independent and may be 1920x1080 while capture is 1280x960. Vulkan swapchain creation is deferred until capture dimensions are known. When they differ from the Win32 surface, query EXT surface/swapchain maintenance1 for stretch support and valid native extents in immediate or mailbox mode. If supported, the presentation engine stretches the native swapchain image; otherwise fail explicitly and request a matching Windows logical display mode. No application pixel resizing, copy fallback or automatic Windows display mode change. Driver-side scaling may still involve internal compositor work and does not guarantee direct scanout. No change to shared capture buffers or real-frame direct read.\\n')
print('Capture-authoritative native swapchain and capability-checked driver stretch applied.')
