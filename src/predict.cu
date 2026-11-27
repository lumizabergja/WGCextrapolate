#include "pixel_types.h"
#ifdef __CUDACC__
#include <cuda_fp16.h>
#endif
#include "freeze_regions.h"
__device__ __forceinline__ bool fixedFreeze(float2 uv,unsigned flags){
    const float x=uv.x*1650.f,y=uv.y*927.f;
    return ((flags&1u)&&FG_HUD_REGION(x,y))||((flags&2u)&&FG_GUN_REGION(x,y));
}

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
    Color prev=sampleColor(pair,pair.previous,pair.previousPitch,old);
    float confidence=(1.f-smooth(.045f,.20f,residual(next,prev)))*
                     (1.f-smooth(.003f,.025f,magnitude(mv.x-v.x,mv.y-v.y)));
    return mixColor(base,next,confidence*centerWeight);
}

extern "C" __global__ void renderFrame(SourcePair pair, OutputImage target, float horizon,
                                       float radius, float feather, const unsigned* cut,unsigned flags) {
    unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x>=target.width || y>=target.height) return;
    unsigned* destination=reinterpret_cast<unsigned*>(target.pixels)+y*target.width+x;
    bool native=pair.width==target.width && pair.height==target.height;
    bool extrapolate=horizon>.00001f && cut && !*cut && pair.flow && pair.flowPitch;
    const unsigned* source=reinterpret_cast<const unsigned*>(pair.current);
    if (native && !extrapolate) { *destination=source[y*pair.currentPitch+x]; return; }
    float2 uv={(x+.5f)/target.width,(y+.5f)/target.height};
    Color base=native ? unpack(source[y*pair.currentPitch+x]) : sampleColor(pair,pair.current,pair.currentPitch,uv);
    if (extrapolate && !fixedFreeze(uv,flags)) base=predict(pair,uv,base,horizon,protection(uv,target,radius,feather));
    *destination=pack(base);
}

