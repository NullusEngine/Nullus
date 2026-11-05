#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>

namespace
{
HANDLE GetLock()
{
    // TracyClient may be statically linked into multiple DLLs. A named mutex
    // makes all of those copies coordinate with the engine resolver.
    static HANDLE handle = []
    {
        char name[80]{};
        std::snprintf(name, sizeof(name), "Local\\NullusDbgHelp-%lu", static_cast<unsigned long>(GetCurrentProcessId()));
        return CreateMutexA(nullptr, FALSE, name);
    }();
    return handle;
}
}

extern "C" void NlsDbgHelpInit() { (void)GetLock(); }
extern "C" void NlsDbgHelpLock() { if (const auto handle = GetLock()) WaitForSingleObject(handle, INFINITE); }
extern "C" void NlsDbgHelpUnlock() { if (const auto handle = GetLock()) ReleaseMutex(handle); }
#endif
