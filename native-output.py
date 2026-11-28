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

# Startup synchronization and crash-only diagnostic breadcrumbs.
edit('src/main.cpp',[
 ('std::atomic<bool> g_running{false};', '''std::atomic<bool> g_running{false};
// Shared only between our launcher and its own worker; no file logging.
enum class CrashStage:LONG { None, Startup, WaitMode, Cuda, CaptureInit, VulkanInit,
    PipelineInit, CaptureGrab, OutputAlloc, FlowInit, FlowStage, FlowExecute,
    Draw, NgxCreate, NgxEvaluate, QueueSubmit, QueuePresent, Shutdown, CaptureTeardown, FlowTeardown, VulkanTeardown };
struct CrashState { volatile LONG stage[4]; volatile LONG faultStage, faultCode; ULONG_PTR address; };
CrashState* g_crashState=nullptr;
thread_local unsigned g_crashLane=0;
thread_local CrashStage g_crashStage=CrashStage::None;
void MarkCrashStage(CrashStage stage) noexcept {
    g_crashStage=stage;
    if(g_crashState)InterlockedExchange(&g_crashState->stage[g_crashLane],LONG(stage));
}
const wchar_t* CrashStageName(LONG v) noexcept {
    switch(static_cast<CrashStage>(v)){
    case CrashStage::Startup:return L"worker startup";
    case CrashStage::WaitMode:return L"wait for native fullscreen mode";
    case CrashStage::Cuda:return L"CUDA driver/context";
    case CrashStage::CaptureInit:return L"NvFBC initialization";
    case CrashStage::VulkanInit:return L"Vulkan initialization";
    case CrashStage::PipelineInit:return L"CUDA pipeline initialization";
    case CrashStage::CaptureGrab:return L"NvFBC grab";
    case CrashStage::OutputAlloc:return L"native output allocation";
    case CrashStage::FlowInit:return L"NvOFA initialization";
    case CrashStage::FlowStage:return L"NvOFA luma staging";
    case CrashStage::FlowExecute:return L"NvOFA execution";
    case CrashStage::Draw:return L"CUDA draw / Vulkan recording";
    case CrashStage::NgxCreate:return L"NGX feature creation";
    case CrashStage::NgxEvaluate:return L"NGX evaluation";
    case CrashStage::QueueSubmit:return L"Vulkan queue submit";
    case CrashStage::QueuePresent:return L"Vulkan present";
    case CrashStage::Shutdown:return L"GPU worker shutdown";
    case CrashStage::CaptureTeardown:return L"NvFBC teardown";
    case CrashStage::FlowTeardown:return L"CUDA / NvOFA teardown";
    case CrashStage::VulkanTeardown:return L"Vulkan teardown";
    default:return L"not reached";
    }
}
LONG WINAPI RecordWorkerFault(EXCEPTION_POINTERS* e) {
    if(g_crashState&&e&&e->ExceptionRecord){
        g_crashState->address=reinterpret_cast<ULONG_PTR>(e->ExceptionRecord->ExceptionAddress);
        InterlockedExchange(&g_crashState->faultStage,LONG(g_crashStage));
        InterlockedExchange(&g_crashState->faultCode,LONG(e->ExceptionRecord->ExceptionCode));
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
'''),
 ('        OverlayWindow overlay;overlay.Init(game.window);', '''        // Launcher temporarily owns focus. Do not initialize capture while CS2
        // is still returning to its exclusive fullscreen display mode.
        MarkCrashStage(CrashStage::WaitMode);
        unsigned stable=0;UINT lastW=0,lastH=0;
        const ULONGLONG deadline=GetTickCount64()+3000;
        while(stable<3){
            if(tracked.Exited()||g_userStopRequested.load(std::memory_order_acquire))return SessionOutcome::Stop;
            DWORD pid=0;HWND fg=GetForegroundWindow();if(fg)GetWindowThreadProcessId(fg,&pid);
            RECT c{};MONITORINFO m{sizeof(m)};
            if(pid==game.pid&&!IsIconic(game.window)&&GetClientRect(game.window,&c)&&
                GetMonitorInfoW(MonitorFromWindow(game.window,MONITOR_DEFAULTTONEAREST),&m)&&
                c.right-c.left==m.rcMonitor.right-m.rcMonitor.left&&
                c.bottom-c.top==m.rcMonitor.bottom-m.rcMonitor.top&&c.right>0&&c.bottom>0){
                UINT w=UINT(c.right-c.left),h=UINT(c.bottom-c.top);
                stable=(w==lastW&&h==lastH)?stable+1:1;lastW=w;lastH=h;
            }else stable=0;
            if(stable>=3)break;
            if(GetTickCount64()>=deadline)throw Error("Native fullscreen mode did not settle; focus game at matching display resolution");
            Sleep(20);
        }
        OverlayWindow overlay;overlay.Init(game.window);'''),
 ('        CudaDriver cuda;\n        CaptureCuda capture(cuda); capture.Init(game.pid);\n        if(overlay.width!=gameW||overlay.height!=gameH)',
  '''        if(overlay.width!=gameW||overlay.height!=gameH)'''),
 ('        VkDevices dx; dx.Init(overlay.hwnd,gameW,gameH,cuda);\n        CudaPipeline gpu(cuda,capture); gpu.Init(dx);',
  '''        MarkCrashStage(CrashStage::Cuda);
        CudaDriver cuda;
        MarkCrashStage(CrashStage::CaptureInit);
        CaptureCuda capture(cuda); capture.Init(game.pid);
        MarkCrashStage(CrashStage::VulkanInit);
        VkDevices dx; dx.Init(overlay.hwnd,gameW,gameH,cuda);
        MarkCrashStage(CrashStage::PipelineInit);
        CudaPipeline gpu(cuda,capture); gpu.Init(dx);'''),
 ('        std::thread producer([&]{\n            try{',
  '        std::thread producer([&]{\n            g_crashLane=1;MarkCrashStage(CrashStage::CaptureGrab);\n            try{'),
 ('                    const bool usable=capture.GrabSlot(available);',
  '                    MarkCrashStage(CrashStage::CaptureGrab);\n                    const bool usable=capture.GrabSlot(available);'),
 ('                    gpu.CreateOutputs(dx,incoming.width,incoming.height);',
  '                    MarkCrashStage(CrashStage::OutputAlloc);\n                    gpu.CreateOutputs(dx,incoming.width,incoming.height);'),
 ('                    gpu.Draw(dx,current,previous>=0?previous:current,prediction,kind,flowReady,',
  '                    MarkCrashStage(CrashStage::Draw);\n                    gpu.Draw(dx,current,previous>=0?previous:current,prediction,kind,flowReady,'),
 ('    HANDLE stop=nullptr,job=nullptr,process=nullptr;',
  '    HANDLE stop=nullptr,job=nullptr,process=nullptr,crashMap=nullptr;\n    CrashState* crash=nullptr;'),
 ('        if(process)CloseHandle(process);',
  '        if(crash)UnmapViewOfFile(crash);\n        if(crashMap)CloseHandle(crashMap);\n        if(process)CloseHandle(process);'),
 ('        if(!stop||GetLastError()==ERROR_ALREADY_EXISTS)throw Error("Create FG worker stop event",GetLastError());',
  '''        if(!stop||GetLastError()==ERROR_ALREADY_EXISTS)throw Error("Create FG worker stop event",GetLastError());
        const std::wstring mapName=name+L"-Crash";
        crashMap=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(CrashState),mapName.c_str());
        if(crashMap)crash=static_cast<CrashState*>(MapViewOfFile(crashMap,FILE_MAP_ALL_ACCESS,0,0,sizeof(CrashState)));'''),
 ('        if(exitCode&&!g_userStopRequested.load()&&!tracked.Exited())Status(L"FG error: GPU worker exited ("+std::to_wstring(exitCode)+L")");',
  '''        if(exitCode&&!g_userStopRequested.load()&&!tracked.Exited()){
            std::wstring reason=L"FG error: GPU worker exited ("+std::to_wstring(exitCode)+L")";
            if(crash){
                if(crash->faultCode){
                    wchar_t address[64]{};swprintf_s(address,L"0x%llX",static_cast<unsigned long long>(crash->address));
                    reason+=L"\\nFault stage: "+std::wstring(CrashStageName(crash->faultStage))+L"\\nAddress: "+address;
                }
                reason+=L"\\nEngine: "+std::wstring(CrashStageName(crash->stage[0]))+
                    L"\\nCapture: "+CrashStageName(crash->stage[1])+L"\\nFlow: "+CrashStageName(crash->stage[2]);
            }
            Status(reason);
        }'''),
 ('        g_childStop=OpenEventW(SYNCHRONIZE,FALSE,name);',
  '''        g_childStop=OpenEventW(SYNCHRONIZE,FALSE,name);
        const std::wstring mapName=std::wstring(name)+L"-Crash";
        HANDLE crashMap=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,mapName.c_str());
        if(crashMap){g_crashState=static_cast<CrashState*>(MapViewOfFile(crashMap,FILE_MAP_ALL_ACCESS,0,0,sizeof(CrashState)));CloseHandle(crashMap);}
        g_crashLane=3;MarkCrashStage(CrashStage::Startup);
        SetUnhandledExceptionFilter(RecordWorkerFault);'''),
])
edit('src/cuda_pipeline.h',[
 ('    void Calculate(int current, int previous) {',
  '    void Calculate(int current, int previous) {\n        g_crashLane=2;'),
 ('        if (!flow) initializeFlow(frame.width,frame.height);',
  '        if (!flow){MarkCrashStage(CrashStage::FlowInit);initializeFlow(frame.width,frame.height);}\n        MarkCrashStage(CrashStage::FlowStage);'),
 ('        NvCheck(api.nvOFExecute(flow,&in,&out),"Execute CUDA NvOFA");',
  '        MarkCrashStage(CrashStage::FlowExecute);\n        NvCheck(api.nvOFExecute(flow,&in,&out),"Execute CUDA NvOFA");'),
 ('    ~CudaPipeline() {','    ~CudaPipeline() {\n        MarkCrashStage(CrashStage::FlowTeardown);'),
])
edit('src/capture_cuda.h',[
 ('    ~CaptureCuda() {','    ~CaptureCuda() {\n        MarkCrashStage(CrashStage::CaptureTeardown);'),
])
edit('src/vulkan_presenter.h',[
 ('V(vkQueueSubmit(queue,1,&submit,finished),"Submit Vulkan draw");','MarkCrashStage(CrashStage::QueueSubmit);V(vkQueueSubmit(queue,1,&submit,finished),"Submit Vulkan draw");'),
 ('auto result=vkQueuePresentKHR(queue,&pr);','MarkCrashStage(CrashStage::QueuePresent);auto result=vkQueuePresentKHR(queue,&pr);'),
 ('~VulkanPresenter(){','~VulkanPresenter(){MarkCrashStage(CrashStage::VulkanTeardown);'),
])
edit('src/native_live.h',[
 ('if(!handle){N(fg_ngx_experiment::create','if(!handle){MarkCrashStage(CrashStage::NgxCreate);N(fg_ngx_experiment::create'),
 ('        N(fg_ngx_experiment::record_real','        MarkCrashStage(CrashStage::NgxEvaluate);\n        N(fg_ngx_experiment::record_real'),
])
with (fg/'README.md').open('a',encoding='utf-8') as f:
    f.write('\n## Startup and crash isolation\nWait for matching focused game/monitor dimensions for three consecutive checks before allocating CUDA, NvFBC or Vulkan resources. A shared memory breadcrumb records the latest engine/capture/flow stage, with a best-effort exception address if the worker faults. No continuous disk logging, no game process modification. If a crash recurs, copy the expanded FG error or the Windows Application Error event.\\n')
print('Native fullscreen startup synchronization and crash-only stage reporting applied.')
