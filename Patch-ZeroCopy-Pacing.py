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
