// Only the worker process loads the capture and presentation drivers.
struct MappingView {
    SessionState* data{};
    ~MappingView(){if(data)UnmapViewOfFile(data);}
};
void CopySettings(SessionState& s){
    InterlockedExchange(&s.mode,g_fgMode.load());
    InterlockedExchange(&s.target,g_targetHz.load());
    InterlockedExchange(&s.radius,g_centerRadiusPx.load());
    InterlockedExchange(&s.feather,g_centerFeatherPx.load());
}
void RunIsolated(Game game){
    try {
        auto pid=GetCurrentProcessId();
        auto name=L"Local\\FGSession-"+std::to_wstring(pid);
        fgwarp::Handle mapping{CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(SessionState),name.c_str())};
        if(!mapping.value || GetLastError()==ERROR_ALREADY_EXISTS)throw Error("Create session mapping",GetLastError());
        MappingView v{static_cast<SessionState*>(MapViewOfFile(mapping.value,FILE_MAP_ALL_ACCESS,0,0,sizeof(SessionState)))};
        if(!v.data)throw Error("Map session settings",GetLastError());
        auto& s=*v.data;s.game=game.window;s.pid=game.pid;s.warp=fgwarp::requested.load();s.native=1;s.backend=1;CopySettings(s);
        wchar_t file[32768]{};if(!GetModuleFileNameW(nullptr,file,32768))throw Error("Find FG executable",GetLastError());
        for(;;){
            auto cmd=L"\""+std::wstring(file)+L"\" --fg-session "+std::to_wstring(pid);
            STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION child{};
            InterlockedExchange64(&s.heartbeat,static_cast<LONG64>(GetTickCount64()));
            if(!CreateProcessW(file,cmd.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&startup,&child))throw Error("Start capture process",GetLastError());
            CloseHandle(child.hThread);fgwarp::Handle proc{child.hProcess};
            ULONGLONG stopping=0; LONG shown=-1; bool watchdog=false;
            while(WaitForSingleObject(proc.value,15)==WAIT_TIMEOUT){
                CopySettings(s);
                const LONG backend=InterlockedCompareExchange(&s.backend,0,0);
                if(backend!=shown){shown=backend;PostMessageW(g_control,WM_FG_BACKEND,WPARAM(backend),0);}
                if(g_userStopRequested.load()){
                    InterlockedExchange(&s.stop,1);
                    if(!stopping)stopping=GetTickCount64();
                    if(GetTickCount64()-stopping>4000)TerminateProcess(proc.value,ERROR_CANCELLED);
                }else if(backend==1||backend==2){
                    // Private native calls may block despite the nonblocking mutex.
                    // Keep the launcher alive; terminate the stuck native session without retry.
                    const auto hb=static_cast<ULONGLONG>(InterlockedCompareExchange64(&s.heartbeat,0,0));
                    if(GetTickCount64()-hb>6000){watchdog=true;TerminateProcess(proc.value,ERROR_TIMEOUT);}
                }
            }
            DWORD code=0;GetExitCodeProcess(proc.value,&code);
            if(!g_userStopRequested.load()){
                if(watchdog){wchar_t msg[256];swprintf(msg,256,L"FG error: D3DKMT native watchdog timeout. Last stage: %ls",s.stage);Status(msg);}
                else if(*s.error)Status(s.error);
                else if(code){wchar_t msg[160];swprintf(msg,160,L"FG error: Capture session exited (0x%08lX). Stage: %ls",code,s.stage);Status(msg);}
            }
            break;
        }
    }catch(const std::exception& e){Status(L"FG error: "+Wide(e.what()));}
    g_lastGameExited.store(!IsWindow(game.window));g_running.store(false);g_engineFinished.store(true);
    PostMessageW(g_control,WM_FG_STOPPED,0,0);
}
int RunChild(DWORD parent){
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    auto name=L"Local\\FGSession-"+std::to_wstring(parent);
    fgwarp::Handle mapping{OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,name.c_str())};
    fgwarp::Handle owner{OpenProcess(SYNCHRONIZE,FALSE,parent)};
    if(!mapping.value||!owner.value)return ERROR_INVALID_HANDLE;
    MappingView v{static_cast<SessionState*>(MapViewOfFile(mapping.value,FILE_MAP_ALL_ACCESS,0,0,sizeof(SessionState)))};
    if(!v.data)return ERROR_INVALID_DATA;
    session=v.data;v124::stageCallback=[](const wchar_t* stage){lstrcpynW(session->stage,stage,96);};fgwarp::requested.store(session->warp!=0);g_nativeRequested.store(true);
    std::atomic<bool> finished{false};
    std::thread monitor([&]{
        while(!finished.load()){
            if(WaitForSingleObject(owner.value,10)==WAIT_OBJECT_0)ExitProcess(ERROR_CANCELLED);
            if(session->stop){g_userStopRequested.store(true);g_running.store(false);}
        }
    });
    Engine(Game{session->game,session->pid,L""});
    finished.store(true);monitor.join();return 0;
}
