#include "../src/late_input.h"
#include <cassert>
#include <cmath>
#include <iostream>
int main(){
    fglate::History h;h.reset(0);
    h.push(10,100,-50);h.push(20,50,-25);
    fglate::Counts at{};
    assert(h.at(10,at)&&at.x==100&&at.y==-50);
    assert(h.at(19,at)&&at.x==100);
    const auto constant=h.correction(0,10,20,.5);
    assert(constant.valid&&constant.actual.x>0&&constant.actual.y<0);
    assert(std::abs(constant.residual.x)<1e-7&&std::abs(constant.residual.y)<1e-7);
    const auto stop=h.correction(0,10,19,.5);
    assert(stop.valid&&stop.actual.x==0&&stop.residual.x<0);
    const auto idle=h.correction(20,30,40,.5);assert(!idle.valid);
    const auto fresh=h.correction(10,19,20,.5);
    assert(fresh.valid&&fresh.residual.x>0);
    h.push(30,-100,50);
    const auto reverse=h.correction(10,20,30,.5);
    assert(reverse.valid&&reverse.actual.x<0&&reverse.residual.x<0);
    h.push(40,20000,-20000);
    const auto flick=h.correction(20,30,40,.5);
    assert(flick.valid&&std::hypot(flick.actual.x,flick.actual.y)<.070);
    assert(std::hypot(flick.residual.x,flick.residual.y)<.070);
    for(int i=0;i<3000;++i)h.push(50+i,1,0);
    assert(!h.at(20,at));assert(!h.correction(20,30,4000,.5).valid);
    h.reset(5000);assert(!h.correction(4000,5000,6000,.5).valid);
    std::cout<<"PASS: timestamp lookup, steady motion, stop, idle, fresh input, reversal, flick clamp, history overflow, reset\n";
}
