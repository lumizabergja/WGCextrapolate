// NvFBC legacy Windows read-only diagnostic for V15.
// This program NEVER calls NvFBC_CreateEx or NvFBC_Enable and changes no driver files.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <nvFBC.h>
#include <cstdio>
#include <cwchar>
#include <cstdarg>
#include <string>

static FILE* report = nullptr;
static void Out(const wchar_t* fmt, ...) {
    wchar_t line[4096]{};
    va_list ap;
    va_start(ap, fmt);
    vswprintf_s(line, _countof(line), fmt, ap);
    va_end(ap);
    std::wprintf(L"%ls\n", line);
    if (report) {
        std::fwprintf(report, L"%ls\n", line);
        std::fflush(report);
    }
}
static bool Present(const std::wstring& path) {
    DWORD a=GetFileAttributesW(path.c_str());
    return a!=INVALID_FILE_ATTRIBUTES && !(a&FILE_ATTRIBUTE_DIRECTORY);
}
int wmain() {
    _wfopen_s(&report, L"NvFBC-Diagnostic.txt", L"w, ccs=UTF-8");
    Out(L"V15 NvFBC DRIVER DIAGNOSTIC - READ ONLY");
    Out(L"No changes to driver, registry, NvFBC, or the V15 app.");
    wchar_t sys[MAX_PATH]{};
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring dir(sys);
    std::wstring original=dir+L"\\NvFBC64.dll";
    std::wstring backup=dir+L"\\NvFBC64_.dll";
    Out(L"NvFBC64.dll exists: %ls",Present(original)?L"YES":L"NO");
    Out(L"NvFBC64_.dll backup exists: %ls",Present(backup)?L"YES (wrapper may be installed)":L"NO");
    HKEY registry=nullptr;
    DWORD value=0, size=sizeof(value), type=0;
    LSTATUS reg=RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\nvlddmkm", 0, KEY_READ, &registry);
    if (reg==ERROR_SUCCESS) {
        reg=RegQueryValueExW(registry, L"NVFBCEnable", nullptr, &type, reinterpret_cast<LPBYTE>(&value), &size);
        RegCloseKey(registry);
    }
    if (reg==ERROR_SUCCESS && type==REG_DWORD) Out(L"NVFBCEnable registry: %lu",value);
    else Out(L"NVFBCEnable registry: NOT SET OR UNREADABLE (%ld)",long(reg));
    HMODULE lib=LoadLibraryW(L"NvFBC64.dll");
    if (!lib) {
        Out(L"LoadLibrary(NvFBC64.dll) failed, GetLastError=%lu",GetLastError());
    } else {
        wchar_t loaded[MAX_PATH]{};
        GetModuleFileNameW(lib, loaded, MAX_PATH);
        Out(L"Actual loaded DLL: %ls",loaded);
        auto statusFn=reinterpret_cast<NVFBCRESULT (NVFBCAPI*)(void*)>(
            GetProcAddress(lib,"NvFBC_GetStatusEx"));
        Out(L"NvFBC_GetStatusEx export: %ls",statusFn?L"YES":L"NO");
        Out(L"NvFBC_CreateEx export: %ls",GetProcAddress(lib,"NvFBC_CreateEx")?L"YES":L"NO");
        if (statusFn) {
            NvFBCStatusEx st{};
            st.dwVersion=NVFBC_STATUS_VER;
            NVFBCRESULT result=statusFn(&st);
            Out(L"NvFBC_GetStatusEx result: %ld (0x%08lX)",long(result),static_cast<unsigned long>(static_cast<unsigned int>(result)));
            if (result==NVFBC_SUCCESS) {
                Out(L"bIsCapturePossible: %lu",static_cast<unsigned long>(st.bIsCapturePossible));
                Out(L"bCurrentlyCapturing: %lu",static_cast<unsigned long>(st.bCurrentlyCapturing));
                Out(L"bSupportMultiHead: %lu",static_cast<unsigned long>(st.bSupportMultiHead));
                Out(L"bSupportMultiClient: %lu",static_cast<unsigned long>(st.bSupportMultiClient));
                Out(L"dwNvFBCVersion: 0x%08lX",static_cast<unsigned long>(st.dwNvFBCVersion));
                Out(L"dwAdapterIdx: %lu",static_cast<unsigned long>(st.dwAdapterIdx));
            }
        }
        FreeLibrary(lib);
    }
    Out(L"");
    Out(L"Send NvFBC-Diagnostic.txt to diagnose V15's driver error (-5).");
    if (report) fclose(report);
    std::puts("Press Enter to close...");
    (void)getchar();
    return 0;
}
