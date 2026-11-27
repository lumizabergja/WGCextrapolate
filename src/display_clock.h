#pragma once
#include <cmath>
#include <algorithm>

// Time is expressed in QPC seconds. Keep the display phase independent of
// capture arrivals and of the time spent submitting a frame to DXGI.
class DisplayClock {
    double deadline_=0, period_=0;
    bool initialized_=false;
public:
    void reset() { initialized_=false; deadline_=period_=0; }
    void configure(double now,double period) {
        if(!initialized_) { deadline_=now; initialized_=true; }
        else if(period_>0 && std::abs(period-period_)>period_*0.15) {
            deadline_=now+std::clamp((deadline_-now)/period_,0.0,1.0)*period;
        }
        period_=period;
    }
    bool due(double now,bool real,double early=0.) const {
        return initialized_ && now>=deadline_-(real?period_*0.20:std::clamp(early,0.,period_*.20));
    }
    void submitted(double now) {
        if(period_<=0)return;
        // Skip missed slots instead of submitting a catch-up burst.
        const double steps=std::max(1.0,std::floor((now-deadline_)/period_)+1.0);
        deadline_+=steps*period_;
    }
    double deadline() const {return deadline_;}
};
inline double displayPeriod(int mode,int target,double sourcePeriod) {
    // Dynamic is a refresh-rate target, not an integer frame multiplier.
    return mode==0 ? 1.0/double(target) : sourcePeriod/double(std::max(1,mode));
}
