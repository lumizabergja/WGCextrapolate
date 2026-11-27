#include "../src/camera_fit.h"
#include <cassert>
#include <iostream>
int main(){
    std::array<fgmath::Probe,48> p{};double fx=(1./1.5000008)/(4./3.),fy=1./1.5000008;
    double yaw=.02,pitch=-.015;
    for(unsigned i=0;i<p.size();++i){auto& s=p[i];s.u=(i%8+.5f)/8;s.v=(i/8+.5f)/6;double x=(s.u-.5)/fx,y=(s.v-.5)/fy;s.dx=float(fx*((1+x*x)*yaw+x*y*pitch));s.dy=float(fy*(x*y*yaw+(1+y*y)*pitch));s.weight=1;}
    auto f=fgmath::fit(p.data(),4./3.);assert(f.rotation&&std::abs(f.yaw-yaw)<1e-5&&std::abs(f.pitch-pitch)<1e-5);
    fgmath::Calibrator c;
    for(int i=0;i<40;++i){std::array<fgmath::Delta,14> d{};d[3]={yaw/fgmath::nominal,pitch/fgmath::nominal,true};c.observe(f,d);}
    assert(c.trusted&&std::abs(c.delayMs-30)<.1&&std::abs(c.gain-fgmath::nominal)<1e-6);
    for(auto& s:p)s.weight=0;assert(!fgmath::fit(p.data(),4./3.).rotation);
    std::cout<<"camera fitting tests passed\n";
}
