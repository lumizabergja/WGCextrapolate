#include "pixel_types.h"
#ifdef __CUDACC__
#include <cuda_runtime.h>
#endif

// Full-range BT.601 luma from NvFBC's little-endian BGRA pixels. CUDA NvOFA
// consumes one byte per pixel. The original color buffers feed presentation.
extern "C" __global__ void stageFlowInput(const unsigned* source, unsigned srcPitchPixels,
                                          unsigned char* destination, unsigned dstPitchBytes,
                                          unsigned width, unsigned height) {
    unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;
    unsigned pixel = source[y * srcPitchPixels + x];
    unsigned blue = pixel & 255u, green = (pixel >> 8) & 255u, red = (pixel >> 16) & 255u;
    unsigned luma = (19595u*red + 38470u*green + 7471u*blue + 32768u) >> 16;
    destination[y * dstPitchBytes + x] = static_cast<unsigned char>(luma);
}

struct Color { float b, g, r, a; };
__device__ __forceinline__ Color unpack(unsigned p) {
    return {float(p & 255), float((p >> 8) & 255), float((p >> 16) & 255), float(p >> 24)};
}
__device__ __forceinline__ unsigned pack(Color c) {
    return __float2uint_rn(c.b) | (__float2uint_rn(c.g) << 8) |
           (__float2uint_rn(c.r) << 16) | (__float2uint_rn(c.a) << 24);
}
__device__ __forceinline__ float mixValue(float a, float b, float f) { return fmaf(f, b-a, a); }
__device__ __forceinline__ Color mixColor(Color a, Color b, float f) {
    return {mixValue(a.b,b.b,f), mixValue(a.g,b.g,f), mixValue(a.r,b.r,f), mixValue(a.a,b.a,f)};
}
struct Cell { int lo, hi; float f; };
__device__ __forceinline__ Cell cell(float p, int size) {
    p = fminf(fmaxf(p, 0.0f), float(size-1));
    int lo = int(p);
    return {lo, min(lo+1,size-1), p-float(lo)};
}
__device__ __forceinline__ Color sampleColor(const SourcePair& pair, unsigned long long address,
                                            unsigned pitch, float2 uv) {
#if defined(__CUDA_ARCH__)
    const auto texture=address==pair.current ? pair.currentTexture : pair.previousTexture;
    if(texture){
        const float4 c=tex2D<float4>(static_cast<cudaTextureObject_t>(texture),uv.x,uv.y);
        return {c.x*255.f,c.y*255.f,c.z*255.f,c.w*255.f};
    }
#endif
    const auto* pixels = reinterpret_cast<const unsigned*>(address);
    Cell x = cell(uv.x * pair.width - 0.5f, pair.width);
    Cell y = cell(uv.y * pair.height - 0.5f, pair.height);
    const unsigned* row0 = pixels + y.lo*pitch;
    const unsigned* row1 = pixels + y.hi*pitch;
    Color top = mixColor(unpack(row0[x.lo]),unpack(row0[x.hi]),x.f);
    Color bottom = mixColor(unpack(row1[x.lo]),unpack(row1[x.hi]),x.f);
    return mixColor(top,bottom,y.f);
}

