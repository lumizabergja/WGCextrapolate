# Apply after the original hash-verified reconstruction and all V15 patches.
from pathlib import Path
root=Path(__file__).resolve().parent
fg=root/'fg'
def edit(path,pairs):
    p=fg/path
    s=p.read_text(encoding='utf-8')
    for old,new in pairs:
        if s.count(old)!=1: raise RuntimeError(f'Late-input patch {path}: expected one {old[:120]!r}, got {s.count(old)}')
        s=s.replace(old,new,1)
    p.write_text(s,encoding='utf-8',newline='\n')

(fg/'src'/'late_input.h').write_text(r'''#pragma once
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "camera_math.h"
namespace fglate {
struct Packet { uint64_t stamp{}; double t{},priorT{},olderT{}; int32_t x{},y{},priorX{},priorY{}; };
struct Prediction { double x{},y{},ms{}; };
inline Prediction predict(const Packet& p,double now,double scale,double maxMs){
    if(!(scale>0)||!(maxMs>0)||p.olderT<=0) return {};
    double dt=p.t-p.priorT,dt0=p.priorT-p.olderT,age=now-p.t;
    if(dt<.35||dt>2.5||dt0<.35||dt0>2.5||age<0||age>1.65)return {};
    double x=p.x/dt,y=p.y/dt,px=p.priorX/dt0,py=p.priorY/dt0;
    double a=std::hypot(x,y),b=std::hypot(px,py);
    if(a<.5||b<.5||a>2*b||b>2*a||(x*px+y*py)<.94*a*b)return {};
    double degreesPerMs=a*scale*180./3.141592653589793;
    if(degreesPerMs>1.2)return {}; // do not extrapolate flicks
    double horizon=std::min({maxMs,3.0,std::max(0.,1.8-age)});
    // At most a quarter degree of invented motion; measured input is retained.
    horizon=std::min(horizon,.25/std::max(degreesPerMs,1e-6));
    return {x*horizon,y*horizon,horizon};
}
inline fgcam::Mat3 target(uint64_t from,uint64_t measured,Prediction p,double scale,unsigned valid){
    double x=(double(fgcam::signedDiff(uint32_t(measured),uint32_t(from)))+p.x)*scale;
    double y=(double(fgcam::signedDiff(uint32_t(measured>>32),uint32_t(from>>32)))+p.y)*scale;
    // Radial rather than per-axis limit; tighten when depth support is weak.
    const double limit=(valid>=fgcam::N/8?3.5:2.5)*3.141592653589793/180.;
    double a=std::hypot(x,y);if(a>limit){x*=limit/a;y*=limit/a;}
    return fgcam::rotation(x,y);
}
// One raw-input writer; readers never wait for it. Atomic members avoid C++ data races.
struct History {
    std::atomic<uint64_t> version{0},stamp{0};
    std::atomic<double> t{0},priorT{0},olderT{0};
    std::atomic<int32_t> x{0},y{0},priorX{0},priorY{0};
    void push(uint64_t s,double now,int32_t dx,int32_t dy){
        if(!dx&&!dy)return;
        version.fetch_add(1);
        olderT.store(priorT.load());priorT.store(t.load());t.store(now);
        priorX.store(x.load());priorY.store(y.load());x.store(dx);y.store(dy);
        stamp.store(s);version.fetch_add(1);
    }
    bool snapshot(Packet& out)const{
        for(int i=0;i<2;++i){
            auto before=version.load();if(before&1)continue;
            Packet p{stamp.load(),t.load(),priorT.load(),olderT.load(),x.load(),y.load(),priorX.load(),priorY.load()};
            if(before==version.load()){out=p;return true;}
        }
        return false;
    }
};
}
''',encoding='utf-8',newline='\n')

