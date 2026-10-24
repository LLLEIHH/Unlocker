#include "common/NtApi.h"

namespace nt {
namespace {
PfnNtQuerySystemInformation g_querySystem = nullptr;
PfnNtQueryObject            g_queryObject = nullptr;
PfnNtSuspendProcess         g_suspend     = nullptr;
PfnNtResumeProcess          g_resume      = nullptr;
bool                        g_inited      = false;
}  // namespace

bool Init() {
    if (g_inited) return g_querySystem != nullptr;
    g_inited = true;

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) ntdll = LoadLibraryW(L"ntdll.dll");
    if (!ntdll) return false;

    g_querySystem = reinterpret_cast<PfnNtQuerySystemInformation>(
        reinterpret_cast<void*>(GetProcAddress(ntdll, "NtQuerySystemInformation")));
    g_queryObject = reinterpret_cast<PfnNtQueryObject>(
        reinterpret_cast<void*>(GetProcAddress(ntdll, "NtQueryObject")));
    g_suspend = reinterpret_cast<PfnNtSuspendProcess>(
        reinterpret_cast<void*>(GetProcAddress(ntdll, "NtSuspendProcess")));
    g_resume = reinterpret_cast<PfnNtResumeProcess>(
        reinterpret_cast<void*>(GetProcAddress(ntdll, "NtResumeProcess")));

    return g_querySystem != nullptr && g_queryObject != nullptr;
}

PfnNtQuerySystemInformation QuerySystemInformation() { if (!g_inited) Init(); return g_querySystem; }
PfnNtQueryObject            QueryObject()            { if (!g_inited) Init(); return g_queryObject; }
PfnNtSuspendProcess         SuspendProcess()         { if (!g_inited) Init(); return g_suspend; }
PfnNtResumeProcess          ResumeProcess()          { if (!g_inited) Init(); return g_resume; }

}  // namespace nt
