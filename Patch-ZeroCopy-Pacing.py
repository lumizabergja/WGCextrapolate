from pathlib import Path

root = Path("zero-copy")
def replace(name, pairs):
    path = root / "src" / name
    s = path.read_text(encoding="utf-8")
    for old,new in pairs:
        if s.count(old) != 1:
            raise RuntimeError(f"{name}: expected one match for {old[:100]!r}, got {s.count(old)}")
        s = s.replace(old,new,1)
    path.write_text(s,encoding="utf-8")

replace("zero_present.h",[
("std::array<Slot,2> slots;", "std::array<Slot,3> slots;"),
("HANDLE lost{},compositionHandle{};", """HANDLE lost{},compositionHandle{},statisticsEvent{};
    unsigned flipCount{},compositionCount{},skipCount{};
    unsigned refreshHint{};
    void SetRefreshHint(unsigned hz) {
        if (!hz || hz==refreshHint) return;
        refreshHint=hz;
        SystemInterruptTime duration{}, tolerance{};
        duration.value=10000000ull/hz;
        tolerance.value=duration.value/20;
        // A hint only; unsupported modes must not stop presentation.
        manager->SetPreferredPresentDuration(duration,tolerance);
    }
    void PollStatistics() {
        if(!statisticsEvent) return;
        for(unsigned n=0;n<256 && WaitForSingleObject(statisticsEvent,0)==WAIT_OBJECT_0;++n){
            ComPtr<IPresentStatistics> stat;
            if(FAILED(manager->GetNextPresentStatistics(&stat)) || !stat)break;
            switch(stat->GetKind()){
            case PresentStatisticsKind_IndependentFlipFrame: ++flipCount;break;
            case PresentStatisticsKind_CompositionFrame: ++compositionCount;break;
            case PresentStatisticsKind_PresentStatus:{
                ComPtr<IPresentStatusPresentStatistics> status;
                if(SUCCEEDED(stat.As(&status)) && status->GetPresentStatus()==PresentStatus_Skipped)++skipCount;
                break;
            }
            default:break;
            }
        }
    }"""),
('Check(manager->GetLostEvent(&lost),"Presentation lost event");', '''Check(manager->GetLostEvent(&lost),"Presentation lost event");
        manager->EnablePresentStatisticsKind(PresentStatisticsKind_IndependentFlipFrame,TRUE);
        manager->EnablePresentStatisticsKind(PresentStatisticsKind_CompositionFrame,TRUE);
        manager->EnablePresentStatisticsKind(PresentStatisticsKind_PresentStatus,TRUE);
        manager->GetPresentStatisticsAvailableEvent(&statisticsEvent);'''),
("if(lost)CloseHandle(lost);","if(statisticsEvent)CloseHandle(statisticsEvent);\n        if(lost)CloseHandle(lost);")
])
replace("cuda_pipeline.h",[
("std::array<OutputBuffer,2> outputs{};", """#ifdef FG_ZERO_COPY
    std::array<OutputBuffer,3> outputs{};
#else
    std::array<OutputBuffer,2> outputs{};
#endif""")
])
replace("main.cpp",[
("AsyncFlow flow(gpu,cuda,capture.context);", """AsyncFlow flow(gpu,cuda,capture.context);
#ifdef FG_ZERO_COPY
        dx.zero.SetRefreshHint(monitorHz);
        unsigned diagReal=0,diagGenerated=0,diagFlow=0,diagCapture=0,diagWait=0;
        ULONGLONG diagStart=GetTickCount64(),diagPoll=0;
#endif"""),
("if(flow.Poll()) SetEvent(mailbox.freeEvent);", """if(flow.Poll()) {
                SetEvent(mailbox.freeEvent);
#ifdef FG_ZERO_COPY
                ++diagFlow;
#endif
            }"""),
("if(!focused) {\n                wasFocused=false;", """if(!focused) {
#ifdef FG_ZERO_COPY
                diagStart=GetTickCount64();
                diagReal=diagGenerated=diagFlow=diagCapture=diagWait=0;
                dx.zero.PollStatistics();
                dx.zero.flipCount=dx.zero.compositionCount=dx.zero.skipCount=0;
#endif
                wasFocused=false;"""),
("if(next>=0){\n                const auto& incoming", """if(next>=0){
#ifdef FG_ZERO_COPY
                ++diagCapture;
#endif
                const auto& incoming"""),
("if(current>=0){\n                QueryPerformanceCounter(&now);", """if(current>=0){
#ifdef FG_ZERO_COPY
                const ULONGLONG diagNow=GetTickCount64();
                if(diagNow-diagPoll>=20){
                    dx.zero.PollStatistics();diagPoll=diagNow;
                }
                if(diagNow-diagStart>=1000){
                    const double seconds=(diagNow-diagStart)/1000.0;
                    auto rate=[&](unsigned n){return unsigned(n/seconds+0.5);};
                    wchar_t caption[256]{};
                    std::swprintf(caption,256,L"FG | C%u R%u G%u OF%u IF%u DWM%u Skip%u Wait%u Hz%u",
                        rate(diagCapture),rate(diagReal),rate(diagGenerated),rate(diagFlow),
                        rate(dx.zero.flipCount),rate(dx.zero.compositionCount),rate(dx.zero.skipCount),diagWait,monitorHz);
                    Status(std::wstring(L"DIAG:")+caption);
                    diagReal=diagGenerated=diagFlow=diagCapture=diagWait=0;
                    dx.zero.flipCount=dx.zero.compositionCount=dx.zero.skipCount=0;
                    diagStart=diagNow;
                }
#endif
                QueryPerformanceCounter(&now);"""),
("HANDLE wakes[]={dx.zero.lost,dx.zero.slots[0].available,dx.zero.slots[1].available,mailbox.readyEvent};\n                        DWORD wake=MsgWaitForMultipleObjectsEx(4,wakes,4,QS_ALLINPUT,MWMO_INPUTAVAILABLE);", """++diagWait;
                        HANDLE wakes[]={dx.zero.lost,dx.zero.slots[0].available,dx.zero.slots[1].available,dx.zero.slots[2].available,mailbox.readyEvent};
                        DWORD wake=MsgWaitForMultipleObjectsEx(5,wakes,2,QS_ALLINPUT,MWMO_INPUTAVAILABLE);"""),
("realFramePending=false;\n                    slotGranted=false;", """#ifdef FG_ZERO_COPY
                    if(prediction>0.f)++diagGenerated;else ++diagReal;
#endif
                    realFramePending=false;
                    slotGranted=false;"""),
("if(s.rfind(L\"FG error:\",0)==0||s.rfind(L\"Source error:\",0)==0)", """if(s.rfind(L"DIAG:",0)==0){SetWindowTextW(hwnd,s.c_str()+5);return 0;}
        if(s.rfind(L"FG error:",0)==0||s.rfind(L"Source error:",0)==0)""")
])
(root/"README.md").open("a",encoding="utf-8").write("""
Zero-copy pacing diagnostic: three independently reusable displayable surfaces;
no output copy fallback. Preferred duration uses detected monitor refresh.
The launcher title shows the last active one-second sample:
C=accepted capture, R=real submissions, G=prediction submissions, OF=flow
completions, IF=independent flip reports, DWM=composition reports, Skip=skipped
reports, Wait=buffer wait attempts, Hz=detected monitor refresh.
G counts submissions, not unique pixels or displayed generated frames.
Alt+Tab freezes the last sample. No disk logs or image readback.
""")
print("Zero-copy triple-buffer pacing and diagnostic patch applied.")

