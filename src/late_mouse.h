#pragma once
#include <windows.h>
#include <thread>
#include <mutex>
#include <array>
#include <stdexcept>
#include "late_input.h"
class LateMouse {
    fglate::History history;
    std::mutex mutex;
    std::thread worker;
    HANDLE ready{},stop{};
    DWORD gamePid{};
    bool initialized{};
    static LRESULT CALLBACK Proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
        if(msg==WM_NCCREATE){
            auto* cs=reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        }
        auto* self=reinterpret_cast<LateMouse*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if(msg==WM_INPUT&&self){
            RAWINPUT in{};UINT size=sizeof(in);
            if(GetRawInputData(reinterpret_cast<HRAWINPUT>(lp),RID_INPUT,&in,&size,sizeof(RAWINPUTHEADER))==sizeof(in)&&
                in.header.dwType==RIM_TYPEMOUSE&&!(in.data.mouse.usFlags&MOUSE_MOVE_ABSOLUTE)){
                DWORD pid=0;GetWindowThreadProcessId(GetForegroundWindow(),&pid);
                if(pid==self->gamePid&&(in.data.mouse.lLastX||in.data.mouse.lLastY)){
                    LARGE_INTEGER q{};QueryPerformanceCounter(&q);
                    std::lock_guard<std::mutex> guard(self->mutex);
                    self->history.push(q.QuadPart,in.data.mouse.lLastX,in.data.mouse.lLastY);
                }
            }
        }
        return DefWindowProcW(hwnd,msg,wp,lp);
    }
    void run(){
        SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_ABOVE_NORMAL);
        WNDCLASSW cls{};cls.lpfnWndProc=Proc;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"FGLateMousePro";
        RegisterClassW(&cls);
        HWND window=CreateWindowExW(0,cls.lpszClassName,L"",0,0,0,0,0,HWND_MESSAGE,nullptr,cls.hInstance,this);
        if(window){
            RAWINPUTDEVICE mouse{1,2,RIDEV_INPUTSINK,window};
            initialized=RegisterRawInputDevices(&mouse,1,sizeof(mouse))!=FALSE;
        }
        SetEvent(ready);
        if(initialized){
            while(MsgWaitForMultipleObjectsEx(1,&stop,INFINITE,QS_ALLINPUT,MWMO_INPUTAVAILABLE)==WAIT_OBJECT_0+1){
                MSG msg{};for(unsigned i=0;i<128&&PeekMessageW(&msg,nullptr,0,0,PM_REMOVE);++i)DispatchMessageW(&msg);
            }
            RAWINPUTDEVICE remove{1,2,RIDEV_REMOVE,nullptr};RegisterRawInputDevices(&remove,1,sizeof(remove));
        }
        if(window)DestroyWindow(window);
    }
    void close() noexcept {
        if(stop)SetEvent(stop);
        if(worker.joinable())worker.join();
        if(stop)CloseHandle(stop);
        if(ready)CloseHandle(ready);
        stop=ready=nullptr;
    }
public:
    struct Pose {float yaw{},pitch{};double x{},y{};bool valid{};};
    static constexpr double rad=-.022*1.5*3.14159265358979323846/180.;
    using Set=std::array<fglate::Correction,3>;
    explicit LateMouse(DWORD pid):gamePid(pid){
        ready=CreateEventW(nullptr,TRUE,FALSE,nullptr);stop=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if(!ready||!stop){close();throw std::runtime_error("Create late input events");}
        try{worker=std::thread([this]{run();});}catch(...){close();throw;}
        if(WaitForSingleObject(ready,5000)!=WAIT_OBJECT_0||!initialized){close();throw std::runtime_error("Register dedicated Raw Input");}
    }
    ~LateMouse(){close();}
    void reset(int64_t t){std::lock_guard<std::mutex> guard(mutex);history.reset(t);}
    Pose sourcePose(int64_t prev,int64_t cur,double horizon,double ticksPerMs){
        std::lock_guard<std::mutex> guard(mutex);
        const int64_t offset=static_cast<int64_t>(5.*ticksPerMs);
        fglate::Counts c{},p{};
        if(!history.at(cur-offset,c))return {};
        if(horizon>0&&history.at(prev-offset,p)){c.x+=horizon*(c.x-p.x);c.y+=horizon*(c.y-p.y);}
        return {float(std::remainder(c.x*rad,6.283185307179586)),float(std::remainder(c.y*rad,6.283185307179586)),c.x,c.y,true};
    }
    fglate::Angles targetDelta(const Pose& pose,double ticksPerMs,double lead){
        if(!pose.valid)return {};
        std::lock_guard<std::mutex> guard(mutex);
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const auto latest=now.QuadPart;
        fglate::Counts c{};
        if(!history.at(latest,c))return {};
        auto p=history.predict(latest,ticksPerMs,lead);
        const double x=(c.x+p.x-pose.x)*rad,y=(c.y+p.y-pose.y)*rad;
        const double scale=std::max(1.,std::hypot(x,y)/.06981317007977318);
        return {float(x/scale),float(y/scale)};
    }
    Set sample(int64_t prev,int64_t cur,int64_t latest,double horizon,double ticksPerMs,double lead){
        std::unique_lock<std::mutex> guard(mutex,std::try_to_lock);
        if(!guard.owns_lock())return {};
        Set result{};constexpr double offsets[3]{5.,2.,8.};
        for(size_t i=0;i<result.size();++i){
            const int64_t offset=static_cast<int64_t>(ticksPerMs*offsets[i]);
            result[i]=history.correction(prev-offset,cur-offset,latest,horizon,ticksPerMs,lead);
        }
        return result;
    }
};