#ifdef __CUDACC__
// ZipDepth reference preprocessing: RGB float CHW in [0,1].
extern "C" __global__ void prepareDepthRGB(const unsigned* pixels,unsigned pitch,unsigned sw,unsigned sh,float* tensor){
    constexpr unsigned W=512,H=384;
    unsigned x=blockIdx.x*blockDim.x+threadIdx.x,y=blockIdx.y*blockDim.y+threadIdx.y;
    if(x>=W||y>=H)return;
    SourcePair pair{};pair.width=sw;pair.height=sh;pair.current=reinterpret_cast<unsigned long long>(pixels);
    float2 uv={(x+.5f)/W,(y+.5f)/H};
    Color c=sampleColor(pair,pair.current,pitch,uv);
    unsigned i=y*W+x;
    tensor[i]=c.r/255.f;tensor[i+W*H]=c.g/255.f;tensor[i+2*W*H]=c.b/255.f;
}
// Robust approximate inverse-depth range. This is NOT metric calibration.
extern "C" __global__ void depthRange(const float* depth,float* range,float* temporal,unsigned contiguous){
    float values[64];unsigned n=0;
    for(unsigned y=0;y<8;y++)for(unsigned x=0;x<8;x++){
        float v=depth[(y*48+24)*512+x*64+32];
        if(isfinite(v))values[n++]=v;
    }
    if(n<8){range[0]=0;range[1]=0;temporal[0]=0;temporal[1]=0;return;}
    for(unsigned i=1;i<n;i++){
        float v=values[i];unsigned j=i;
        while(j&&values[j-1]>v){values[j]=values[j-1];--j;}
        values[j]=v;
    }
    float lo=values[n/10],hi=values[n-1-n/10];
    if(contiguous&&temporal[1]-temporal[0]>1.e-6f&&hi-lo>1.e-6f){
        float oldSpan=temporal[1]-temporal[0],span=hi-lo;
        // Reject large scale or scene changes rather than smear calibration.
        if(span>.5f*oldSpan&&span<2.f*oldSpan&&fabsf(lo-temporal[0])<oldSpan*.5f){
            lo=mixValue(temporal[0],lo,.25f);hi=mixValue(temporal[1],hi,.25f);
        }
    }
    range[0]=temporal[0]=lo;range[1]=temporal[1]=hi;
}
__device__ __forceinline__ float readDepth(const float* d,float2 uv){
    Cell x=cell(uv.x*512-.5f,512),y=cell(uv.y*384-.5f,384);
    return mixValue(mixValue(d[y.lo*512+x.lo],d[y.lo*512+x.hi],x.f),
                    mixValue(d[y.hi*512+x.lo],d[y.hi*512+x.hi],x.f),y.f);
}
// Follow the original extrapolator's coordinates and confidence blend so
// synthesized depth approximates the synthesized color. Depth is relative.
// One motion lookup / confidence blend for color and depth/motion.
// Fixed-freeze regions use the exact real-frame color and neutral guidance.
extern "C" __global__ void renderFused(SourcePair pair,OutputImage target,OutputImage guidance,float horizon,float radius,float feather,
                                      const unsigned* cut,const float* raw,const float* range,float interval,unsigned previousDepth,unsigned flags){
    unsigned x=blockIdx.x*blockDim.x+threadIdx.x,y=blockIdx.y*blockDim.y+threadIdx.y;
    if(x>=target.width||y>=target.height)return;
    unsigned idx=y*target.width+x;
    float2 uv={(x+.5f)/target.width,(y+.5f)/target.height};
    bool native=pair.width==target.width&&pair.height==target.height;
    Color base=native?unpack(reinterpret_cast<const unsigned*>(pair.current)[y*pair.currentPitch+x]):sampleColor(pair,pair.current,pair.currentPitch,uv);
    if(fixedFreeze(uv,flags)){
        reinterpret_cast<unsigned*>(target.pixels)[idx]=pack(base);
        reinterpret_cast<float4*>(guidance.pixels)[idx]={.999f,0,0,0};return;
    }
    float d=readDepth(raw,uv);float2 motion={0,0};
    if(previousDepth&&cut&&*cut){reinterpret_cast<unsigned*>(target.pixels)[idx]=pack(base);reinterpret_cast<float4*>(guidance.pixels)[idx]={.999f,0,0,0};return;}
    if(cut&&!*cut&&pair.flow&&pair.flowPitch){
        float2 v=flowAt(pair,uv),q={uv.x+horizon*v.x,uv.y+horizon*v.y};
        if(!outside(q)){
            float2 mv=flowAt(pair,q),old={q.x+mv.x,q.y+mv.y};
            float mag=magnitude(mv.x,mv.y);
            if(!outside(old)&&mag<=.12f){
                Color next=sampleColor(pair,pair.current,pair.currentPitch,q);
                Color prev=sampleColor(pair,pair.previous,pair.previousPitch,old);
                float confidence=(1.f-smooth(.045f,.20f,residual(next,prev)))*(1.f-smooth(.003f,.025f,magnitude(mv.x-v.x,mv.y-v.y)));
                float weight=confidence*protection(uv,target,radius,feather);
                if(horizon>.00001f)base=mixColor(base,next,weight);
                if(mag<.12f){motion={mv.x*target.width*interval*weight,mv.y*target.height*interval*weight};if(previousDepth)d=mixValue(d,readDepth(raw,old),weight);}
                if(!previousDepth&&horizon>.00001f)d=mixValue(d,readDepth(raw,q),weight);
            }
        }
    }
    float lo=range[0],hi=range[1];
    float disparity=(isfinite(d)&&hi-lo>1.e-6f)?fminf(1.f,fmaxf(0.f,(d-lo)/(hi-lo))):0.f;
    float inv=.02f+1.98f*disparity;
    float depth=(1000.f/(1000.f-.1f))*(1.f-.1f*inv);
    reinterpret_cast<unsigned*>(target.pixels)[idx]=pack(base);
    reinterpret_cast<float4*>(guidance.pixels)[idx]={fminf(1.f,fmaxf(0.f,depth)),motion.x,motion.y,0.f};
}

