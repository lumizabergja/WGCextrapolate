// Exercise the production shader on the Windows D3D11 software renderer.
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>
#include "../src/extrapolation_shader.h"
using Microsoft::WRL::ComPtr;
constexpr UINT width=128,height=96;
using Pixel=std::array<float,4>;

void check(HRESULT hr,const char* operation){
    if(FAILED(hr)){std::fprintf(stderr,"%s: %08lx\n",operation,static_cast<unsigned long>(hr));throw std::runtime_error(operation);}
}
ComPtr<ID3DBlob> compile(const char* entry,const char* profile){
    ComPtr<ID3DBlob> code,error;
    auto hr=D3DCompile(extrapolationShader,sizeof(extrapolationShader)-1,"FG",nullptr,nullptr,
        entry,profile,D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error);
    if(FAILED(hr)&&error)std::fprintf(stderr,"%s\n",static_cast<const char*>(error->GetBufferPointer()));
    check(hr,entry);return code;
}
void expectNear(float value,float expected,float epsilon,const char* test){
    if(!(std::abs(value-expected)<=epsilon)){
        std::fprintf(stderr,"%s: got %.7f, expected %.7f +/- %.7f\n",test,value,expected,epsilon);
        throw std::runtime_error(test);
    }
}
int main(){
    try{
        ComPtr<ID3D11Device> dev;
        ComPtr<ID3D11DeviceContext> ctx;
        check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,
            D3D11_SDK_VERSION,&dev,nullptr,&ctx),"D3D11 WARP");
        auto texture=[&](UINT w,UINT h,DXGI_FORMAT format,UINT bind,const void* pixels,UINT pitch){
            D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=1;
            d.Format=format;d.SampleDesc.Count=1;d.BindFlags=bind;
            d.Usage=bind?D3D11_USAGE_DEFAULT:D3D11_USAGE_STAGING;d.CPUAccessFlags=bind?0:D3D11_CPU_ACCESS_READ;
            D3D11_SUBRESOURCE_DATA data{pixels,pitch,0};ComPtr<ID3D11Texture2D> tex;
            check(dev->CreateTexture2D(&d,pixels?&data:nullptr,&tex),"texture");return tex;
        };
        std::vector<Pixel> ramp(width*height);
        for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x)ramp[y*width+x]={x/256.f,y/256.f,0.f,1.f};
        std::vector<std::array<short,2>> vectors(width/4*height/4,{128,0}); // four source pixels right
        auto image=texture(width,height,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D11_BIND_SHADER_RESOURCE,ramp.data(),width*sizeof(Pixel));
        auto flow=texture(width/4,height/4,DXGI_FORMAT_R16G16_SINT,D3D11_BIND_SHADER_RESOURCE,vectors.data(),width/4*4);
        auto output=texture(width,height,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D11_BIND_RENDER_TARGET,nullptr,0);
        auto readback=texture(width,height,DXGI_FORMAT_R32G32B32A32_FLOAT,0,nullptr,0);
        ComPtr<ID3D11ShaderResourceView> colors,motion;
        check(dev->CreateShaderResourceView(image.Get(),nullptr,&colors),"color view");
        check(dev->CreateShaderResourceView(flow.Get(),nullptr,&motion),"vector view");
        ComPtr<ID3D11RenderTargetView> target;
        check(dev->CreateRenderTargetView(output.Get(),nullptr,&target),"output view");
        auto v=compile("VS","vs_5_0"),p=compile("PS","ps_5_0");
        ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;
        check(dev->CreateVertexShader(v->GetBufferPointer(),v->GetBufferSize(),nullptr,&vs),"VS");
        check(dev->CreatePixelShader(p->GetBufferPointer(),p->GetBufferSize(),nullptr,&ps),"PS");
        D3D11_BUFFER_DESC bd{};bd.ByteWidth=sizeof(ExtrapolationParams);bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        ComPtr<ID3D11Buffer> cb;check(dev->CreateBuffer(&bd,nullptr,&cb),"params");
        D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;
        ComPtr<ID3D11SamplerState> sampler;check(dev->CreateSamplerState(&sd,&sampler),"sampler");
        D3D11_VIEWPORT viewport{0,0,float(width),float(height),0,1};
        ctx->RSSetViewports(1,&viewport);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs.Get(),nullptr,0);ctx->PSSetShader(ps.Get(),nullptr,0);
        ctx->VSSetConstantBuffers(0,1,cb.GetAddressOf());ctx->PSSetConstantBuffers(0,1,cb.GetAddressOf());
        ctx->VSSetSamplers(0,1,sampler.GetAddressOf());ctx->PSSetSamplers(0,1,sampler.GetAddressOf());
        ExtrapolationParams params{0,1.f/width,1.f/height,.12f,width/4.f,height/4.f,4,0,0,0,width,height,{}};
        auto draw=[&](UINT x,UINT y){
            ctx->UpdateSubresource(cb.Get(),0,nullptr,&params,0,0);
            ID3D11ShaderResourceView* views[]={colors.Get(),params.horizon?motion.Get():nullptr,colors.Get()};
            ctx->VSSetShaderResources(0,3,views);ctx->PSSetShaderResources(0,3,views);
            ctx->OMSetRenderTargets(1,target.GetAddressOf(),nullptr);ctx->Draw(3,0);
            ctx->OMSetRenderTargets(0,nullptr,nullptr);
            ctx->CopyResource(readback.Get(),output.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};check(ctx->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped),"read pixel");
            const auto row=static_cast<const unsigned char*>(mapped.pData)+size_t(y)*mapped.RowPitch;
            auto pixel=reinterpret_cast<const Pixel*>(row)[x];
            ctx->Unmap(readback.Get(),0);return pixel;
        };
        constexpr UINT x=64,y=48;
        constexpr float epsilon=.08f/256;
        auto base=draw(x,y);
        expectNear(base[0],x/256.f,epsilon,"unshifted image");
        // At image center the focal length is 64 px. Choose rotations that
        // map to exactly four and two source pixels of horizontal camera motion.
        auto yaw=[](double pixels){
            const double angle=std::atan(pixels/64.);
            return cs2::Rotation{float(std::sin(angle)),float(std::cos(angle)),0,1};
        };
        params.camera.live=yaw(2);
        expectNear(draw(x,y)[0],(x+2)/256.f,epsilon,"camera-only first response with no motion texture");
        params.horizon=.5f;params.camera.previous=yaw(4);
        expectNear(draw(x,y)[0],(x+2)/256.f,epsilon,"steady pan counted once");
        params.camera.live={};
        expectNear(draw(x,y)[0],x/256.f,epsilon,"camera stops while old optical flow points right");
        params.camera.live=yaw(-2);
        expectNear(draw(x,y)[0],(x-2)/256.f,epsilon,"immediate reversal");
        params.camera.previous={};
        expectNear(draw(x,y)[0],x/256.f,epsilon,"independent object flow still contributes");
        params.camera={};
        expectNear(draw(x,y)[0],(x+2)/256.f,epsilon,"optical-flow-only baseline");
        params.horizon=0;params.camera.live=cs2::fromCounts(0,100,8);
        expectNear(draw(x,y)[1],(y+64*std::tan(.033*100*3.141592653589793/180))/256.f,epsilon,"mouse-down pitch");
        params.centerRadiusPx=3;params.centerFeatherPx=2;
        expectNear(draw(x,y)[1],base[1],epsilon,"protected center");
        params.centerRadiusPx=0;params.centerFeatherPx=0;params.camera.live=yaw(4);
        expectNear(draw(width-1,y)[0],(width-1)/256.f,epsilon,"exposed edge uses current image");
        std::puts("D3D11 mouse reprojection, flow composition, center and edge tests passed.");
        return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}
}
