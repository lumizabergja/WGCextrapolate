// V15 Clean v16 with DXGI Desktop Duplication capture.
#ifndef UNICODE
#define UNICODE
#endif
#define _UNICODE
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>
#include <array>
#include <algorithm>
#include <utility>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <exception>
#include <cmath>
#include <cwchar>
#include <dwmapi.h>
#include <uxtheme.h>
#include <commctrl.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <nvOpticalFlowD3D12.h>

using Microsoft::WRL::ComPtr;
namespace {
constexpr int kNumCapture = 6;
constexpr int kOpticalFlowGrid = 4; // This app has a fixed NvOFA FAST 4x4 path.
constexpr UINT kIconId = 101;
constexpr UINT kEmergencyHotkey = 0x5240;
constexpr int kTarget = 101, kRefresh = 102, kMultiplier = 103, kStart = 104, kStop = 105,
    kDynamicTarget = 107, kCenterRadius = 108, kCenterFeather = 109, kPrimaryProbe = 110;
constexpr UINT WM_FG_STATUS = WM_APP+77, WM_FG_STOPPED = WM_APP+78;
HWND g_control{},g_targets{},g_multi{},g_start{},g_stop{},g_overlay{},g_dynamicTarget{};
HWND g_centerRadiusEdit{},g_centerFeatherEdit{};
HWND g_refresh{},g_primaryProbe{};
HFONT g_font{},g_fontBold{},g_fontSmall{},g_fontHero{};
HBRUSH g_bgBrush{},g_fieldBrush{},g_panelBrush{};
const COLORREF kBackground=RGB(10,14,22),kPanel=RGB(20,27,38),kField=RGB(26,36,49),kText=RGB(233,244,250),kMuted=RGB(145,160,173),kAccent=RGB(61,226,220);
std::atomic<bool> g_running{false};
// Separate user-requested Stop from a transient invalidated capture session.
// The latter ends one GPU session before the supervisor restarts it.
std::atomic<bool> g_userStopRequested{false};
// The presenter may temporarily stop during DXGI capture recovery. Only the outer
// Engine() completion marks the launcher as ready for a fresh Start.
std::atomic<bool> g_engineFinished{true};
std::atomic<bool> g_lastGameExited{false};
constexpr UINT_PTR kLauncherLifecycleTimer = 0xF615;
// 0=Dynamic (target refresh), 1=Off, 2..6=static x2..x6.
std::atomic<int> g_fgMode{2};
std::atomic<int> g_targetHz{320};
// Center-only protection; zero radius and zero feather disable protection.
std::atomic<unsigned> g_centerRadiusPx{0},g_centerFeatherPx{0};
std::thread g_worker;
std::mutex g_statusLock;
std::wstring g_statusText;
struct Game {HWND window;DWORD pid;std::wstring name;};
std::vector<Game> g_games;

void Status(const std::wstring& s) {
    {std::lock_guard<std::mutex> l(g_statusLock);g_statusText=s;}
    if(g_control) PostMessageW(g_control,WM_FG_STATUS,0,0);
}
std::runtime_error Error(const std::string& what,HRESULT hr=0) {
    char b[128];std::snprintf(b,sizeof(b),"%s (0x%08lX)",what.c_str(),(unsigned long)hr);
    return std::runtime_error(b);
}
void Check(HRESULT hr,const char* what) {if(FAILED(hr)) throw Error(what,hr);}
void NvCheck(NV_OF_STATUS s,const char* what) {if(s!=NV_OF_SUCCESS)throw Error(what,static_cast<HRESULT>(s));}
struct DisplayChanged {};
void CheckDuplication(HRESULT hr,const char* operation){
    if(hr==DXGI_ERROR_ACCESS_LOST || hr==DXGI_ERROR_SESSION_DISCONNECTED ||
       hr==DXGI_ERROR_MODE_CHANGE_IN_PROGRESS)throw DisplayChanged{};
    Check(hr,operation);
}
std::wstring Wide(const std::string& s) {int n=MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,nullptr,0);if(n<1)return L"Error";std::wstring w(size_t(n),L'\0');MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,w.data(),n);w.pop_back();return w;}
BOOL CALLBACK FindWindows(HWND h,LPARAM) {
    if(!IsWindowVisible(h)||h==g_control||h==g_overlay||GetWindow(h,GW_OWNER))return TRUE;
    wchar_t title[256]{};GetWindowTextW(h,title,256);if(!title[0])return TRUE;
    DWORD pid=0;GetWindowThreadProcessId(h,&pid);if(!pid||pid==GetCurrentProcessId())return TRUE;
    RECT r{};GetWindowRect(h,&r);if(r.right-r.left<320||r.bottom-r.top<200)return TRUE;
    g_games.push_back({h,pid,title});return TRUE;
}
void RefreshGames(){
    SendMessageW(g_targets,CB_RESETCONTENT,0,0);g_games.clear();EnumWindows(FindWindows,0);
    for(const auto& g:g_games)SendMessageW(g_targets,CB_ADDSTRING,0,(LPARAM)g.name.c_str());
    if(!g_games.empty())SendMessageW(g_targets,CB_SETCURSEL,0,0);
}