edit('src/main.cpp',[
 ('#include "camera_math.h"','#include "camera_math.h"\n#include "late_input.h"'),
 ('std::atomic<uint64_t> g_mousePacked{0};', '''std::atomic<uint64_t> g_mousePacked{0};
fglate::History g_lateMouse;
double g_latePredictionMs=1.5;
bool g_estimateWarpDepth=false; // configured once before starting GPU/flow threads
double MouseClockMs(){
    static const double scale=[](){LARGE_INTEGER f{};QueryPerformanceFrequency(&f);return 1000./double(f.QuadPart);}();
    LARGE_INTEGER now{};QueryPerformanceCounter(&now);return double(now.QuadPart)*scale;
}'''),
 ('g_verticalFov=EnvDouble(L"FG_VERTICAL_FOV_DEG",73.7398,20.,150.)*3.141592653589793/180.;', '''g_verticalFov=EnvDouble(L"FG_VERTICAL_FOV_DEG",73.7398,20.,150.)*3.141592653589793/180.;
            g_latePredictionMs=EnvDouble(L"FG_REFLEX2_PREDICT_MS",1.5,0.,3.);
            g_estimateWarpDepth=EnvDouble(L"FG_REFLEX2_DEPTH",0.,0.,1.)>=.5;'''),
 ('g_mousePacked.store(fgcam::addMouse(g_mousePacked.load(std::memory_order_relaxed),raw.data.mouse.lLastX,raw.data.mouse.lLastY),std::memory_order_release);',
 '''const auto packed=fgcam::addMouse(g_mousePacked.load(std::memory_order_relaxed),raw.data.mouse.lLastX,raw.data.mouse.lLastY);
            g_mousePacked.store(packed,std::memory_order_release);
            g_lateMouse.push(packed,MouseClockMs(),raw.data.mouse.lLastX,raw.data.mouse.lLastY);'''),
])
edit('src/cuda_pipeline.h',[
 ('if(!presenter->vk.warpEnabled){','if(!presenter->vk.warpEnabled||!g_estimateWarpDepth){'),
 ('dx.vk.native.Prepare(grid,real.mouseStamp,g_mousePacked.load(std::memory_order_acquire),real.width,real.height,g_verticalFov,g_mouseScale);',
  'dx.vk.native.Prepare(grid,real.mouseStamp,real.width,real.height,g_verticalFov,g_mouseScale);'),
])
edit('src/native_live.h',[
 ('fg_ngx_experiment::Inputs inputs{};',
  '''fg_ngx_experiment::Inputs inputs{};
    uint64_t sourceStamp{};double mouseScale{};
    bool staticGeometry=false,geometryDirty=true;
    void RefreshTarget(){
        fglate::Packet packet{};
        const bool consistent=g_lateMouse.snapshot(packet);
        const auto actual=g_mousePacked.load(std::memory_order_acquire);
        const auto prediction=consistent&&packet.stamp==actual?
            fglate::predict(packet,MouseClockMs(),mouseScale,g_latePredictionMs):fglate::Prediction{};
        inputs.targetView=fgcam::view(fglate::target(sourceStamp,actual,prediction,mouseScale,lastValid));
    }'''),
 ('void Prepare(const fgcam::Grid* g,uint64_t stamp,uint64_t late,unsigned sourceW,unsigned sourceH,double vf,double scale){',
  'void Prepare(const fgcam::Grid* g,uint64_t stamp,unsigned sourceW,unsigned sourceH,double vf,double scale){'),
 ('lastValid=g?g->valid:0;',
  '''sourceStamp=stamp;mouseScale=scale;
        if(g&&g->valid<fgcam::N/20)g=nullptr;
        lastValid=g?g->valid:0;
        geometryDirty=g||!staticGeometry;
        staticGeometry=!g;'''),
 ('else{std::fill_n(d,fgcam::N,fgcam::projectDepth(fgcam::farZ));std::fill_n(mv,fgcam::N*2,0.f);}',
  'else if(geometryDirty){std::fill_n(d,fgcam::N,fgcam::projectDepth(fgcam::farZ));std::fill_n(mv,fgcam::N*2,0.f);}'),
 ('inputs.targetView=fgcam::view(fgcam::motion(stamp,late,scale,.07));','inputs.targetView=inputs.sourceView;'),
 ('    static void Layout(VkCommandBuffer cmd,', '''    static void Between(VkCommandBuffer cmd,void* context){
        Barrier(cmd,nullptr);
        static_cast<NativeWarp*>(context)->RefreshTarget();
    }
    static void Layout(VkCommandBuffer cmd,'''),
 ('Barrier(cmd,nullptr);Upload(cmd,depthGrid,0);Upload(cmd,motionGrid,depthBytes);Barrier(cmd,nullptr);Blit(cmd,depthGrid,depth.image,w,h);Blit(cmd,motionGrid,motion.image,w,h);Barrier(cmd,nullptr);',
  '''if(geometryDirty){
            Barrier(cmd,nullptr);Upload(cmd,depthGrid,0);Upload(cmd,motionGrid,depthBytes);Barrier(cmd,nullptr);
            Blit(cmd,depthGrid,depth.image,w,h);Blit(cmd,motionGrid,motion.image,w,h);Barrier(cmd,nullptr);
            geometryDirty=false;
        }else Barrier(cmd,nullptr);'''),
 ('N(fg_ngx_experiment::record_real(cmd,handle,params,inputs,frameId,true,Barrier,nullptr),',
  '''RefreshTarget();
        N(fg_ngx_experiment::record_real(cmd,handle,params,inputs,frameId,true,Between,this),'''),
 ('imagesReady=false;}','imagesReady=false;staticGeometry=false;geometryDirty=true;}'),
])
edit('native_ngx/latewarp_vk.h',[
 ('    barrier(cmd,context);\n    p->Set("Latewarp.IsRenderedFrame",0u);',
  '''    barrier(cmd,context);
    // Rebind after the callback: this is the last CPU-side pose sample before evaluation.
    p->Set("Latewarp.WorldToViewMatrix",static_cast<void*>(in.targetView.data()));
    p->Set("Latewarp.IsRenderedFrame",0u);'''),
])
(fg/'tests'/'late_input_test.cpp').write_text(r'''#include "late_input.h"
#include <cassert>
#include <cstdio>
int main(){
    constexpr double scale=.033*3.141592653589793/180.;
    fglate::History h;fglate::Packet p;
    h.push(1,10,1,0);h.push(3,11,2,0);h.push(5,12,2,0);
    assert(h.snapshot(p)&&p.stamp==5);
    auto r=fglate::predict(p,12.2,scale,3);
    assert(r.x>0&&r.ms>0&&r.ms<=3&&r.y==0);
    assert(fglate::predict(p,14,scale,3).ms==0);
    assert(fglate::predict(p,12.2,scale,0).ms==0);
    h.push(3,13,-2,0);assert(h.snapshot(p));
    assert(fglate::predict(p,13.1,scale,3).ms==0);
    h.push(1003,14,1000,0);h.push(2003,15,1000,0);assert(h.snapshot(p));
    assert(fglate::predict(p,15.1,scale,3).ms==0);
    auto m=fglate::target(0,0,{},scale,0);assert(std::abs(m[0].x-1)<1e-10);
    m=fglate::target(0,fgcam::addMouse(0,10000,10000),{},scale,0);
    assert(std::abs(m[0].z)<.045&&std::abs(m[1].z)<.045);
    assert(fgcam::signedDiff(1,0xffffffffu)==2);
    std::puts("PASS: late-input snapshots, stale/reversed/flick prediction rejection, angular limit and wrapped counters.");
}
''',encoding='utf-8',newline='\n')
edit('CI-Build.cmd',[('pixel_tests flow_handoff frame_kind camera_math_test','pixel_tests flow_handoff frame_kind camera_math_test late_input_test')])
with (fg/'README.md').open('a',encoding='utf-8') as f:
    f.write('\n## Adaptive Reflex 2 late input (experimental)\nNative NGX remains real-frame-only; latest Raw Input is sampled before each native evaluation, with the target matrix rebound between them. Prediction defaults to a maximum 1.5 ms and is rejected for stale input, direction changes, flicks or inconsistent intervals; additional predicted rotation is capped at 0.25 degrees. Set FG_REFLEX2_PREDICT_MS=0 to disable prediction or up to 3 to experiment. The default fast rotation-only geometry mode publishes optical flow without blocking for CPU depth readback and caches static far-plane depth/zero motion textures. Set FG_REFLEX2_DEPTH=1 before starting FG to restore the optical-flow geometry estimator; low-support estimates are rejected. The radial correction cap is 2.5 degrees with weak/no geometry, 3.5 with stronger geometry. Capture completion still approximates source camera time; no engine depth, game simulation or HUD separation is added. Generated frames, Vulkan sleep/markers, UI and OFF path are unchanged. Final native GPU blit is retained. Windows build and native GPU latency/distortion must be verified on target hardware. Test offline first.\n')
print('Adaptive late input, bounded prediction, optional blocking geometry and cached static geometry applied.')