// Explicit pitched linear planes copied into mapped D3D11 CUDA arrays.
// No imported NvFBC alias, persistent CUDA surface, or CPU staging.
__device__ __forceinline__ void writeDirect(OutputImage color,OutputImage depth,unsigned long long motion,unsigned cp,unsigned dp,unsigned mp,unsigned x,unsigned y,Color c,float z,float mx,float my){
    unsigned p=pack(c);unsigned rgba=(p&0xff00ff00u)|((p&255u)<<16)|((p>>16)&255u);
    reinterpret_cast<unsigned*>(color.pixels+y*static_cast<unsigned long long>(cp))[x]=rgba;
    reinterpret_cast<float*>(depth.pixels+y*static_cast<unsigned long long>(dp))[x]=z;
    reinterpret_cast<__half2*>(motion+y*static_cast<unsigned long long>(mp))[x]=__floats2half2_rn(mx,my);
}
extern "C" __global__ void renderDirect(SourcePair pair,OutputImage target,OutputImage guidance,unsigned long long motionPlane,unsigned colorPitch,unsigned depthPitch,unsigned motionPitch,float horizon,float radius,float feather,
                                      const unsigned* cut,const float* raw,const float* range,float interval,unsigned previousDepth,unsigned flags){
    unsigned x=blockIdx.x*blockDim.x+threadIdx.x,y=blockIdx.y*blockDim.y+threadIdx.y;
    if(x>=target.width||y>=target.height)return;
    unsigned idx=y*target.width+x;
    (void)idx;
    float2 uv={(x+.5f)/target.width,(y+.5f)/target.height};
    bool native=pair.width==target.width&&pair.height==target.height;
    Color base=native?unpack(reinterpret_cast<const unsigned*>(pair.current)[y*pair.currentPitch+x]):sampleColor(pair,pair.current,pair.currentPitch,uv);
    if(fixedFreeze(uv,flags)){
        writeDirect(target,guidance,motionPlane,colorPitch,depthPitch,motionPitch,x,y,base,.999f,0.f,0.f);return;
    }
    float d=readDepth(raw,uv);float2 motion={0,0};
    if(previousDepth&&cut&&*cut){writeDirect(target,guidance,motionPlane,colorPitch,depthPitch,motionPitch,x,y,base,.999f,0.f,0.f);return;}
    if(cut&&!*cut&&pair.flow&&pair.flowPitch){
        float2 v=flowAt(pair,uv),q={uv.x+horizon*v.x,uv.y+horizon*v.y};
        if(!outside(q)){
            float2 mv=flowAt(pair,q),old={q.x+mv.x,q.y+mv.y};
            float mag=magnitude(mv.x,mv.y);
            if(!outside(old)&&mag<=.12f){
                Color next=sampleColor(pair,pair.current,pair.currentPitch,q);
                Color prev=sampleColor(pair,pair.previous,pair.previousPitch,old);
                float confidence=(1.f-smooth(.045f,.20f,residual(next,prev)))*(1.f-smooth(.003f,.025f,magnitude(mv.x-v.x,mv.y-v.y)));
                float weight=confidence*protection(uv,target,radius,feather);
                if(horizon>.00001f)base=mixColor(base,next,weight);
                if(mag<.12f){motion={mv.x*target.width*interval*weight,mv.y*target.height*interval*weight};if(previousDepth)d=mixValue(d,readDepth(raw,old),weight);}
                if(!previousDepth&&horizon>.00001f)d=mixValue(d,readDepth(raw,q),weight);
            }
        }
    }
    float lo=range[0],hi=range[1];
    float disparity=(isfinite(d)&&hi-lo>1.e-6f)?fminf(1.f,fmaxf(0.f,(d-lo)/(hi-lo))):0.f;
    float inv=.02f+1.98f*disparity;
    float depth=(1000.f/(1000.f-.1f))*(1.f-.1f*inv);
    writeDirect(target,guidance,motionPlane,colorPitch,depthPitch,motionPitch,x,y,base,fminf(1.f,fmaxf(0.f,depth)),motion.x,motion.y);
}
#endif
