# Apply the V37 conservative center/edge guards to reconstructed V35.
# Deliberately fail if the expected *original V35* anchors are missing.
from pathlib import Path
import re
p=Path("source/main.cpp")
s=p.read_text(encoding="utf-8-sig")
def rep(a,b):
    global s
    n=s.count(a)
    if n!=1: raise RuntimeError("V35 source mismatch: "+repr(a[:85])+" matches "+str(n))
    s=s.replace(a,b,1)

rep('kAppName[] = L"FG Extrap V35 - Low Latency"','kAppName[] = L"FG V37 - Stable Crosshair + Borders"')
rep('IDC_NVOF_RESOLUTION, IDC_DYNAMIC_TARGET, IDC_CROSSHAIR_RADIUS, IDC_CROSSHAIR_FEATHER\n',
    'IDC_NVOF_RESOLUTION, IDC_DYNAMIC_TARGET, IDC_CROSSHAIR_RADIUS, IDC_CROSSHAIR_FEATHER, IDC_BORDER_PERCENT\n')
rep('CW_USEDEFAULT, CW_USEDEFAULT, 640, 490, nullptr, nullptr, instance_, nullptr);',
    'CW_USEDEFAULT, CW_USEDEFAULT, 640, 549, nullptr, nullptr, instance_, nullptr);')
rep('UINT crosshairRadius_{12};\n    UINT crosshairFeather_{8};\n    UINT ofaPercent_{100};',
    'UINT crosshairRadius_{24};\n    UINT crosshairFeather_{8};\n    UINT borderPercent_{10}; // Symmetric hard edge protection.\n    UINT ofaPercent_{10};')
rep('SendMessageW(ofRes,CB_SETCURSEL,0,0);','SendMessageW(ofRes,CB_SETCURSEL,2,0);')
start=s.index('    label(L"Crosshair radius (px)",18,203,165,24);')
end=s.index('\n}\n\nBOOL CALLBACK App::EnumWindowsProc',start)
s=s[:start]+r'''    label(L"Crosshair hard center (px)",18,203,174,24);
    HWND radius=CreateWindowW(L"EDIT",L"24",WS_CHILD|WS_VISIBLE|WS_BORDER|ES_NUMBER|ES_CENTER,
        192,199,90,28,control_,reinterpret_cast<HMENU>(IDC_CROSSHAIR_RADIUS),instance_,nullptr);
    SendMessageW(radius,EM_SETLIMITTEXT,3,0);
    label(L"Outer feather (px)",317,203,128,24);
    HWND feather=CreateWindowW(L"EDIT",L"8",WS_CHILD|WS_VISIBLE|WS_BORDER|ES_NUMBER|ES_CENTER,
        452,199,90,28,control_,reinterpret_cast<HMENU>(IDC_CROSSHAIR_FEATHER),instance_,nullptr);
    SendMessageW(feather,EM_SETLIMITTEXT,3,0);
    label(L"Center stays newest REAL frame; feather starts OUTSIDE the hard radius.",18,237,598,23);

    label(L"Protect all 4 borders (%)",18,272,178,24);
    HWND border=CreateWindowW(L"EDIT",L"10",WS_CHILD|WS_VISIBLE|WS_BORDER|ES_NUMBER|ES_CENTER,
        192,267,90,28,control_,reinterpret_cast<HMENU>(IDC_BORDER_PERCENT),instance_,nullptr);
    SendMessageW(border,EM_SETLIMITTEXT,2,0);
    label(L"0 = off   |   10 = recommended",317,272,285,24);
    label(L"Real-frame strips + smooth fade towards extrapolation (no extra render pass).",18,309,598,23);

    CreateWindowW(L"BUTTON",L"Start",WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON,
        18,353,282,39,control_,reinterpret_cast<HMENU>(IDC_START),instance_,nullptr);
    CreateWindowW(L"BUTTON",L"Stop",WS_CHILD|WS_VISIBLE|WS_DISABLED,
        316,353,291,39,control_,reinterpret_cast<HMENU>(IDC_STOP),instance_,nullptr);
    status_=label(L"Idle - choose a game window and press Start.",18,412,580,49);
    label(L"Fullscreen ON | Low latency ON | NvOFA 4x4 FAST | Reflex + Boost ON",18,474,594,22);
    label(L"F11 fullscreen  |  F8 pause  |  F10 stop",18,499,580,22);'''+s[end:]
rep('    lowLatency_ = true; // Max-latency-one and tearing remain enabled.',
    '''    BOOL validBorder=FALSE;
    borderPercent_=GetDlgItemInt(control_,IDC_BORDER_PERCENT,&validBorder,FALSE);
    if(!validBorder || borderPercent_>30) {
        SetWindowTextW(status_,L"Border protection must be 0-30 percent of each edge.");return;
    }
    lowLatency_ = true; // Max-latency-one and tearing remain enabled.''')
