#include <cuda_runtime.h>
#include "dp4a_kernel.h"

// A group of four neighboring luminance bytes, at an arbitrary x offset.
__device__ __forceinline__ unsigned packed4(const unsigned* p,int pitch,int x,int y) {
    const unsigned* row=p+y*pitch; const int k=x>>2, shift=(x&3)*8;
    unsigned a=row[k];
    return shift? (a>>shift)|(row[k+1]<<(32-shift)):a;
}
struct Patch { unsigned a[8]; unsigned norm; };
__device__ __forceinline__ Patch loadPatch(const unsigned* p,int pitch,int x,int y){
    Patch t{};
    #pragma unroll
    for(int r=0;r<4;++r){
        unsigned a=packed4(p,pitch,x,y+r*2),b=packed4(p,pitch,x+4,y+r*2);
        t.a[r*2]=a;t.a[r*2+1]=b;
        t.norm=__dp4a(a,a,t.norm);t.norm=__dp4a(b,b,t.norm);
    }
    return t;
}
__device__ __forceinline__ int score(const Patch& a,const unsigned* prev,int pitch,int w,int h,int x,int y,int dx,int dy){
    int qx=x+dx,qy=y+dy;
    if(qx<0||qy<0||qx+7>=w||qy+6>=h)return 0x3fffffff;
    unsigned norm=0,cross=0;
    #pragma unroll
    for(int r=0;r<4;++r){
        unsigned b0=packed4(prev,pitch,qx,qy+r*2),b1=packed4(prev,pitch,qx+4,qy+r*2);
        norm=__dp4a(b0,b0,norm);norm=__dp4a(b1,b1,norm);
        cross=__dp4a(a.a[r*2],b0,cross);cross=__dp4a(a.a[r*2+1],b1,cross);
    }
    // Sum of squared differences; tiny tie-breaker favors smaller vectors.
    return int(a.norm+norm)-2*int(cross)+2*(abs(dx)+abs(dy));
}
__device__ __forceinline__ float refine(int left,int center,int right){
    if(left>=0x3fffffff||right>=0x3fffffff)return 0.f;
    const float denom=float(left)+float(right)-2.f*float(center);
    return denom>1.f?fminf(.5f,fmaxf(-.5f,.5f*float(left-right)/denom)):0.f;
}
__global__ void motion(const unsigned* cur,const unsigned* prev,int pitch,int w,int h,int2* out,int fw,int fh){
    const int gx=blockIdx.x*blockDim.x+threadIdx.x,gy=blockIdx.y*blockDim.y+threadIdx.y;
    if(gx>=fw||gy>=fh)return;
    // 8-pixel output grid, half-resolution 8x7 support patch.
    const int x=max(0,min(w-8,gx*4-2)),y=max(0,min(h-7,gy*4-1));
    const Patch a=loadPatch(cur,pitch,x,y);
    int bx=0,by=0,best=score(a,prev,pitch,w,h,x,y,0,0),zero=best;
    // Search +/-32 full-res pixels at coarse spacing, then refine to half-res.
    for(int dy=-16;dy<=16;dy+=8)for(int dx=-16;dx<=16;dx+=8){
        if(!dx&&!dy)continue;
        const int s=score(a,prev,pitch,w,h,x,y,dx,dy);
        if(s<best){best=s;bx=dx;by=dy;}
    }
    // Include the small-motion neighborhood before refining the winning basin.
    for(int dy=-2;dy<=2;++dy)for(int dx=-2;dx<=2;++dx){
        if(!dx&&!dy)continue;
        const int s=score(a,prev,pitch,w,h,x,y,dx,dy);
        if(s<best){best=s;bx=dx;by=dy;}
    }
    for(int step=4;step>=1;step>>=1){
        const int cx=bx,cy=by;
        for(int j=-1;j<=1;++j)for(int i=-1;i<=1;++i){
            if(!i&&!j)continue;
            int dx=cx+i*step,dy=cy+j*step;
            const int s=score(a,prev,pitch,w,h,x,y,dx,dy);
            if(s<best){best=s;bx=dx;by=dy;}
        }
    }
    // Avoid unstable vectors when a stationary patch is already a good match.
    if(zero<=32||best*100LL>=zero*98LL){out[gy*fw+gx]=make_int2(0,0);return;}
    float rx=refine(score(a,prev,pitch,w,h,x,y,bx-1,by),best,score(a,prev,pitch,w,h,x,y,bx+1,by));
    float ry=refine(score(a,prev,pitch,w,h,x,y,bx,by-1),best,score(a,prev,pitch,w,h,x,y,bx,by+1));
    out[gy*fw+gx]=make_int2(__float2int_rn((bx+rx)*64.f),__float2int_rn((by+ry)*64.f));
}
cudaError_t fgLaunchDP4a(const unsigned* current,const unsigned* previous,int pitch,int w,int h,int2* output,int fw,int fh,cudaStream_t stream){
    dim3 threads(16,8),blocks((fw+15)/16,(fh+7)/8);
    motion<<<blocks,threads,0,stream>>>(current,previous,pitch,w,h,output,fw,fh);
    return cudaGetLastError();
}
