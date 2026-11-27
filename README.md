# FG CUDA Safe Address Fix

Repair/isolation build for CUDA_ERROR_ILLEGAL_ADDRESS reported at capture event publication in Ultra Pipeline. Based on the Deadline + Texture branch. Retains NvFBC CUDA capture, D3D11 flip-model presentation, UI, Off/x2-x6, fractional Dynamic 320 clock and NvOFA 4x4 FAST cost OFF.

The experimental imported KMT texture/surface, external fence handoff, overlapping luma stream and confidence cache are not used. Native capture and NvOFA vectors use original bounded software loads instead of CUDA texture objects. Registered D3D11 output buffers are mapped/unmapped explicitly. No CPU frame readback.

Normal mode uses the existing asynchronous pipeline. For optional troubleshooting, start from PowerShell with $env:FG_CUDA_DIAGNOSTIC='1'; .\FG.exe . It adds stream synchronization after capture, luma, OFA, scene gate and rendering to improve error attribution, but will increase latency and still cannot guarantee attribution across concurrent streams. Close FG and remove the environment variable for normal performance. A CUDA illegal address can poison the process context; restart FG after the error. This is a candidate repair, not a verified runtime fix.

Build.cmd --blackwell builds CUDA PTX + SM120; Test.cmd runs CPU tests. AppVeyor publishes source and Windows x64 ZIPs.