# V3: explicit surface geometry, tagged display feedback, optional visual probe.
replace("zero_present.h",[
('Check(surface->SetColorSpace(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709),"Composition color space");', '''Check(surface->SetColorSpace(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709),"Composition color space");
        surface->SetTag(0x4647);
        PresentationTransform transform{};
        transform.M11=1.0f;transform.M22=1.0f;
        Check(surface->SetTransform(&transform),"Set identity presentation transform");'''),
('auto& s=slots.at(index);', '''auto& s=slots.at(index);
        if(index==0){
            RECT sourceRect{0,0,LONG(width),LONG(height)};
            Check(surface->SetSourceRect(&sourceRect),"Set full native presentation rectangle");
        }'''),
('Check(manager->Present(),"Present same texture allocation");', '''SystemInterruptTime now{};
        QueryInterruptTimePrecise(&now.value);
        Check(manager->SetTargetTime(now),"Set immediate presentation target");
        Check(manager->Present(),"Present same texture allocation");'''),
('manager->EnablePresentStatisticsKind(PresentStatisticsKind_IndependentFlipFrame,TRUE);', 'Check(manager->EnablePresentStatisticsKind(PresentStatisticsKind_IndependentFlipFrame,TRUE),"Enable independent flip statistics");'),
('manager->EnablePresentStatisticsKind(PresentStatisticsKind_CompositionFrame,TRUE);', 'Check(manager->EnablePresentStatisticsKind(PresentStatisticsKind_CompositionFrame,TRUE),"Enable composition statistics");'),
('manager->EnablePresentStatisticsKind(PresentStatisticsKind_PresentStatus,TRUE);', 'Check(manager->EnablePresentStatisticsKind(PresentStatisticsKind_PresentStatus,TRUE),"Enable present status statistics");'),
('manager->GetPresentStatisticsAvailableEvent(&statisticsEvent);','Check(manager->GetPresentStatisticsAvailableEvent(&statisticsEvent),"Get statistics event");')
])
replace("cuda_pipeline.h",[
('void* args[]={&images,&target,&horizon,&radius,&feather,&cut,&out.zeroSurface};','''// Off by default; opt-in only for a short visibility test.
        static const unsigned probe=[]{
            wchar_t value[8]{};
            return GetEnvironmentVariableW(L"FG_VISUAL_PROBE",value,8)==1 && value[0]==L'1' ? 1u:0u;
        }();
        unsigned marker=probe;
        void* args[]={&images,&target,&horizon,&radius,&feather,&cut,&out.zeroSurface,&marker};''')
])
replace("predict.cu",[
('float horizon,float radius,float feather,const unsigned* cut,cudaSurfaceObject_t surface) {',
 'float horizon,float radius,float feather,const unsigned* cut,cudaSurfaceObject_t surface,unsigned marker) {'),
('    surf2Dwrite(make_uchar4(result&255u,(result>>8)&255u,(result>>16)&255u,result>>24),surface,x*4,y);',
'''    // Diagnostic only: small corner marker, normal rendering unchanged when off.
    // Green: real. Magenta: generated. Yellow: scene gate rejected prediction.
    if(marker && x>=16 && x<96 && y>=16 && y<64) {
        if(horizon<=0.00001f) result=0xff00ff00u;
        else if(*cut) result=0xffffff00u;
        else {
            unsigned phase=min(3u,unsigned(horizon*4.0f));
            result=x<16+(phase+1)*20 ? 0xffff00ffu : 0xff300030u;
        }
    }
    surf2Dwrite(make_uchar4(result&255u,(result>>8)&255u,(result>>16)&255u,result>>24),surface,x*4,y);''')
])
(root/"README.md").open("a",encoding="utf-8").write("""
V3: explicit full-resolution source rectangle, identity transform, content tag,
current interrupt-time presentation target, checked statistics registration.
For an optional optical visibility test launch with FG_VISUAL_PROBE=1:
green corner = real submission, magenta phase bar = generated submission,
yellow corner = scene gate has disabled extrapolation for the current pair.
The marker is off by default and is drawn by the same CUDA surface kernel,
not another overlay or GPU copy. Record the monitor using a high-speed camera.
""")
print("V3 native geometry and optional visibility probe applied.")