rep('IDC_NVOF_RESOLUTION,IDC_DYNAMIC_TARGET,IDC_CROSSHAIR_RADIUS,IDC_CROSSHAIR_FEATHER})',
    'IDC_NVOF_RESOLUTION,IDC_DYNAMIC_TARGET,IDC_CROSSHAIR_RADIUS,IDC_CROSSHAIR_FEATHER,IDC_BORDER_PERCENT})')
rep('IDC_NVOF_RESOLUTION,IDC_CROSSHAIR_RADIUS,IDC_CROSSHAIR_FEATHER})',
    'IDC_NVOF_RESOLUTION,IDC_CROSSHAIR_RADIUS,IDC_CROSSHAIR_FEATHER,IDC_BORDER_PERCENT})')
rep('params[1].Constants.Num32BitValues = 5;','params[1].Constants.Num32BitValues = 6;')
start=s.index('    static const char shader[] =',s.index('bool App::CreatePipeline('))
end=s.index('    ComPtr<ID3DBlob> vs, ps;',start)
s=s[:start]+r'''    // V35 optical-flow extrapolation, but protected center and edges always
    // sample the most recent REAL image, with a fade only OUTSIDE hard zones.
    static const char shader[] = R"HLSL(
Texture2D src : register(t0);
Texture2D<float2> flow : register(t1);
SamplerState samp : register(s0);
cbuffer Predict : register(b0) {
    float fraction; float sourceW; float sourceH; float protectRadius;
    float protectFeather; float borderFraction;
};
struct V { float4 p:SV_Position; float2 uv:TEXCOORD0; };
V VS(uint id:SV_VertexID) {
    V o; float2 p=float2((id==2)?3.0:-1.0,(id==1)?3.0:-1.0);
    o.p=float4(p,0,1);
    o.uv=float2((p.x+1)*0.5,1-(p.y+1)*0.5); return o;
}
float4 PS(V i):SV_Target {
    [branch] if(fraction<=0.0) return src.Sample(samp,i.uv);
    const float2 dimensions=max(float2(sourceW,sourceH),float2(1.0,1.0));
    const float2 uv=saturate(i.uv);
    const float2 centerDelta=(uv-float2(0.5,0.5))*dimensions;
    const float centerDistance=length(centerDelta);
    [branch] if(protectRadius>0.0 && centerDistance<=protectRadius)
        return src.Sample(samp,i.uv);
    const float edgeDistance=min(min(uv.x,1.0-uv.x),min(uv.y,1.0-uv.y));
    [branch] if(borderFraction>0.0 && edgeDistance<=borderFraction)
        return src.Sample(samp,i.uv);
    float centerWeight=1.0;
    if(protectRadius>0.0 && protectFeather>0.0)
        centerWeight=smoothstep(protectRadius,protectRadius+protectFeather,centerDistance);
    float edgeWeight=1.0;
    if(borderFraction>0.0)
        edgeWeight=smoothstep(borderFraction,borderFraction+0.02,edgeDistance);
    float weight=centerWeight*edgeWeight;
    [branch] if(weight<=0.00001) return src.Sample(samp,i.uv);
    float2 v=clamp(flow.SampleLevel(samp,uv,0),-128.0,128.0);
    float2 predictedUV=saturate(uv+v*(fraction*weight/dimensions));
    return src.Sample(samp,predictedUV);
}
)HLSL";
'''+s[end:]
rep('float params[]={0.0f,float(d.sourceWidth),float(d.sourceHeight),float(crosshairRadius_),float(crosshairFeather_)};\n    d.list->SetGraphicsRoot32BitConstants(1, 5, params, 0);',
    'float params[]={0.0f,float(d.sourceWidth),float(d.sourceHeight),float(crosshairRadius_),float(crosshairFeather_),borderPercent_/100.0f};\n    d.list->SetGraphicsRoot32BitConstants(1, 6, params, 0);')
rep('float params[]={fraction,float(d.sourceWidth),float(d.sourceHeight),float(crosshairRadius_),float(crosshairFeather_)};\n    d.list->SetGraphicsRoot32BitConstants(1,5,params,0);',
    'float params[]={fraction,float(d.sourceWidth),float(d.sourceHeight),float(crosshairRadius_),float(crosshairFeather_),borderPercent_/100.0f};\n    d.list->SetGraphicsRoot32BitConstants(1,6,params,0);')
assert s.count('IDC_BORDER_PERCENT')>=5
assert 'Num32BitValues = 5' not in s
assert 'SetGraphicsRoot32BitConstants(1,5' not in s
assert 'if(protectRadius>0.0 && centerDistance<=protectRadius)' in s
assert 'if(borderFraction>0.0 && edgeDistance<=borderFraction)' in s
p.write_text(s,encoding="utf-8")
print("V37 guards patched into original V35 DXGI/WGC and NvOFA predictor; bytes:",len(s))
