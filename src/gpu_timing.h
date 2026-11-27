#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include "scanout_clock.h"
// Small ring of timestamp queries. Never flush or wait for a result.
// Only one in four warped frames is instrumented. Late marks are GPU
// execution intervals, not CPU-to-GPU queue delay or photon measurements.
class GpuTiming {
    using Query=Microsoft::WRL::ComPtr<ID3D11Query>;
    struct Slot {Query disjoint,begin,late,end;bool pending{};};
    std::array<Slot,8> slots{};
    unsigned cursor{},counter{};
    int active=-1;bool supported{};
    ID3D11DeviceContext* ctx{};
public:
    void Init(ID3D11Device* device,ID3D11DeviceContext* context){
        ctx=context;
        for(auto& s:slots){
            D3D11_QUERY_DESC d{D3D11_QUERY_TIMESTAMP_DISJOINT,0},t{D3D11_QUERY_TIMESTAMP,0};
            if(FAILED(device->CreateQuery(&d,&s.disjoint))||FAILED(device->CreateQuery(&t,&s.begin))||
               FAILED(device->CreateQuery(&t,&s.late))||FAILED(device->CreateQuery(&t,&s.end)))return;
        }
        supported=true;
    }
    void Poll(ScanoutClock& clock){
        if(!supported)return;
        for(auto& s:slots){
            if(!s.pending)continue;
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d{};UINT64 b=0,l=0,e=0;
            const UINT flags=D3D11_ASYNC_GETDATA_DONOTFLUSH;
            HRESULT a=ctx->GetData(s.disjoint.Get(),&d,sizeof(d),flags);
            if(a==S_FALSE)continue;
            if(FAILED(a)||d.Disjoint||!d.Frequency){s.pending=false;continue;}
            HRESULT qb=ctx->GetData(s.begin.Get(),&b,sizeof(b),flags),ql=ctx->GetData(s.late.Get(),&l,sizeof(l),flags),qe=ctx->GetData(s.end.Get(),&e,sizeof(e),flags);
            if(FAILED(qb)||FAILED(ql)||FAILED(qe)){s.pending=false;continue;}
            if(qb!=S_OK||ql!=S_OK||qe!=S_OK)continue;
            s.pending=false;
            if(b<=l&&l<=e)clock.Gpu(double(e-b)*1000./double(d.Frequency),double(e-l)*1000./double(d.Frequency));
        }
    }
    void Begin(){
        active=-1;if(!supported||(++counter%4))return;
        for(unsigned n=0;n<slots.size();++n){
            unsigned i=(cursor+n)%unsigned(slots.size());auto& s=slots[i];if(s.pending)continue;
            active=int(i);cursor=(i+1)%unsigned(slots.size());ctx->Begin(s.disjoint.Get());ctx->End(s.begin.Get());return;
        }
    }
    void Late(){if(active>=0)ctx->End(slots[active].late.Get());}
    void End(){if(active<0)return;auto& s=slots[active];ctx->End(s.end.Get());ctx->End(s.disjoint.Get());s.pending=true;active=-1;}
};
