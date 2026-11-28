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
 ('        CudaDriver cuda;\n        CaptureCuda capture(cuda); capture.Init(game.pid);\n        VkDevices dx; dx.Init(overlay.hwnd,overlay.width,overlay.height,cuda);\n        CudaPipeline gpu(cuda,capture); gpu.Init(dx);',
  '''        MarkCrashStage(CrashStage::Cuda);
        CudaDriver cuda;
        MarkCrashStage(CrashStage::CaptureInit);
        CaptureCuda capture(cuda); capture.Init(game.pid);
        MarkCrashStage(CrashStage::VulkanInit);
        VkDevices dx; dx.Init(overlay.hwnd,overlay.width,overlay.height,cuda);
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
    f.write('\n## Startup and crash isolation\nV15-style independent game and display dimensions, with shared memory crash breadcrumbs. A shared memory breadcrumb records the latest engine/capture/flow stage, with a best-effort exception address if the worker faults. No continuous disk logging, no game process modification. If a crash recurs, copy the expanded FG error or the Windows Application Error event.\\n')
print('Crash-only stage reporting applied.')

# Preserve the adaptive build's V15 game-resolution normalization and standard
# monitor-sized WSI. On matching display modes the original shader is 1:1.
# Do not explicitly disable Reflex during swapchain destruction; swapchain
# lifetime ends anyway. This isolates the suspected vendor cleanup call.
edit('src/vulkan_presenter.h',[
 ('if(swap&&setSleep)setSleep(device,swap,nullptr);',
  '/* Reflex state belongs to the destroyed swapchain; no redundant driver call. */'),
])
with (fg/'README.md').open('a',encoding='utf-8') as f:
    f.write('\n## V15-compatible resolution recovery\nRestored adaptive baseline game-sized normalization and the independent logical monitor-sized Vulkan surface. Native NvOFA bypasses percentage calculations and staging resizing. The final shader/NGX blit is one-to-one when game and logical monitor mode agree; otherwise the original V15-style final fullscreen stretch is retained. No experimental WSI extensions, no strict mismatch rejection, no forced display changes. Removed redundant Reflex disable call during swapchain destruction to isolate the reported teardown fault. Reflex + Boost remains enabled during presentation. GPU runtime verification required.\n')
print('Restored V15-compatible native processing and monitor presentation; retained direct native NvOFA.')

# Preserve captured color. Never reduce a driver-scaled capture to game-client
# size and then stretch it a second time. The native source is the actual
# NvFBC buffer, not a reconstructed lower-resolution image.
edit('src/main.cpp',[
 ('                    if(usable)gpu.NormalizeCapture(available,gameW,gameH);',
  '                    // Preserve NvFBC pixels and pitch. No capture normalization.'),
 ('Status(L"Native source: "+std::to_wstring(incoming.width)+L"x"+std::to_wstring(incoming.height));',
  '''Status(L"Native source: "+std::to_wstring(incoming.width)+L"x"+std::to_wstring(incoming.height)+
                        L" | game "+std::to_wstring(gameW)+L"x"+std::to_wstring(gameH)+
                        L" | output "+std::to_wstring(dx.vk.extent.width)+L"x"+std::to_wstring(dx.vk.extent.height));'''),
])
# The existing renderFrame kernel performs a bit-exact BGRA copy at zero
# horizon and equal dimensions. The existing Vulkan shader reads exactly one
# pixel at matching extents. Preserve these two paths and all FG/NGX math.
# Prefer a normal SDR swapchain with no sRGB attachment conversion; keep the
# original supported-format selection if the preferred color space is absent.
edit('src/vulkan_presenter.h',[
 ('auto it=std::find_if(fs.begin(),fs.end(),[](auto f){return f.format==VK_FORMAT_B8G8R8A8_UNORM||f.format==VK_FORMAT_R8G8B8A8_UNORM;});',
  '''auto it=std::find_if(fs.begin(),fs.end(),[](auto f){return f.format==VK_FORMAT_B8G8R8A8_UNORM&&f.colorSpace==VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;});
        if(it==fs.end())it=std::find_if(fs.begin(),fs.end(),[](auto f){return f.format==VK_FORMAT_R8G8B8A8_UNORM&&f.colorSpace==VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;});
        if(it==fs.end())it=std::find_if(fs.begin(),fs.end(),[](auto f){return f.format==VK_FORMAT_B8G8R8A8_UNORM||f.format==VK_FORMAT_R8G8B8A8_UNORM;});'''),
])
with (fg/'README.md').open('a',encoding='utf-8') as f:
    f.write('\n## Color-preserving native capture (supersedes earlier normalization)\nNvFBC output pixels, dimensions and pitch are now used unchanged for CUDA extrapolation and Native NvOFA. No downscale to game client dimensions. The title reports capture/game/Vulkan output extents. Equal source/output dimensions are a one-to-one copy for unwarped real frames; if the driver returns a pre-scaled image, FG cannot recover the original game framebuffer. If source and output differ, the existing final fullscreen scaling remains necessary. A preferred ordinary SDR UNORM swapchain avoids accidental selection of a nonstandard color space when SDR is available. No sharpening or added color transformations. No changes to adaptive Reflex 2, timing or reduced-resolution NvOFA. Windows compilation and target display verification required.\\n')
print('Removed capture normalization; preserved exact equal-size real-frame color path.')
