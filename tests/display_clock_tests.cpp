#include <cassert>
#include <cmath>
#include <cstdio>
#include "../src/display_clock.h"
int main(){
  for(int fps:{40,60,80,100,120}){
    const double step=displayPeriod(0,320,1.0/fps);
    assert(std::abs(step-0.003125)<1e-12);
    DisplayClock c; c.configure(0,step);
    int n=0;
    for(int i=0;i<100000;++i){
      double now=i*0.00001;
      c.configure(now,step);
      if(c.due(now,false)){c.submitted(now);++n;}
    }
    assert(n==320);
  }
  DisplayClock c; c.configure(0,0.003125);c.submitted(0);
  assert(!c.due(0.001,false));assert(c.due(0.0026,true));
  c.submitted(0.0026); assert(std::abs(c.deadline()-0.00625)<1e-9);
  c.submitted(0.030);assert(c.deadline()>0.030);
  c.reset();c.configure(10,0.003125);assert(c.due(10,false));
  assert(std::abs(displayPeriod(4,320,1.0/60)-1.0/240)<1e-12);
  puts("Display clock tests passed");
}
