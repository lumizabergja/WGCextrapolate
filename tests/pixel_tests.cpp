#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>
using std::min;
struct float2 { float x,y; };
struct short2 { int16_t x,y; };
struct Index { unsigned x,y,z; };
Index blockIdx{},blockDim{},threadIdx{};
unsigned __float2uint_rn(float x){return (unsigned)std::nearbyint(x);}
#define __device__
#define __global__
#define __forceinline__ inline
#include "../src/predict.cu"

struct Frame {
    unsigned width,height,pitch;
    std::vector<unsigned> data;
    Frame(unsigned w,unsigned h,unsigned p) : width(w),height(h),pitch(p),data(h*p,0xdecadeff) {}
    unsigned& at(unsigned x,unsigned y){return data.at(y*pitch+x);}
};
unsigned pattern(int x,int y) {
    return 0xff000000u | (((x*11+y*3)%256)<<16) | (((x*7+y*13)%256)<<8) | ((x*19+y*5)%256);
}
unsigned long long address(const void* p){return (unsigned long long)(uintptr_t)p;}
std::vector<unsigned> display(const SourcePair& pair,unsigned w,unsigned h,float horizon,float r,float f,unsigned cut) {
    std::vector<unsigned> pixels(w*h);
    OutputImage output{address(pixels.data()),w,h};
    blockDim={1,1,1};
    for (unsigned y=0;y<h;++y) for(unsigned x=0;x<w;++x) {
        blockIdx={x,y,0};
        renderFrame(pair,output,horizon,r,f,horizon>0?&cut:nullptr);
    }
    return pixels;
}
int main(){
    const unsigned w=65,h=37,delta=2,flowPitch=20;
    Frame cur(w,h,75),prev(w,h,78);
    for(unsigned y=0;y<h;++y) for(unsigned x=0;x<w;++x) {
        prev.at(x,y)=pattern(x,y);
        cur.at(x,y)=pattern(std::max(int(x)-int(delta),0),y);
    }
    std::vector<short2> vectors(flowPitch*((h+3)/4),short2{-64,0});
    SourcePair pair{address(cur.data.data()),address(prev.data.data()),address(vectors.data()),
                    w,h,cur.pitch,prev.pitch,flowPitch};
    unsigned cut=42;
    sceneGate(pair,&cut); assert(cut==0);
    auto real=display(pair,w,h,0,0,0,0);
    auto predicted=display(pair,w,h,1.f,0,0,0);
    auto protectedCenter=display(pair,w,h,1.f,6,0,0);
    auto feather=display(pair,w,h,1.f,3,10,0);
    auto cutBypass=display(pair,w,h,1.f,0,0,1);
    for(unsigned y=0;y<h;y++) for(unsigned x=0;x<w;x++){
        unsigned i=y*w+x;
        assert(real[i]==cur.at(x,y));
        assert(cutBypass[i]==real[i]);
        if(x>=5) assert(predicted[i]==cur.at(x-delta,y));
        if(x<=2) assert(predicted[i]==real[i]);
        float d=std::hypot(float(x)-32.f,float(y)-18.f);
        if(d<5.9f)assert(protectedCenter[i]==real[i]);
        if(d>6.1f)assert(protectedCenter[i]==predicted[i]);
        if(d<2.9f)assert(feather[i]==real[i]);
        if(d>13.1f)assert(feather[i]==predicted[i]);
    }
    assert(feather[18*w+39]!=real[18*w+39] && feather[18*w+39]!=predicted[18*w+39]);
    std::fill(prev.data.begin(),prev.data.end(),0xff000000u);
    std::fill(cur.data.begin(),cur.data.end(),0xffffffffu);
    sceneGate(pair,&cut);assert(cut==1);
    const unsigned lumaPitch=w+8;
    std::vector<unsigned char> luma(lumaPitch*h,0xa5);
    for(unsigned y=0;y<h;++y) for(unsigned x=0;x<w;++x) cur.at(x,y)=pattern(x,y);
    const unsigned swatches[]={0xff000000u,0xffffffffu,0xffff0000u,0xff00ff00u,0xff0000ffu,0x000000ffu};
    std::copy(std::begin(swatches),std::end(swatches),cur.data.begin());
    for(unsigned i=0;i<256;++i) cur.at(i%w,1+i/w)=0xff000000u|i*0x010101u;
    for(unsigned y=0;y<h+2;y++) for(unsigned x=0;x<w+2;x++){
        blockIdx={x,y,0};
        stageFlowInput(cur.data.data(),cur.pitch,luma.data(),lumaPitch,w,h);
    }
    const unsigned char expected[]={0,255,76,150,29,29};
    assert(std::equal(std::begin(expected),std::end(expected),luma.begin()));
    for(unsigned i=0;i<256;++i) assert(luma[(1+i/w)*lumaPitch+i%w]==i);
    for(unsigned y=0;y<h;++y) {
        for(unsigned x=0;x<w;++x) {
            unsigned color=cur.at(x,y);
            long value=std::lround(.299*((color>>16)&255u)+.587*((color>>8)&255u)+.114*(color&255u));
            assert(std::abs(long(luma[y*lumaPitch+x])-value)<=1);
        }
        for(unsigned x=w;x<lumaPitch;++x) assert(luma[y*lumaPitch+x]==0xa5);
    }
    Frame small(4,2,7); for(unsigned y=0;y<2;y++)for(unsigned x=0;x<4;x++)small.at(x,y)=0xff000000u | x*40 | ((y*80)<<16);
    SourcePair colors{address(small.data.data()),0,0,4,2,7,7,0};
    auto scaled=display(colors,8,4,0,0,0,0);
    assert(scaled.front()==small.at(0,0) && scaled.back()==small.at(3,1));
    assert(scaled[1*8+1]==0xff14000au);
    std::puts("PASS: exact real-frame color, BGRA-to-luma, padded buffers, motion direction, bounds, scene cut, radius/feather, stretched resampling.");
}