struct OverlayWindow {
    HWND hwnd{};
    UINT width{},height{};
    LONG originX{},originY{};
    bool visible{};
    // Track mode changes when an exclusive 4:3 game regains focus.
    // The HWND must occupy the current logical monitor (not the old desktop mode).
    bool SyncToMonitor(HWND target) {
        MONITORINFO mi{sizeof(mi)};
        if(!GetMonitorInfoW(MonitorFromWindow(target,MONITOR_DEFAULTTONEAREST),&mi))
            throw Error("GetMonitorInfo during mode switch failed",GetLastError());
        const RECT r=mi.rcMonitor;
        const UINT w=UINT(r.right-r.left),h=UINT(r.bottom-r.top);
        if(w<320||h<200)throw Error("Invalid output display mode");
        if(w==width&&h==height&&r.left==originX&&r.top==originY)return false;
        if(!SetWindowPos(hwnd,HWND_TOPMOST,r.left,r.top,int(w),int(h),SWP_NOACTIVATE))
            throw Error("Resize click-through presenter failed",GetLastError());
        width=w;height=h;originX=r.left;originY=r.top;
        return true;
    }
    void SetVisible(bool active) {
        if(!hwnd || active==visible) return;
        // Never activate or focus our click-through presenter on Alt+Tab.
        ShowWindow(hwnd,active?SW_SHOWNOACTIVATE:SW_HIDE);
        if(active) SetWindowPos(hwnd,HWND_TOPMOST,0,0,0,0,
            SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW);
        visible=active;
    }
    void Init(HWND target){
        // Fullscreen non-layered topmost HWNDs cannot reliably pass menu clicks
        // to CS2. Use a permanently layered, nonactivating click-through HWND.
        // The source game stays in control of keyboard/mouse input.
        HMONITOR monitor=MonitorFromWindow(target,MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{sizeof(mi)};
        if(!GetMonitorInfoW(monitor,&mi))throw Error("GetMonitorInfo failed",GetLastError());
        RECT r=mi.rcMonitor;width=r.right-r.left;height=r.bottom-r.top;
        originX=r.left;originY=r.top;
        const DWORD normalEx=WS_EX_TOPMOST|WS_EX_NOACTIVATE|WS_EX_TRANSPARENT|WS_EX_LAYERED|WS_EX_TOOLWINDOW;
        hwnd=CreateWindowExW(normalEx,
            L"FGDXGIOverlay",L"FG",WS_POPUP,r.left,r.top,width,height,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        if(!hwnd)throw Error("Create presenter window failed",GetLastError());
        // Full-opacity layered click-through overlay: input reaches CS2 underneath.
        if(!SetLayeredWindowAttributes(hwnd,0,255,LWA_ALPHA))
            throw Error("SetLayeredWindowAttributes failed",GetLastError());
        // Fail closed: without capture exclusion the presenter can recursively capture itself.
        if(!SetWindowDisplayAffinity(hwnd,0x00000011))throw Error("WDA_EXCLUDEFROMCAPTURE unsupported",GetLastError());
        // Do not show over the control window; engine reveals it only when game has focus.
        ShowWindow(hwnd,SW_HIDE);
        visible=false;
        g_overlay=hwnd;
    }
    ~OverlayWindow(){if(hwnd)DestroyWindow(hwnd);g_overlay=nullptr;}
};

#include "ultradirect.h"

// A user may Alt+Tab or minimize; neither is an exit. Check the original
// HWND/PID identity and, when permitted, the process termination signal.
struct TrackedGame {
    Game game;
    HANDLE process{};
    explicit TrackedGame(Game g):game(std::move(g)) {
        process=OpenProcess(SYNCHRONIZE,FALSE,game.pid);
    }
    ~TrackedGame(){if(process)CloseHandle(process);}
    bool Exited() const {
        if(process && WaitForSingleObject(process,0)==WAIT_OBJECT_0)return true;
        DWORD owner=0;
        return !IsWindow(game.window)||
            GetWindowThreadProcessId(game.window,&owner)==0||owner!=game.pid;
    }
};

enum class SessionOutcome { Stop, Recreate };

// One session owns ALL DXGI capture, D3D11, and NvOFA resources. Returning from
// this function destroys them in reverse lifetime order before another attempt.
SessionOutcome EngineSession(const Game& game,const TrackedGame& tracked){
    try {
        if(tracked.Exited())return SessionOutcome::Stop;
        // D3D11 resources are created on this single render thread.
        OverlayWindow overlay;overlay.Init(game.window);
        RECT rc{};GetClientRect(game.window,&rc);
        const UINT gameW=UINT(rc.right-rc.left),gameH=UINT(rc.bottom-rc.top);
        MONITORINFOEXW monitorInfo{};monitorInfo.cbSize=sizeof(monitorInfo);
        UINT monitorHz=320;
        if(GetMonitorInfoW(MonitorFromWindow(game.window,MONITOR_DEFAULTTONEAREST),&monitorInfo)){
            DEVMODEW dm{};dm.dmSize=sizeof(dm);
            if(EnumDisplaySettingsW(monitorInfo.szDevice,ENUM_CURRENT_SETTINGS,&dm)&&dm.dmDisplayFrequency>=30)
                monitorHz=dm.dmDisplayFrequency;
        }
        // Duplicate output pixels inside the selected client rectangle.
        const UINT sourceW=gameW,sourceH=gameH;
        if(sourceW<16||sourceH<16||sourceW>8192||sourceH>8192)
            throw Error("Invalid game native capture resolution");
        DxDevices dx;dx.Init(overlay.hwnd,sourceW,sourceH,overlay.width,overlay.height);
        Status(L"Running");

        constexpr int ofGrid=kOpticalFlowGrid;
        Flow11 flow;flow.Init(dx,ofGrid);
        CaptureMailbox mailbox(dx,game.window,game.pid);
        // Anchor predictions to the DXGI desktop update timestamp. During
        // that train, only make zero-timeout duplication calls.

        LARGE_INTEGER freq{};
        QueryPerformanceFrequency(&freq);
        LARGE_INTEGER previousCaptureAt{};
        double realIntervalMs=16.667;
        int current=-1,previous=-1;
        bool flowReady=false;
        bool realFramePending=false;
        bool flowPending=false;
        DWORD captureWaitMs=4;
        bool wasFocused=false;
        int resumeWarmup=0;

        unsigned nextPrediction=1;
        unsigned maxPredictions=0;
        int predictionMode=2;
        int predictionTargetHz=320;
        double predictionRealIntervalMs=16.667;
        // Only the V15 render thread touches the D3D11 immediate context,
        // DXGI duplication, NvOFA surfaces and swapchain. No second thread
        // can overwrite a texture while it is being sampled or copied.
        while(g_running.load(std::memory_order_acquire)){
            MSG message{};
            while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){
                TranslateMessage(&message);DispatchMessageW(&message);
            }
            if(tracked.Exited()){
                g_running.store(false,std::memory_order_release);
                break;
            }
            DWORD foregroundPid=0;
            HWND foreground=GetForegroundWindow();
            if(foreground)GetWindowThreadProcessId(foreground,&foregroundPid);
            const bool focused=!IsIconic(game.window)&&foregroundPid==game.pid;
            if(focused && overlay.SyncToMonitor(game.window))throw DisplayChanged{};
            if(!focused){
                mailbox.SetActive(false);
                overlay.SetVisible(false);
                wasFocused=false;
                flowReady=false;
                realFramePending=false;
                flowPending=false;
                nextPrediction=maxPredictions+1;
                Sleep(8);
                continue;
            }
            if(!wasFocused){
                wasFocused=true;
                mailbox.SetActive(true);
                resumeWarmup=2;
                current=-1;previous=-1;previousCaptureAt={};
                realIntervalMs=16.667;
                flowReady=false;
                realFramePending=false;
                flowPending=false;
                nextPrediction=maxPredictions+1;
                captureWaitMs=4;
            }

            // Always poll DXGI capture before a queued prediction. New real
            // frames supersede the old prediction train, as in V35's mailbox.
            LARGE_INTEGER captureTime{};
            int next=mailbox.Take(captureTime);
            captureWaitMs=4;
            if(next>=0){
                previous=current;
                current=next;
                if(previousCaptureAt.QuadPart>0 && captureTime.QuadPart>previousCaptureAt.QuadPart && freq.QuadPart>0){
                    const double measured=1000.0*double(captureTime.QuadPart-previousCaptureAt.QuadPart)/double(freq.QuadPart);
                    if(measured>=1.0 && measured<=80.0)
                        realIntervalMs=std::clamp(realIntervalMs*0.75+measured*0.25,1.0,50.0);
                }
                previousCaptureAt=captureTime;
                flowReady=false;
                realFramePending=true;
                flowPending=(previous>=0 && resumeWarmup==0);
                if(resumeWarmup>0)--resumeWarmup;
                nextPrediction=maxPredictions+1;
            }
            if(current<0){Sleep(1);continue;}

            if(realFramePending){
                // Non-blocking wait: the separate producer in V35 can keep
                // capturing during its 250ms wait. In single-thread V15 a
                // blocking wait prevents new captures, making the image freeze.
                const DWORD ready=WaitForSingleObject(dx.frameReady,0);
                if(ready==WAIT_TIMEOUT){captureWaitMs=1;SwitchToThread();continue;}
                if(ready!=WAIT_OBJECT_0)throw Error("DXGI frame latency wait failed",GetLastError());

                dx.PresentReal(UINT(current),ofGrid);
                overlay.SetVisible(true);
                realFramePending=false;
                const int mode=std::clamp(g_fgMode.load(std::memory_order_relaxed),0,6);
                const int configuredTarget=g_targetHz.load(std::memory_order_relaxed);
                const int target=std::clamp(configuredTarget==0?int(monitorHz):configuredTarget,30,1000);
                predictionMode=mode;
                predictionTargetHz=target;
                predictionRealIntervalMs=realIntervalMs;
                maxPredictions=mode==0?5u:(mode>1?unsigned(mode-1):0u);
                // Allow 5% timing jitter when native updates already meet the target.
                if(mode==0 && predictionRealIntervalMs<=1.05*1000.0/target)
                    maxPredictions=0;
                nextPrediction=1;
                // Avoid optical flow work when the entire prediction window has expired.
                LARGE_INTEGER afterPresent{};
                QueryPerformanceCounter(&afterPresent);
                const double ageMs=1000.0 * double(afterPresent.QuadPart-previousCaptureAt.QuadPart) / double(freq.QuadPart);
                if(flowPending && maxPredictions>0 && ageMs<predictionRealIntervalMs){
                    flowReady=flow.Calculate(current,previous);
                    flowPending=!flowReady;
                }else flowPending=false;
                continue;
            }
            if(flowPending && maxPredictions>0){
                LARGE_INTEGER t{};QueryPerformanceCounter(&t);
                const double age=1000.0*double(t.QuadPart-previousCaptureAt.QuadPart)/double(freq.QuadPart);
                if(age>=predictionRealIntervalMs)flowPending=false;
                else if(flow.Calculate(current,previous)){flowReady=true;flowPending=false;}
            }
            if(!flowReady || nextPrediction>maxPredictions){
                captureWaitMs=4;
                if(flowPending)SwitchToThread();else Sleep(1);
                continue;
            }
            const double slotMs=predictionMode==0 ? 1000.0/predictionTargetHz
                                                   : predictionRealIntervalMs/predictionMode;
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            const double elapsedMs=1000.0 * double(now.QuadPart-previousCaptureAt.QuadPart) / double(freq.QuadPart);
            if(elapsedMs>=predictionRealIntervalMs ||
               slotMs*nextPrediction>=predictionRealIntervalMs){
                nextPrediction=maxPredictions+1;
                continue;
            }
            if(elapsedMs<slotMs*nextPrediction){
                SwitchToThread();
                continue;
            }
            // Submit only the most recent due slot after a delayed GPU or CPU
            // wakeup. Back-to-back catch-up presents can replace each other.
            unsigned dueSlot=std::min(maxPredictions,unsigned(elapsedMs/slotMs));
            nextPrediction=std::max(nextPrediction,dueSlot);
            const double predictionMs=nextPrediction*slotMs;
            // A successful wait consumes a presentation slot. Once acquired,
            // always submit this frame before polling the handle again.
            if(!DxDevices::Done(dx.ofFence.Get(),dx.ofValue)){SwitchToThread();continue;}
            const DWORD ready=WaitForSingleObject(dx.frameReady,0);
            if(ready==WAIT_TIMEOUT){captureWaitMs=1;SwitchToThread();continue;}
            if(ready!=WAIT_OBJECT_0)throw Error("DXGI frame latency wait failed",GetLastError());

            const float fraction=float(predictionMs/predictionRealIntervalMs);
            dx.Draw(UINT(current),UINT(previous>=0?previous:current),
                    flow.motionSRV,fraction,ofGrid);
            ++nextPrediction;
            captureWaitMs=(nextPrediction<=maxPredictions)?0:4;
        }
        mailbox.Stop();
        overlay.SetVisible(false);
        Status(L"Stopped");
    }catch(const DisplayChanged&){
        return SessionOutcome::Recreate;
    }catch(const std::exception& e){
        if(!tracked.Exited() && !g_userStopRequested.load())
            Status(L"FG error: "+Wide(e.what()));
        else Status(L"Stopped");
        g_running.store(false,std::memory_order_release);
    }
    return SessionOutcome::Stop;
}

void Engine(Game game){
    Status(L"Starting FG...");
    TrackedGame tracked(game);
    // After repeated invalidations in a short time, report failure rather
    // than spin/reinitialize indefinitely. Full teardown is mandatory.
    ULONGLONG windowStart=GetTickCount64();
    unsigned restartCount=0;
    while(!g_userStopRequested.load(std::memory_order_acquire) && !tracked.Exited()){
        g_running.store(true,std::memory_order_release);
        const SessionOutcome outcome=EngineSession(game,tracked);
        if(outcome!=SessionOutcome::Recreate || tracked.Exited() ||
           g_userStopRequested.load(std::memory_order_acquire))break;
        const ULONGLONG now=GetTickCount64();
        if(now-windowStart>10000){windowStart=now;restartCount=0;}
        if(++restartCount>3){
            Status(L"Source error: DXGI capture repeatedly invalidated. "
                   L"Full capture restart failed; check the display mode and driver.");
            break;
        }
        // Allow transient display-mode/topology changes to settle before
        // building a NEW D3D11 device, shared textures and NvOFA state.
        for(unsigned i=0;i<10&&!g_userStopRequested.load()&&!tracked.Exited();++i)
            Sleep(10);
    }
    g_running.store(false,std::memory_order_release);
    g_lastGameExited.store(tracked.Exited(),std::memory_order_release);
    // Publish completion only AFTER all sessions and capture threads have
    // returned; never treat a transient DXGI capture recreation as a full stop.
    g_engineFinished.store(true,std::memory_order_release);
    if(g_control)PostMessageW(g_control,WM_FG_STOPPED,tracked.Exited()?1u:0u,0);
}


void InitUiStyle(HWND hwnd){
    if(!g_bgBrush)g_bgBrush=CreateSolidBrush(kBackground);
    if(!g_panelBrush)g_panelBrush=CreateSolidBrush(kPanel);
    if(!g_fieldBrush)g_fieldBrush=CreateSolidBrush(kField);
    if(!g_font)g_font=CreateFontW(-17,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    if(!g_fontBold)g_fontBold=CreateFontW(-18,0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI Semibold");
    if(!g_fontSmall)g_fontSmall=CreateFontW(-13,0,0,0,FW_MEDIUM,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    if(!g_fontHero)g_fontHero=CreateFontW(-32,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    const BOOL enabled=TRUE;
    DwmSetWindowAttribute(hwnd,static_cast<DWMWINDOWATTRIBUTE>(20),&enabled,sizeof(enabled));
    SetWindowTheme(hwnd,L"DarkMode_Explorer",nullptr);
}
void StyleControl(HWND control,bool bold=false,bool useSmallFont=false){
    if(!control)return;
    SendMessageW(control,WM_SETFONT,(WPARAM)(useSmallFont?g_fontSmall:(bold?g_fontBold:g_font)),TRUE);
    SetWindowTheme(control,L"DarkMode_Explorer",nullptr);
}
HWND UiLabel(HWND parent,const wchar_t* text,int x,int y,int w,int h,bool useSmallFont=false){
    HWND c=CreateWindowW(L"STATIC",text,WS_CHILD|WS_VISIBLE|SS_LEFT,x,y,w,h,parent,nullptr,nullptr,nullptr);
    StyleControl(c,false,useSmallFont);
    return c;
}
void PaintRoundBox(HDC dc,int l,int t,int r,int b,COLORREF bg,COLORREF border,int radius=20){
    HPEN pen=CreatePen(PS_SOLID,1,border);
    HBRUSH brush=CreateSolidBrush(bg);
    HGDIOBJ op=SelectObject(dc,pen),ob=SelectObject(dc,brush);
    RoundRect(dc,l,t,r,b,radius,radius);
    SelectObject(dc,op);SelectObject(dc,ob);
    DeleteObject(pen);DeleteObject(brush);
}
void DrawUi(HWND hwnd,HDC dc){
    RECT cr{};GetClientRect(hwnd,&cr);
    FillRect(dc,&cr,g_bgBrush);
    // Branded header: subtle divider and cyan accent.
    RECT top{24,79,cr.right-24,81};HBRUSH line=CreateSolidBrush(RGB(45,62,75));
    FillRect(dc,&top,line);DeleteObject(line);
    RECT accent{24,79,124,81};HBRUSH cyan=CreateSolidBrush(kAccent);
    FillRect(dc,&accent,cyan);DeleteObject(cyan);
    PaintRoundBox(dc,20,96,cr.right-20,199,kPanel,RGB(40,58,70));
    PaintRoundBox(dc,20,210,cr.right-20,306,kPanel,RGB(40,58,70));
    PaintRoundBox(dc,20,316,cr.right-20,390,kPanel,RGB(40,58,70));
    PaintRoundBox(dc,20,397,cr.right-20,476,kPanel,RGB(40,58,70));
    HICON icon=LoadIconW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(kIconId));
    if(icon)DrawIconEx(dc,24,18,icon,52,52,0,nullptr,DI_NORMAL);
    SetBkMode(dc,TRANSPARENT);
    HFONT original=(HFONT)SelectObject(dc,g_fontHero);
    SetTextColor(dc,kText);TextOutW(dc,86,19,L"FG",2);
    SelectObject(dc,g_fontSmall);
    SetTextColor(dc,kMuted);
    const wchar_t* sub=L"FRAME GENERATION  /  LOW LATENCY";
    TextOutW(dc,87,58,sub,(int)wcslen(sub));
    SelectObject(dc,original);
}
void DrawUiButton(const DRAWITEMSTRUCT* ds){
    HDC dc=ds->hDC;RECT r=ds->rcItem;
    bool enabled=(ds->itemState&ODS_DISABLED)==0;
    bool hover=(ds->itemState&ODS_SELECTED)!=0;
    bool isStart=ds->CtlID==kStart;
    COLORREF fill=isStart?(enabled?(hover?RGB(37,174,171):RGB(41,209,203)):RGB(39,88,91)):
                 (hover?RGB(51,68,85):RGB(38,52,66));
    PaintRoundBox(dc,r.left+1,r.top+1,r.right-1,r.bottom-1,fill,isStart?RGB(65,233,230):RGB(70,89,105),12);
    wchar_t caption[80]{};GetWindowTextW(ds->hwndItem,caption,80);
    SetBkMode(dc,TRANSPARENT);
    SetTextColor(dc,isStart?RGB(5,18,25):(enabled?kText:kMuted));
    HFONT old=(HFONT)SelectObject(dc,g_fontBold);
    DrawTextW(dc,caption,-1,&r,DT_SINGLELINE|DT_CENTER|DT_VCENTER);
    SelectObject(dc,old);
}
void DrawUiCombo(const DRAWITEMSTRUCT* ds){
    if(ds->itemID==UINT(-1)&& (ds->itemState&ODS_COMBOBOXEDIT)==0)return;
    const bool selected=(ds->itemState&ODS_SELECTED)!=0;
    COLORREF background=selected?RGB(30,92,104):kField;
    HBRUSH brush=CreateSolidBrush(background);
    FillRect(ds->hDC,&ds->rcItem,brush);DeleteObject(brush);
    int id=ds->itemID==UINT(-1)?int(SendMessageW(ds->hwndItem,CB_GETCURSEL,0,0)):int(ds->itemID);
    if(id<0)return;
    wchar_t text[260]{};
    SendMessageW(ds->hwndItem,CB_GETLBTEXT,id,(LPARAM)text);
    RECT rc=ds->rcItem;rc.left+=12;
    SetBkMode(ds->hDC,TRANSPARENT);
    SetTextColor(ds->hDC,kText);
    HFONT old=(HFONT)SelectObject(ds->hDC,g_font);
    DrawTextW(ds->hDC,text,-1,&rc,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);
    SelectObject(ds->hDC,old);
}

LRESULT CALLBACK OverlayProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
    if(msg==WM_NCHITTEST)return HTTRANSPARENT;
    if(msg==WM_MOUSEACTIVATE)return MA_NOACTIVATE;
    return DefWindowProcW(hwnd,msg,wp,lp);
}
LRESULT CALLBACK ControlProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
    switch(msg){
    case WM_CREATE:{
        InitUiStyle(hwnd);
        UiLabel(hwnd,L"SOURCE",36,113,90,21,true);
        g_targets=CreateWindowW(L"COMBOBOX",nullptr,WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|CBS_OWNERDRAWFIXED|CBS_HASSTRINGS|WS_VSCROLL,
            36,136,335,190,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTarget)),nullptr,nullptr);
        StyleControl(g_targets);
        g_refresh=CreateWindowW(L"BUTTON",L"Refresh",WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,
            386,135,140,31,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRefresh)),nullptr,nullptr);
        StyleControl(g_refresh);
        UiLabel(hwnd,L"FRAME GENERATION",36,176,180,18,true);
        UiLabel(hwnd,L"UltraDirect V2",285,176,240,18,true);
        g_multi=CreateWindowW(L"COMBOBOX",nullptr,WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|CBS_OWNERDRAWFIXED|CBS_HASSTRINGS|WS_VSCROLL,
            36,229,160,210,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(kMultiplier)),nullptr,nullptr);
        StyleControl(g_multi);
        for(const wchar_t* mode:{L"Dynamic",L"Off",L"x2",L"x3",L"x4",L"x5",L"x6"})
            SendMessageW(g_multi,CB_ADDSTRING,0,(LPARAM)mode);
        SendMessageW(g_multi,CB_SETCURSEL,0,0);
        UiLabel(hwnd,L"Target FPS",217,225,95,20,true);
        g_dynamicTarget=CreateWindowW(L"EDIT",L"320",WS_CHILD|WS_VISIBLE|ES_NUMBER|ES_AUTOHSCROLL,
            218,247,100,27,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDynamicTarget)),nullptr,nullptr);
        StyleControl(g_dynamicTarget);
        SendMessageW(g_dynamicTarget,EM_SETLIMITTEXT,4,0);
        g_start=CreateWindowW(L"BUTTON",L"Start",WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,
            36,331,233,42,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStart)),nullptr,nullptr);
        g_stop=CreateWindowW(L"BUTTON",L"Stop",WS_CHILD|WS_VISIBLE|WS_DISABLED|BS_OWNERDRAW,
            282,331,244,42,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStop)),nullptr,nullptr);
        StyleControl(g_start,true);StyleControl(g_stop,true);
        UiLabel(hwnd,L"CENTER RADIUS (px)",36,407,200,19,true);
        g_centerRadiusEdit=CreateWindowW(L"EDIT",L"0",WS_CHILD|WS_VISIBLE|ES_NUMBER|ES_AUTOHSCROLL,
            36,429,200,30,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCenterRadius)),nullptr,nullptr);
        StyleControl(g_centerRadiusEdit);
        SendMessageW(g_centerRadiusEdit,EM_SETLIMITTEXT,3,0);
        UiLabel(hwnd,L"CENTER FEATHER (px)",285,407,230,19,true);
        g_centerFeatherEdit=CreateWindowW(L"EDIT",L"0",WS_CHILD|WS_VISIBLE|ES_NUMBER|ES_AUTOHSCROLL,
            285,429,200,30,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCenterFeather)),nullptr,nullptr);
        StyleControl(g_centerFeatherEdit);
        SendMessageW(g_centerFeatherEdit,EM_SETLIMITTEXT,3,0);
        // The timer is a fallback for a missed/late worker completion message.
        // It never enables Start while DXGI capture is still restarting or tearing down.
        SetTimer(hwnd,kLauncherLifecycleTimer,100,nullptr);
        RefreshGames();return 0;
    }
    case WM_DRAWITEM:{
        const auto* d=reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
        if(d && d->CtlType==ODT_BUTTON){DrawUiButton(d);return TRUE;}
        if(d && d->CtlType==ODT_COMBOBOX){DrawUiCombo(d);return TRUE;}
        break;
    }
    case WM_MEASUREITEM:{
        auto* m=reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
        if(m && m->CtlType==ODT_COMBOBOX){m->itemHeight=25;return TRUE;}
        break;
    }
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORSTATIC:{
        HDC dc=(HDC)wp;SetBkMode(dc,TRANSPARENT);SetTextColor(dc,kMuted);
        SetBkColor(dc,kBackground);
        // Labels are painted over the matching panel color.
        return (LRESULT)g_panelBrush;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:{
        HDC dc=(HDC)wp;SetTextColor(dc,kText);SetBkColor(dc,kField);
        return (LRESULT)g_fieldBrush;
    }
    case WM_ERASEBKGND:return TRUE;
    case WM_PAINT:{
        PAINTSTRUCT ps{};HDC dc=BeginPaint(hwnd,&ps);
        DrawUi(hwnd,dc);EndPaint(hwnd,&ps);return 0;
    }
    case WM_TIMER:
        if(wp==kLauncherLifecycleTimer &&
           g_engineFinished.load(std::memory_order_acquire) &&
           !IsWindowEnabled(g_start)){
            // Engine owns GPU teardown; only the GUI joins after completion.
            if(g_worker.joinable())g_worker.join();
            EnableWindow(g_start,TRUE);
            EnableWindow(g_stop,FALSE);
            if(g_lastGameExited.exchange(false,std::memory_order_acq_rel))RefreshGames();
        }
        return 0;
    case WM_HOTKEY:
        if(wp==kEmergencyHotkey){
            g_userStopRequested.store(true);
            g_running.store(false);
        }
        return 0;
    case WM_COMMAND:{
        switch(LOWORD(wp)){
        case kRefresh:if(!g_running.load())RefreshGames();break;
        case kMultiplier:
            if(HIWORD(wp)==CBN_SELCHANGE){
                const auto value=SendMessageW(g_multi,CB_GETCURSEL,0,0);
                if(value!=CB_ERR)g_fgMode.store(int(value));
            }
            break;
        case kCenterRadius:
            if(HIWORD(wp)==EN_CHANGE && g_centerRadiusEdit){
                wchar_t value[16]{};GetWindowTextW(g_centerRadiusEdit,value,16);
                g_centerRadiusPx.store(std::min(300ul,wcstoul(value,nullptr,10)),std::memory_order_relaxed);
            }
            break;
        case kCenterFeather:
            if(HIWORD(wp)==EN_CHANGE && g_centerFeatherEdit){
                wchar_t value[16]{};GetWindowTextW(g_centerFeatherEdit,value,16);
                g_centerFeatherPx.store(std::min(300ul,wcstoul(value,nullptr,10)),std::memory_order_relaxed);
            }
            break;
        case kDynamicTarget:
            if(HIWORD(wp)==EN_CHANGE){
                wchar_t value[16]{};GetWindowTextW(g_dynamicTarget,value,16);
                const unsigned hz=wcstoul(value,nullptr,10);
                if(hz>=30&&hz<=1000)g_targetHz.store(int(hz));
            }
            break;
        case kStart:{
            if(g_running.load()||!g_engineFinished.load(std::memory_order_acquire))break;
            auto selection=SendMessageW(g_targets,CB_GETCURSEL,0,0);
            auto validSelection=[&](LRESULT index){
                if(index==CB_ERR||index<0||size_t(index)>=g_games.size())return false;
                const Game& g=g_games[size_t(index)];
                DWORD owner=0;
                return IsWindow(g.window)&&
                    GetWindowThreadProcessId(g.window,&owner)!=0&&owner==g.pid;
            };
            // If CS2 has been closed and relaunched, the old HWND/PID is stale.
            // Refresh the chooser instead of starting a capture on a dead window.
            if(!validSelection(selection)){
                RefreshGames();
                selection=SendMessageW(g_targets,CB_GETCURSEL,0,0);
            }
            if(!validSelection(selection)){
                MessageBoxW(hwnd,L"Open the game and click Refresh before Start.",
                    L"FG",MB_ICONINFORMATION);break;
            }
            const Game game=g_games[size_t(selection)];
            auto chosen=SendMessageW(g_multi,CB_GETCURSEL,0,0);
            g_fgMode.store(chosen==CB_ERR?0:int(chosen));
            if(g_worker.joinable())g_worker.join();
            g_userStopRequested.store(false,std::memory_order_release);
            g_lastGameExited.store(false,std::memory_order_release);
            g_engineFinished.store(false,std::memory_order_release);
            g_running.store(true);EnableWindow(g_start,FALSE);EnableWindow(g_stop,TRUE);
            g_worker=std::thread([game]{Engine(game);});
            // Start gives focus back to the selected game. The presenter never activates.
            SetForegroundWindow(game.window);
            break;
        }
        case kStop:
            g_userStopRequested.store(true);
            g_running.store(false);EnableWindow(g_stop,FALSE);break;
        }
        return 0;
    }
    case WM_FG_STATUS:{
        std::wstring s;{std::lock_guard<std::mutex> l(g_statusLock);s=g_statusText;}
        // No Ready/Running/Stopped display. Keep actionable errors visible.
        if(s.rfind(L"FG error:",0)==0||s.rfind(L"Source error:",0)==0)
            MessageBoxW(hwnd,s.c_str(),L"FG error",MB_OK|MB_ICONERROR);
        
        return 0;
    }
    case WM_FG_STOPPED:
        // A session can temporarily stop for DXGI capture recovery. Only the final
        // outer Engine() completion is allowed to unlock another Start.
        if(!g_engineFinished.load(std::memory_order_acquire))return 0;
        if(g_worker.joinable())g_worker.join();
        EnableWindow(g_start,TRUE);EnableWindow(g_stop,FALSE);
        if(g_lastGameExited.exchange(false,std::memory_order_acq_rel))RefreshGames();
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd,kLauncherLifecycleTimer);
        g_userStopRequested.store(true);
        g_running.store(false);
        if(g_worker.joinable())g_worker.join();
        UnregisterHotKey(hwnd,kEmergencyHotkey);
        if(g_font)DeleteObject(g_font);if(g_fontBold)DeleteObject(g_fontBold);
        if(g_fontSmall)DeleteObject(g_fontSmall);if(g_fontHero)DeleteObject(g_fontHero);
        if(g_bgBrush)DeleteObject(g_bgBrush);if(g_panelBrush)DeleteObject(g_panelBrush);
        if(g_fieldBrush)DeleteObject(g_fieldBrush);
        PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
} // namespace
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int){
    // Obtain physical monitor/window dimensions rather than DPI-virtualized pixels.
    if(!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        SetProcessDPIAware();
    WNDCLASSW o{};o.hInstance=instance;o.lpfnWndProc=OverlayProc;
    o.lpszClassName=L"FGDXGIOverlay";o.hCursor=nullptr;
    o.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(kIconId));
    if(!RegisterClassW(&o))return 1;
    WNDCLASSW c{};c.hInstance=instance;c.lpfnWndProc=ControlProc;
    c.lpszClassName=L"FGDXGIControl";c.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    c.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(kIconId));
    c.hbrBackground=CreateSolidBrush(kBackground);
    if(!RegisterClassW(&c))return 1;
    g_control=CreateWindowW(c.lpszClassName,L"FG",WS_OVERLAPPEDWINDOW&~WS_MAXIMIZEBOX&~WS_THICKFRAME,
        CW_USEDEFAULT,CW_USEDEFAULT,570,540,nullptr,nullptr,instance,nullptr);
    if(!g_control)return 2;
    RegisterHotKey(g_control,kEmergencyHotkey,MOD_CONTROL|MOD_SHIFT,VK_F12);
    HICON appIcon=LoadIconW(instance,MAKEINTRESOURCEW(kIconId));
    if(appIcon){SendMessageW(g_control,WM_SETICON,ICON_BIG,(LPARAM)appIcon);
                SendMessageW(g_control,WM_SETICON,ICON_SMALL,(LPARAM)appIcon);}
    ShowWindow(g_control,SW_SHOW);UpdateWindow(g_control);
    MSG message{};while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}
    return int(message.wParam);
}
