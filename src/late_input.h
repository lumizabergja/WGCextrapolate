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
struct Correction { Angles actual{},residual{}; bool valid{}; };
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
    Correction correction(int64_t previous,int64_t current,int64_t latest,double horizon) const {
        Counts p,c,n;
        if(previous>=current||latest<current||!at(previous,p)||!at(current,c)||!at(latest,n))return {};
        constexpr double rad=.022*1.5*3.14159265358979323846/180.;
        constexpr double limit=4.*3.14159265358979323846/180.;
        const double ax=(n.x-c.x)*rad,ay=(n.y-c.y)*rad;
        const double rx=ax-horizon*(c.x-p.x)*rad,ry=ay-horizon*(c.y-p.y)*rad;
        // Bound fast flicks rather than expose large missing image regions.
        auto bounded=[&](double a,double b){const double scale=std::max(1.,std::hypot(a,b)/limit);return Angles{float(a/scale),float(b/scale)};};
        if(std::abs(ax)+std::abs(ay)+std::abs(rx)+std::abs(ry)<1e-8)return {};
        return {bounded(ax,ay),bounded(rx,ry),true};
    }
};
}
