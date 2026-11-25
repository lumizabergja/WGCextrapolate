#include "../src/cs2_mouse.h"
#include <cassert>
#include <cstdio>

int main() {
    constexpr std::int64_t hz=1000000;
    cs2::MouseHistory h;
    assert(!h.at(500).valid);
    h.record(0,0,0);
    h.record(1000,8,0);
    h.record(2000,16,0);
    h.record(3000,24,0);
    const auto capture=h.at(1999);
    assert(capture.valid && capture.x==8);
    assert(!h.at(-1).valid && h.at(2000).x==16);
    assert(h.since(capture,3000,hz,false)==cs2::fromCounts(16,0,8));
    assert(h.since(capture,3000,hz,true)==cs2::fromCounts(24,0,8));
    assert(h.since(capture,4250,hz,true)==cs2::fromCounts(18,0,8));
    assert(h.since(capture,4500,hz,true)==h.since(capture,4500,hz,false));
    h.record(4000,16,0); // Direction reversal immediately cancels the lead.
    assert(h.since(capture,4000,hz,true)==cs2::fromCounts(8,0,8));
    h.record(4100,8,0); // A bunched report still supplies measured position.
    assert(!h.since(capture,4100,hz,true).moved());
    h.record(5100,8,0); // Button-only report.
    assert(!h.since(capture,5100,hz,true).moved());
    h.clear();
    assert(!h.since(capture,6000,hz,true).moved());
    for(int i=0;i<600;++i)h.record(10000+i,100+i,-i);
    assert(!h.at(10087).valid && h.at(10088).x==188);
    assert(h.at(10599).y==-599);
    assert(!h.since({},11000,hz,false).moved());
    auto extreme=cs2::fromCounts(1000000,-1000000,8);
    assert(extreme.sinYaw>0 && extreme.sinPitch<0);
    assert(std::hypot(std::asin(extreme.sinYaw),std::asin(extreme.sinPitch))<.14);
    std::puts("Mouse timestamp, direction, capped lead, stale input and focus reset checks passed.");
}
