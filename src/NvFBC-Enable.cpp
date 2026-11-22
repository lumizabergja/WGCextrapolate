// Standalone explicit opt-in NVIDIA NvFBC_Enable legacy API test.
// Does not alter V15, DLL files, or registry. This driver API may reset the display.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <nvFBC.h>
#include <cstdio>
#include <cstring>

using StatusFn = NVFBCRESULT (NVFBCAPI*)(NvFBCStatusEx*);
using EnableFn = NVFBCRESULT (NVFBCAPI*)(NVFBC_STATE);

static FILE* logFile=nullptr;
static void Write(const char* message) {
    puts(message);
    if(logFile){fputs(message,logFile);fputc('\n',logFile);fflush(logFile);}
}
static void PrintResult(const char* name,NVFBCRESULT r) {
    char s[200]{};
    sprintf_s(s,"%s: %ld (0x%08lX)",name,(long)r,
              (unsigned long)(unsigned int)r);
    Write(s);
}
static void ReadStatus(StatusFn status,const char* label) {
    NvFBCStatusEx st{};
    st.dwVersion=NVFBC_STATUS_VER;
    NVFBCRESULT ret=status(&st);
    PrintResult(label,ret);
    if(ret==NVFBC_SUCCESS) {
        char line[256]{};
        sprintf_s(line,"CapturePossible=%u Capturing=%u NvFBCVersion=0x%08lX",
                  st.bIsCapturePossible?1:0,st.bCurrentlyCapturing?1:0,
                  (unsigned long)st.dwNvFBCVersion);
        Write(line);
    }
}
static bool IsElevated() {
    HANDLE token{};
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))return false;
    TOKEN_ELEVATION elevation{};
    DWORD returned=0;
    bool ok=GetTokenInformation(token,TokenElevation,&elevation,sizeof(elevation),&returned)!=FALSE;
    CloseHandle(token);
    return ok&&elevation.TokenIsElevated!=0;
}
int main() {
    fopen_s(&logFile,"NvFBC-Enable-Result.txt","w");
    Write("V15 Nvidia NvFBC ENABLE API TEST - explicit opt-in only");
    Write("This EXE will NOT patch or replace NvFBC DLLs or change V15.");
    Write("WARNING: NVIDIA's legacy NvFBC_Enable API may reset the graphics driver.");
    Write("Close all games, streaming software, and graphics workloads first.");
    if(!IsElevated()) {
        Write("ERROR: Not elevated. Right-click EXE > Run as administrator.");
        if(logFile)fclose(logFile);getchar();return 1;
    }
    HMODULE dll=LoadLibraryExW(L"NvFBC64.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!dll) {
        char line[128]{};
        sprintf_s(line,"Could not load NvFBC64.dll: Windows error %lu",GetLastError());
        Write(line);if(logFile)fclose(logFile);getchar();return 2;
    }
    auto status=reinterpret_cast<StatusFn>(GetProcAddress(dll,"NvFBC_GetStatusEx"));
    auto enable=reinterpret_cast<EnableFn>(GetProcAddress(dll,"NvFBC_Enable"));
    if(!status || !enable) {
        Write("Missing NvFBC_GetStatusEx or NvFBC_Enable export.");
        FreeLibrary(dll);if(logFile)fclose(logFile);getchar();return 3;
    }
    ReadStatus(status,"Status before");
    Write("Type exactly ENABLE and press Enter to issue NvFBC_Enable(ENABLE).");
    Write("Press Enter alone or type anything else to cancel.");
    char answer[32]{};
    if(!fgets(answer,sizeof(answer),stdin) ||
       (strcmp(answer,"ENABLE\n")!=0 && strcmp(answer,"ENABLE\r\n")!=0)) {
        Write("Cancelled; did not call NvFBC_Enable.");
    } else {
        NVFBCRESULT result=enable(NVFBC_STATE_ENABLE);
        PrintResult("NvFBC_Enable result",result);
        ReadStatus(status,"Status after");
        Write("If result=0 and CapturePossible=1, restart V15.");
        Write("If CapturePossible=0 or result is an error, capture is still blocked.");
    }
    FreeLibrary(dll);
    if(logFile)fclose(logFile);
    puts("Press Enter to close...");
    getchar();
    return 0;
}
