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
    InterlockedExchange(&s.lateInput,g_lateInput.load()?1:0);
}
void RunIsolated(Game game){
    try {
        auto pid=GetCurrentProcessId();
        auto name=L"Local\\FGSession-"+std::to_wstring(pid);
        fgwarp::Handle mapping{CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(SessionState),name.c_str())};
        if(!mapping.value || GetLastError()==ERROR_ALREADY_EXISTS)throw Error("Create session mapping",GetLastError());
        MappingView v{static_cast<SessionState*>(MapViewOfFile(mapping.value,FILE_MAP_ALL_ACCESS,0,0,sizeof(SessionState)))};
        if(!v.data)throw Error("Map session settings",GetLastError());
        auto& s=*v.data;s.game=game.window;s.pid=game.pid;s.warp=fgwarp::requested.load();CopySettings(s);
        wchar_t file[32768]{};if(!GetModuleFileNameW(nullptr,file,32768))throw Error("Find FG executable",GetLastError());
        auto cmd=L"\""+std::wstring(file)+L"\" --fg-session "+std::to_wstring(pid);
        STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION child{};
        if(!CreateProcessW(file,cmd.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&startup,&child))throw Error("Start capture process",GetLastError());
        CloseHandle(child.hThread);fgwarp::Handle proc{child.hProcess};
        ULONGLONG stopping=0;
        while(WaitForSingleObject(proc.value,15)==WAIT_TIMEOUT){
            CopySettings(s);
            if(g_userStopRequested.load()){
                InterlockedExchange(&s.stop,1);
                if(!stopping)stopping=GetTickCount64();
                if(GetTickCount64()-stopping>4000)TerminateProcess(proc.value,ERROR_CANCELLED);
            }
        }
        DWORD code=0;GetExitCodeProcess(proc.value,&code);
        if(!g_userStopRequested.load()){
            if(*s.error)Status(s.error);
            else if(code){wchar_t msg[160];swprintf(msg,160,L"FG error: Capture session exited (0x%08lX). Stage: %ls",code,s.stage);Status(msg);}
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
    session=v.data;fgwarp::stageCallback=[](const wchar_t* stage){lstrcpynW(session->stage,stage,96);};fgwarp::requested.store(session->warp!=0);
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
