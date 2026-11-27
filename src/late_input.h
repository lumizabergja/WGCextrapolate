#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <cmath>

// Single render/message thread. All stamps are QPC ticks, not device timestamps.
namespace fglate {
struct Counts { double x{},y{}; };
struct Angles { float x{},y{}; };
struct Correction { Angles actual{},residual{}; bool valid{}; Angles historical{}; };
class History {
    struct Sample { int64_t t{},x{},y{}; };
    static constexpr std::size_t capacity=2048;
    std::array<Sample,capacity> samples{};
    std::size_t head{},count{};
    int64_t x{},y{};
public:
    void reset(int64_t t){head=count=0;x=y=0;push(t,0,0);}
    void push(int64_t t,int dx,int dy){
        if(count)t=std::max(t,samples[(head+capacity-1)%capacity].t);
        x+=dx;y+=dy;samples[head]={t,x,y};head=(head+1)%capacity;
        count=std::min(count+1,capacity);
    }
    bool at(int64_t t,Counts& out) const {
        if(!count)return false;
        const std::size_t first=(head+capacity-count)%capacity;
        if(t<samples[first].t)return false;
        std::size_t lo=0,hi=count;
        while(lo<hi){const std::size_t mid=(lo+hi)/2;
            if(samples[(first+mid)%capacity].t<=t)lo=mid+1;else hi=mid;}
        const auto& s=samples[(first+lo-1)%capacity];out={double(s.x),double(s.y)};return true;
    }
    // Estimate future relative counts from three recent, coherent intervals.
    // Receipt timestamps are not hardware timestamps; reject bursts and stale data.
    Counts predict(int64_t latest,double ticksPerMs,double maxMs=5.) const {
        if(count<4||ticksPerMs<=0||maxMs<=0)return {};
        const auto& last=samples[(head+capacity-1)%capacity];
        const double age=double(latest-last.t)/ticksPerMs;
        if(age<0||age>=2.)return {}; // target 1000 Hz; do not invent stale movement
        Counts v[3]{};double speed[3]{};
        for(std::size_t i=0;i<3;++i){
            const auto& a=samples[(head+capacity-4+i)%capacity];
            const auto& b=samples[(head+capacity-3+i)%capacity];
            const double dt=double(b.t-a.t)/ticksPerMs;
            if(dt<.45||dt>2.)return {};
            v[i]={double(b.x-a.x)/dt,double(b.y-a.y)/dt};
            speed[i]=std::hypot(v[i].x,v[i].y);
            if(speed[i]<.25)return {};
        }
        double coherence=1.;
        for(int i=0;i<2;++i){
            const double dot=(v[i].x*v[2].x+v[i].y*v[2].y)/(speed[i]*speed[2]);
            const double ratio=std::min(speed[i],speed[2])/std::max(speed[i],speed[2]);
            if(dot<.85||ratio<.55)return {};
            coherence=std::min(coherence,std::min(dot,ratio));
        }
        const double stability=std::clamp((coherence-.55)/.45,0.,1.);
        const double freshness=std::clamp(2.-age,0.,1.);
        const double lead=std::min(maxMs,5.)*stability*freshness;
        Counts out{lead*(.2*v[0].x+.3*v[1].x+.5*v[2].x),
                   lead*(.2*v[0].y+.3*v[1].y+.5*v[2].y)};
        if(speed[2]>speed[1]&&speed[1]>=speed[0]&&stability>.5){
            const double dt=double(samples[(head+capacity-1)%capacity].t-samples[(head+capacity-2)%capacity].t)/ticksPerMs;
            const double gain=.25*lead*lead/std::max(.45,dt);
            double dx=(v[2].x-v[1].x)*gain,dy=(v[2].y-v[1].y)*gain;
            const double cap=.25*std::hypot(out.x,out.y);
            const double scale=std::max(1.,std::hypot(dx,dy)/std::max(.0001,cap));
            out.x+=dx/scale;out.y+=dy/scale;
        }
        // Separate prediction-only clamp: 1.5 degrees at the configured sensitivity.
        const double scale=std::max(1.,std::hypot(out.x,out.y)/(1.5/(.022*1.5)));
        return {out.x/scale,out.y/scale};
    }
    Correction correction(int64_t previous,int64_t current,int64_t latest,double horizon,double ticksPerMs=0.,double maxMs=5.) const {
        Counts p,c,n;
        if(previous>=current||latest<current||!at(previous,p)||!at(current,c)||!at(latest,n))return {};
        constexpr double rad=.022*1.5*3.14159265358979323846/180.;
        constexpr double limit=4.*3.14159265358979323846/180.;
        const auto future=predict(latest,ticksPerMs,maxMs);
        const double ax=(n.x-c.x+future.x)*rad,ay=(n.y-c.y+future.y)*rad;
        const double rx=ax-horizon*(c.x-p.x)*rad,ry=ay-horizon*(c.y-p.y)*rad;
        // Bound fast flicks rather than expose large missing image regions.
        auto bounded=[&](double a,double b){const double scale=std::max(1.,std::hypot(a,b)/limit);return Angles{float(a/scale),float(b/scale)};};
        if(std::abs(ax)+std::abs(ay)+std::abs(rx)+std::abs(ry)<1e-8)return {};
        return {bounded(ax,ay),bounded(rx,ry),true,{float((c.x-p.x)*rad),float((c.y-p.y)*rad)}};
    }
};
}
