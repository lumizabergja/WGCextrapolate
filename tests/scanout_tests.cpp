#include "../src/scanout_clock.h"
#include "../src/display_clock.h"
#include <cassert>
#include <cmath>
int main(){
    ScanoutClock c;
    const double p=3.125;
    assert(!c.PhaseValid(100));
    for(unsigned i=0;i<5;++i)c.Observe(100+i*p,100+i,100+i*p+.1,p);
    assert(c.PhaseValid(113));assert(!c.PhaseValid(200));
    for(int i=0;i<3;++i)c.Gpu(1.0,.5);
    assert(c.Measured());assert(std::abs(c.FullMs()-1.)<1e-8);
    assert(c.EarlyMs(p)<=p*.2);assert(c.Lead(113,p,p,true,.1)<=p);
    assert(std::abs(c.Lead(113,p,p,true,.1)-2.0625)<1e-8);
    assert(c.Lead(113,p,p,false,.1)<=p);
    c.Observe(114,200,114,p);assert(!c.PhaseValid(114));
    assert(!c.DepthBudget(.1,p));assert(c.DepthBudget(2.,p));
    DisplayClock d;d.configure(1.,.003125);d.submitted(1.);
    assert(!d.due(1.0024,false,.0005));assert(d.due(1.0027,false,.0005));
}
