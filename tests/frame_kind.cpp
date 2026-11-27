#include "../src/frame_kind.h"
#include <cassert>
#include <cstdio>
int main(){
    int warped=0;
    auto route=[&](bool real,float horizon,bool ready){
        if(ShouldRecordNativeLatewarp(ClassifyFrame(real,horizon),ready))++warped;
    };
    route(true,0,false); assert(warped==0);
    route(true,0,true); assert(warped==1);
    for(int i=1;i<=5;++i)route(false,i/6.f,true);
    route(false,0,true); assert(warped==1);
    route(true,0,true); assert(warped==2);
    assert(ClassifyFrame(false,0)==FrameKind::Repeat);
    assert(ClassifyFrame(false,.1f)==FrameKind::Generated);
    std::puts("PASS: only new capture is eligible; generated and repeated frames bypass; missing backend never runs.");
}
