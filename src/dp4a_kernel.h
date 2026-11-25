#pragma once
#include <cuda_runtime_api.h>
// Packed 8-bit half-resolution luminance, backward flow (current -> previous).
// Output is signed int2, in full-resolution pixels * 32, matching FG's shader.
cudaError_t fgLaunchDP4a(const unsigned* current,const unsigned* previous,
    int packedWidth,int halfWidth,int halfHeight,int2* output,int flowWidth,int flowHeight,
    cudaStream_t stream);
