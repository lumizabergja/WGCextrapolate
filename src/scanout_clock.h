#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
// All times in QPC-derived milliseconds. Refresh phase is advisory,
// never a reason to hold an already-ready present or invent scanout time.
class ScanoutClock {
    double anchor=0,period=0,lastObserved=0;
    uint32_t refresh=0;unsigned stable=0;
    double full=.8,late=.4;unsigned samples=0;
public:
    void ResetPhase(){anchor=period=lastObserved=0;refresh=stable=0;}
    void Observe(double qpcMs,uint32_t count,double now,double nominal){
        if(!std::isfinite(qpcMs)||qpcMs<=0||nominal<=0||qpcMs>now+nominal||now-qpcMs>nominal*8){ResetPhase();return;}
        if(!anchor){anchor=qpcMs;refresh=count;lastObserved=now;return;}
        if(count==refresh)return;
        uint32_t n=count-refresh;
        double measured=(qpcMs-anchor)/double(n);
        if(n>32||!std::isfinite(measured)||std::abs(measured-nominal)>nominal*.12){ResetPhase();anchor=qpcMs;refresh=count;lastObserved=now;return;}
        period=stable?period*.8+measured*.2:measured;
        stable=std::min(8u,stable+1);anchor=qpcMs;refresh=count;lastObserved=now;
    }
    bool PhaseValid(double now)const{return stable>=3&&period>0&&now>=lastObserved&&now-lastObserved<period*8;}
    void Gpu(double total,double tail){
        if(!std::isfinite(total)||!std::isfinite(tail)||tail<0||total<tail||total>100)return;
        full=samples?full*.85+total*.15:total;late=samples?late*.85+tail*.15:tail;
        samples=std::min(100u,samples+1);
    }
    double FullMs()const{return full;}
    double LateMs()const{return late;}
    bool Measured()const{return samples>=3;}
    // Only phase-align the target prediction when fixed-refresh phase is
    // credible and tearing is disabled. No claim of measured pixel scanout.
    double Lead(double now,double outputPeriod,double monitorPeriod,bool tearing,double hostMs)const{
        double cap=std::min(5.,std::max(0.,outputPeriod));
        double tail=std::max(std::clamp(hostMs,0.,5.),late);
        double lead=tail+monitorPeriod*.5;
        if(!tearing&&PhaseValid(now)){
            double complete=now+tail;
            double next=anchor+(std::floor((complete-anchor)/period)+1.)*period;
            lead=next-now+period*.5;
        }
        return std::clamp(lead,0.,cap);
    }
    double EarlyMs(double outputPeriod)const{
        return Measured()?std::clamp(full*.5,0.,outputPeriod*.20):0.;
    }
    bool DepthBudget(double slack,double outputPeriod)const{
        return slack>std::min(outputPeriod*.55,std::max(.35,full*.65));
    }
};
