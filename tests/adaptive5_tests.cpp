#include <cassert>
#include <cmath>
#include "../src/late_input.h"
int main(){
  fglate::History h;h.reset(0);
  for(int i=1;i<=5;++i)h.push(i*1000,4,0);
  auto p=h.predict(5000,1000,5.);
  assert(std::abs(p.x-20.)<1e-6 && p.y==0);
  assert(h.predict(7000,1000,5.).x==0.);
  h.push(6000,-4,0);
  assert(h.predict(6000,1000,5.).x==0.);
  h.reset(0);for(int i=1;i<=5;++i)h.push(i*1000,0,0);
  assert(h.predict(5000,1000,5.).x==0.);
}
