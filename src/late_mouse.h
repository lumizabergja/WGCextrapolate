#pragma once
#include "late_input.h"
#include "pixel_types.h"
// Independent Raw Input receiver. Does not modify or inject game input.
class LateMouse {
    std::thread worker;
    std::mutex mutex;
    fglate::History history;
    HANDLE ready{};
    DWORD tid{};
    bool registered{},focused{};
    double ticksPerMs{};
    static int64_t stamp(){LARGE_INTEGER q{};QueryPerformanceCounter(&q);return q.QuadPart;}
    static LRESULT CALLBACK Proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
        if(msg==WM_NCCREATE){
            auto* cs=reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        }
        auto* self=reinterpret_cast<LateMouse*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if(msg==WM_INPUT&&self){
            RAWINPUT raw{};UINT n=sizeof(raw);
            const UINT got=GetRawInputData(reinterpret_cast<HRAWINPUT>(lp),RID_INPUT,&raw,&n,sizeof(RAWINPUTHEADER));
            if(got!=UINT(-1)&&raw.header.dwType==RIM_TYPEMOUSE&&!(raw.data.mouse.usFlags&MOUSE_MOVE_ABSOLUTE)){
                const auto q=stamp();
                std::lock_guard<std::mutex> guard(self->mutex);
                if(self->focused)self->history.push(q,raw.data.mouse.lLastX,raw.data.mouse.lLastY);
            }
        }
        return DefWindowProcW(hwnd,msg,wp,lp);
    }
    void run(){
        SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_ABOVE_NORMAL);
        tid=GetCurrentThreadId();
        WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=Proc;wc.lpszClassName=L"FGUltraLateMouse";
        HWND hwnd=nullptr;
        if(RegisterClassW(&wc)||GetLastError()==ERROR_CLASS_ALREADY_EXISTS)
            hwnd=CreateWindowExW(0,wc.lpszClassName,L"",0,0,0,0,0,HWND_MESSAGE,nullptr,wc.hInstance,this);
        if(hwnd){
            RAWINPUTDEVICE device{1,2,RIDEV_INPUTSINK,hwnd};
            registered=RegisterRawInputDevices(&device,1,sizeof(device))!=FALSE;
        }
        MSG msg{};PeekMessageW(&msg,nullptr,0,0,PM_NOREMOVE);
        SetEvent(ready);
        if(registered){
            while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}
            RAWINPUTDEVICE remove{1,2,RIDEV_REMOVE,nullptr};RegisterRawInputDevices(&remove,1,sizeof(remove));
        }
        if(hwnd)DestroyWindow(hwnd);
    }
    static CameraRotation rotation(double x,double y,double limit){
        const double scale=std::max(1.,std::hypot(x,y)/limit);x/=scale;y/=scale;
        return {float(std::sin(x)),float(std::cos(x)),float(std::sin(y)),float(std::cos(y))};
    }
public:
    LateMouse(){
        LARGE_INTEGER f{};QueryPerformanceFrequency(&f);ticksPerMs=double(f.QuadPart)/1000.;
        ready=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if(!ready)throw Error("Create raw input event",GetLastError());
        try{worker=std::thread([this]{run();});}catch(...){CloseHandle(ready);throw;}
        WaitForSingleObject(ready,INFINITE);CloseHandle(ready);ready=nullptr;
        if(!registered){worker.join();throw Error("Register raw mouse input");}
    }
    ~LateMouse(){if(worker.joinable()){PostThreadMessageW(tid,WM_QUIT,0,0);worker.join();}}
    void Focus(bool value){
        std::lock_guard<std::mutex> g(mutex);
        focused=value;history.reset(stamp());
    }
    InputPose Sample(int64_t previous,int64_t current,UINT w,UINT h,bool hud,bool gun){
        InputPose out{};
        out.hud=hud;out.gun=gun;
        std::lock_guard<std::mutex> g(mutex);
        const auto now=stamp();
        if(!focused||!current||now<current||double(now-current)>50.*ticksPerMs||!w||!h)return out;
        fglate::Counts c{},n{},p{};
        if(!history.at(current,c)||!history.at(now,n))return out;
        const bool pair=previous>0&&previous<current&&history.at(previous,p);
        const auto future=history.predict(now,ticksPerMs,5.);
        constexpr double rad=.022*1.5*3.14159265358979323846/180.;
        constexpr double limit=4.*3.14159265358979323846/180.;
        // CS2 unzoomed vertical FOV 73.74 degrees; normalized source aspect.
        out.fy=float(.5/std::tan(73.74*3.14159265358979323846/360.));
        out.fx=out.fy*float(h)/float(w);
        out.latest=rotation((n.x-c.x+future.x)*rad,(n.y-c.y+future.y)*rad,limit);
        if(pair){
            out.historical=rotation((c.x-p.x)*rad,(c.y-p.y)*rad,2.*limit);
            out.history=1;
        }
        out.active=1;
        return out;
    }
};