__device__ __forceinline__ float2 flowAt(const SourcePair& pair, float2 uv) {
    // NvOFA vectors are signed S10.5 translations in source pixels. Sample
    // at native 4x4 block centers, including the last partial row or column.
#if defined(__CUDA_ARCH__)
    if(pair.flowTexture){
        const float gx=uv.x*pair.width*0.25f/float((pair.width+3)/4);
        const float gy=uv.y*pair.height*0.25f/float((pair.height+3)/4);
        const float2 v=tex2D<float2>(static_cast<cudaTextureObject_t>(pair.flowTexture),gx,gy);
        return {v.x*(32767.f/(32.f*pair.width)),v.y*(32767.f/(32.f*pair.height))};
    }
#endif
    Cell x = cell(uv.x*pair.width*0.25f-0.5f, int((pair.width+3)/4));
    Cell y = cell(uv.y*pair.height*0.25f-0.5f, int((pair.height+3)/4));
    const short2* flow = reinterpret_cast<const short2*>(pair.flow);
    short2 a=flow[y.lo*pair.flowPitch+x.lo], b=flow[y.lo*pair.flowPitch+x.hi];
    short2 c=flow[y.hi*pair.flowPitch+x.lo], d=flow[y.hi*pair.flowPitch+x.hi];
    float vx=mixValue(mixValue(a.x,b.x,x.f),mixValue(c.x,d.x,x.f),y.f);
    float vy=mixValue(mixValue(a.y,b.y,x.f),mixValue(c.y,d.y,x.f),y.f);
    return {vx/(32.f*pair.width), vy/(32.f*pair.height)};
}
__device__ __forceinline__ bool outside(float2 uv) {
    return uv.x<0 || uv.y<0 || uv.x>1 || uv.y>1;
}
__device__ __forceinline__ float residual(Color a, Color b) {
    return (fabsf(a.r-b.r)+fabsf(a.g-b.g)+fabsf(a.b-b.b))*(.333333f/255.f);
}
__device__ __forceinline__ float smooth(float a, float b, float value) {
    float t=fminf(fmaxf((value-a)/(b-a),0.f),1.f);
    return t*t*(3.f-2.f*t);
}
__device__ __forceinline__ float magnitude(float x, float y) { return sqrtf(x*x+y*y); }

// Same five samples as the V15 vertex shader. Compute once per flow pair.
extern "C" __global__ void sceneGate(SourcePair pair, unsigned* cut) {
    const float2 probes[5]={{.2f,.3f},{.8f,.3f},{.5f,.5f},{.2f,.7f},{.8f,.7f}};
    float sum=0;
    for (int i=0;i<5;++i) {
        float2 uv=probes[i], v=flowAt(pair,uv);
        float2 old={uv.x+v.x,uv.y+v.y};
        if (!outside(old))
            sum+=residual(sampleColor(pair,pair.current,pair.currentPitch,uv),
                          sampleColor(pair,pair.previous,pair.previousPitch,old));
    }
    *cut = smooth(.19f,.32f,sum*.2f)>.9f;
}

__device__ __forceinline__ float protection(float2 uv, OutputImage target, float radius, float feather) {
    if (radius==0 && feather==0) return 1;
    float d=magnitude((uv.x-.5f)*target.width,(uv.y-.5f)*target.height);
    return feather>0 ? smooth(radius,radius+feather,d) : float(d>radius);
}
// Source-space photometric consistency is independent of prediction horizon.
// Keep the original per-output flow-discontinuity check.
__device__ __forceinline__ float confidenceAt(const SourcePair& pair,float2 uv) {
#if defined(__CUDA_ARCH__)
    if(pair.confidenceTexture)
        return tex2D<float>(static_cast<cudaTextureObject_t>(pair.confidenceTexture),uv.x,uv.y);
#endif
    const auto* p=reinterpret_cast<const unsigned char*>(pair.confidence);
    Cell x=cell(uv.x*pair.width-.5f,pair.width),y=cell(uv.y*pair.height-.5f,pair.height);
    return mixValue(mixValue(p[y.lo*pair.width+x.lo],p[y.lo*pair.width+x.hi],x.f),
                    mixValue(p[y.hi*pair.width+x.lo],p[y.hi*pair.width+x.hi],x.f),y.f)/255.f;
}
extern "C" __global__ void buildConfidence(SourcePair pair,unsigned char* out) {
    unsigned x=blockIdx.x*blockDim.x+threadIdx.x,y=blockIdx.y*blockDim.y+threadIdx.y;
    if(x>=pair.width||y>=pair.height)return;
    float2 uv={(x+.5f)/pair.width,(y+.5f)/pair.height},v=flowAt(pair,uv);
    float2 old={uv.x+v.x,uv.y+v.y};
    float confidence=0;
    if(!outside(old)&&magnitude(v.x,v.y)<=.12f)
        confidence=1.f-smooth(.045f,.20f,residual(
            sampleColor(pair,pair.current,pair.currentPitch,uv),
            sampleColor(pair,pair.previous,pair.previousPitch,old)));
    out[y*pair.width+x]=static_cast<unsigned char>(__float2uint_rn(confidence*255.f));
}
__device__ __forceinline__ Color predict(const SourcePair& pair, float2 uv, Color base,
                                         float horizon, float centerWeight) {
    if (centerWeight==0) return base;
    float2 v=flowAt(pair,uv);
    float2 q={uv.x+horizon*v.x,uv.y+horizon*v.y};
    if (outside(q)) return base;
    float2 mv=flowAt(pair,q);
    float2 old={q.x+mv.x,q.y+mv.y};
    if (outside(old) || magnitude(mv.x,mv.y)>.12f) return base;
    Color next=sampleColor(pair,pair.current,pair.currentPitch,q);
    float photo;
    if(pair.confidence)photo=confidenceAt(pair,q);
    else {
        Color prev=sampleColor(pair,pair.previous,pair.previousPitch,old);
        photo=1.f-smooth(.045f,.20f,residual(next,prev));
    }
    float confidence=photo*(1.f-smooth(.003f,.025f,magnitude(mv.x-v.x,mv.y-v.y)));
    return mixColor(base,next,confidence*centerWeight);
}

