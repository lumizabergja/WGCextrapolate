#pragma once
#include <windows.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <stdexcept>
#include "late_input.h"
#include "camera_fit.h"
namespace fgwarp {
inline int64_t stamp(){LARGE_INTEGER t{};QueryPerformanceCounter(&t);return t.QuadPart;}
class InputWorker {
    std::mutex lock;
    std::condition_variable startup;
    std::thread worker;
    fglate::History history;
    bool ready{},accept{};
    DWORD error{},tid{},gamePid{};
    static LRESULT CALLBACK Proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
        if(msg==WM_NCCREATE){
            auto* cs=reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        }
        auto* self=reinterpret_cast<InputWorker*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if(msg==WM_INPUT&&self){
            const auto at=stamp();RAWINPUT raw{};UINT size=sizeof(raw);
            if(GetRawInputData(reinterpret_cast<HRAWINPUT>(lp),RID_INPUT,&raw,&size,sizeof(RAWINPUTHEADER))!=UINT(-1)&&raw.header.dwType==RIM_TYPEMOUSE&&!(raw.data.mouse.usFlags&MOUSE_MOVE_ABSOLUTE)){
                DWORD pid=0;HWND fg=GetForegroundWindow();if(fg)GetWindowThreadProcessId(fg,&pid);
                std::lock_guard<std::mutex> g(self->lock);
                if(self->accept&&pid==self->gamePid)self->history.push(at,raw.data.mouse.lLastX,raw.data.mouse.lLastY);
            }
        }
        return DefWindowProcW(hwnd,msg,wp,lp);
    }
    void Run(){
        SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_ABOVE_NORMAL);
        const auto instance=GetModuleHandleW(nullptr);
        WNDCLASSW wc{};wc.hInstance=instance;wc.lpfnWndProc=Proc;wc.lpszClassName=L"FGDedicatedRawInput";
        ATOM atom=RegisterClassW(&wc);DWORD failure=0;
        if(!atom&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)failure=GetLastError();
        HWND hwnd=nullptr;
        if(!failure){hwnd=CreateWindowExW(0,wc.lpszClassName,L"",0,0,0,0,0,HWND_MESSAGE,nullptr,instance,this);if(!hwnd)failure=GetLastError();}
        bool registered=false;
        if(!failure){RAWINPUTDEVICE mouse{1,2,RIDEV_INPUTSINK,hwnd};registered=RegisterRawInputDevices(&mouse,1,sizeof(mouse))!=FALSE;if(!registered)failure=GetLastError();}
        {std::lock_guard<std::mutex> g(lock);tid=GetCurrentThreadId();error=failure;ready=true;}startup.notify_one();
        if(registered){MSG msg{};while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}
            RAWINPUTDEVICE remove{1,2,RIDEV_REMOVE,nullptr};RegisterRawInputDevices(&remove,1,sizeof(remove));}
        if(hwnd)DestroyWindow(hwnd);
        if(atom)UnregisterClassW(wc.lpszClassName,instance);
    }
public:
    explicit InputWorker(DWORD pid):gamePid(pid){
        history.reset(stamp());worker=std::thread([this]{Run();});
        std::unique_lock<std::mutex> g(lock);startup.wait(g,[this]{return ready;});
        if(error){g.unlock();worker.join();throw std::runtime_error("Dedicated Raw Input initialization failed");}
    }
    void Focus(bool focused){std::lock_guard<std::mutex> g(lock);accept=focused;history.reset(stamp());}
    bool Pair(int64_t previous,int64_t current,fglate::Counts& prev,fglate::Counts& cur,bool& havePair){
        std::lock_guard<std::mutex> g(lock);const bool sampled=accept&&history.at(current,cur);
        havePair=sampled&&previous<current&&history.at(previous,prev);return sampled;
    }
    std::array<fgmath::Delta,fgmath::delays> DelayedPairs(int64_t prev,int64_t cur,double ticksPerMs){
        std::array<fgmath::Delta,fgmath::delays> result{};
        std::lock_guard<std::mutex> g(lock);
        if(!accept||prev>=cur)return result;
        for(unsigned i=0;i<result.size();++i){
            fglate::Counts a{},b{};auto shift=int64_t(10.*i*ticksPerMs);
            if(history.at(prev-shift,a)&&history.at(cur-shift,b))result[i]={b.x-a.x,b.y-a.y,true};
        }return result;
    }
    bool Latest(int64_t now,double ticksPerMs,double lead,fglate::Counts& current,fglate::Counts& future){
        std::lock_guard<std::mutex> g(lock);if(!accept||!history.at(now,current))return false;
        future=history.predict(now,ticksPerMs,lead);return true;
    }
    ~InputWorker(){if(worker.joinable()){PostThreadMessageW(tid,WM_QUIT,0,0);worker.join();}}
};
}
