#pragma once
#include <array>
#include <cmath>
#include <algorithm>
#include <cstdint>
namespace fgmath {
constexpr double nominal=.022*1.5*3.14159265358979323846/180.;
constexpr unsigned count=48, delays=14;
struct Probe{float u,v,dx,dy,weight;};
struct Fit{double yaw{},pitch{},error{},motion{};unsigned used{};bool rotation{};};
inline Fit fit(const Probe* p,double aspect){
    constexpr double fy=1./(2.*.7500004); // tan(73.74/2 deg)
    const double fx=fy/aspect;
    Fit out{};
    for(int pass=0;pass<3;++pass){
        double aa=0,ab=0,bb=0,ax=0,bx=0,err=0,mag=0,weight=0;unsigned n=0;
        for(unsigned i=0;i<count;++i){
            if(!(p[i].weight>0))continue;
            double x=(p[i].u-.5)/fx,y=(p[i].v-.5)/fy;
            double a=fx*(1+x*x),b=fx*x*y,c=fy*x*y,d=fy*(1+y*y);
            double ex=p[i].dx-a*out.yaw-b*out.pitch,ey=p[i].dy-c*out.yaw-d*out.pitch;
            double e=std::hypot(ex,ey),q=p[i].weight;
            if(pass)q*=std::min(1.,.004/std::max(e,1e-8));
            aa+=q*(a*a+c*c);ab+=q*(a*b+c*d);bb+=q*(b*b+d*d);
            ax+=q*(a*p[i].dx+c*p[i].dy);bx+=q*(b*p[i].dx+d*p[i].dy);
            err+=q*e*e;mag+=q*(p[i].dx*p[i].dx+p[i].dy*p[i].dy);weight+=q;++n;
        }
        const double det=aa*bb-ab*ab;
        if(n<12||det<1e-8||weight<4)return {};
        out.error=std::sqrt(err/weight);out.motion=std::sqrt(mag/weight);out.used=n;
        out.yaw=(ax*bb-bx*ab)/det;out.pitch=(bx*aa-ax*ab)/det;
    }
    unsigned inliers=0;
    for(unsigned i=0;i<count;++i)if(p[i].weight>0){
        double x=(p[i].u-.5)/fx,y=(p[i].v-.5)/fy;
        double ex=p[i].dx-fx*((1+x*x)*out.yaw+x*y*out.pitch),ey=p[i].dy-fy*(x*y*out.yaw+(1+y*y)*out.pitch);
        if(std::hypot(ex,ey)<.0035+.15*out.motion)++inliers;
    }
    out.rotation=out.used>=16&&inliers*4>=out.used*3&&out.error<.0035+.15*out.motion&&std::hypot(out.yaw,out.pitch)<.16;
    return out;
}
struct Delta{double x{},y{};bool valid{};};
struct Calibrator{
    struct Stats{double xx{},xy{},yy{};unsigned n{};};
    std::array<Stats,delays> stats{};
    double delayMs{},gain=nominal;bool trusted=false;
    void reset(){*this=Calibrator{};}
    void observe(const Fit& f,const std::array<Delta,delays>& samples){
        if(!f.rotation||std::hypot(f.yaw,f.pitch)<.0015)return;
        for(unsigned i=0;i<delays;++i){
            auto s=samples[i];if(!s.valid||std::hypot(s.x,s.y)<3)continue;
            auto& a=stats[i];
            a.xx=.98*a.xx+s.x*s.x+s.y*s.y;a.xy=.98*a.xy+s.x*f.yaw+s.y*f.pitch;
            a.yy=.98*a.yy+f.yaw*f.yaw+f.pitch*f.pitch;++a.n;
        }
        double best=.72;int index=-1;double g=nominal;
        for(unsigned i=0;i<delays;++i){
            auto a=stats[i];if(a.n<20||a.xx<=0||a.yy<=0)continue;
            const double candidate=a.xy/a.xx,score=a.xy/std::sqrt(a.xx*a.yy);
            if(candidate<.3*nominal||candidate>1.6*nominal||score<=best)continue;
            best=score;index=int(i);g=candidate;
        }
        if(index>=0){delayMs=trusted?.85*delayMs+.15*(10.*index):10.*index;gain=trusted?.90*gain+.10*g:g;trusted=true;}
    }
};
}