__device__ __forceinline__ unsigned renderedPixel(SourcePair pair, OutputImage target,float horizon,
                                       float radius,float feather,const unsigned* cut,unsigned x,unsigned y) {
    bool native=pair.width==target.width && pair.height==target.height;
    bool extrapolate=horizon>.00001f && !*cut;
    const unsigned* source=reinterpret_cast<const unsigned*>(pair.current);
    if (native && !extrapolate) return source[y*pair.currentPitch+x];
    float2 uv={(x+.5f)/target.width,(y+.5f)/target.height};
    Color base=native ? unpack(source[y*pair.currentPitch+x]) : sampleColor(pair,pair.current,pair.currentPitch,uv);
    if (extrapolate) base=predict(pair,uv,base,horizon,protection(uv,target,radius,feather));
    return pack(base);
}
extern "C" __global__ void renderFrame(SourcePair pair,OutputImage target,float horizon,float radius,float feather,const unsigned* cut){
    unsigned x=blockIdx.x*blockDim.x+threadIdx.x,y=blockIdx.y*blockDim.y+threadIdx.y;
    if(x>=target.width||y>=target.height)return;
    reinterpret_cast<unsigned*>(target.pixels)[y*target.width+x]=renderedPixel(pair,target,horizon,radius,feather,cut,x,y);
}
#ifdef __CUDACC__
extern "C" __global__ void renderSurface(SourcePair pair,OutputImage target,float horizon,float radius,float feather,const unsigned* cut,cudaSurfaceObject_t surface){
    unsigned x=blockIdx.x*blockDim.x+threadIdx.x,y=blockIdx.y*blockDim.y+threadIdx.y;
    if(x>=target.width||y>=target.height)return;
    surf2Dwrite(renderedPixel(pair,target,horizon,radius,feather,cut,x,y),surface,x*4,y);
}
#endif
extern "C" __global__ void renderReal(SourcePair pair,OutputImage target){
    unsigned x=blockIdx.x*blockDim.x+threadIdx.x,y=blockIdx.y*blockDim.y+threadIdx.y;
    if(x>=target.width||y>=target.height)return;
    reinterpret_cast<unsigned*>(target.pixels)[y*target.width+x]=renderedPixel(pair,target,0,0,0,nullptr,x,y);
}
#ifdef __CUDACC__
extern "C" __global__ void renderRealSurface(SourcePair pair,OutputImage target,cudaSurfaceObject_t surface){
    unsigned x=blockIdx.x*blockDim.x+threadIdx.x,y=blockIdx.y*blockDim.y+threadIdx.y;
    if(x>=target.width||y>=target.height)return;
    surf2Dwrite(renderedPixel(pair,target,0,0,0,nullptr,x,y),surface,x*4,y);
}
#endif
