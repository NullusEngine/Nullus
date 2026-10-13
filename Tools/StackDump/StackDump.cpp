// Minimal cross-thread native stack dumper.
//
// Built against the Windows SDK dbghelp already installed on this machine so a
// hung test process can be inspected without a separate debugger product.
// Usage: StackDump.exe <pid> [symbolPath] [maxFrames]
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#pragma comment(lib, "dbghelp.lib")

namespace
{
    HANDLE g_process = nullptr;

    BOOL CALLBACK ReadProcessMemoryProc64(
        HANDLE,
        const DWORD64 baseAddress,
        PVOID buffer,
        const DWORD size,
        LPDWORD bytesRead)
    {
        SIZE_T read = 0u;
        const BOOL ok = ReadProcessMemory(
            g_process,
            reinterpret_cast<LPCVOID>(baseAddress),
            buffer,
            size,
            &read);
        if (bytesRead != nullptr)
            *bytesRead = static_cast<DWORD>(read);
        return ok && read == size;
    }

    std::string ModuleFromSymbolInfo(const DWORD64 address, std::string* symbolName)
    {
        IMAGEHLP_MODULE64 moduleInfo {};
        moduleInfo.SizeOfStruct = sizeof(moduleInfo);
        std::string module;
        if (SymGetModuleInfo64(g_process, address, &moduleInfo))
            module = moduleInfo.ModuleName;

        // Symbol name: SYMBOL_INFO is variable length, so reserve room for the
        // maximum undecorated name.
        std::vector<char> storage(sizeof(SYMBOL_INFO) + 2048u, 0);
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage.data());
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 2048u;

        DWORD64 displacement = 0u;
        if (SymFromAddr(g_process, address, &displacement, symbol))
        {
            *symbolName = symbol->Name;
            *symbolName += "+0x";
            char offsetBuffer[32] {};
            _ui64toa_s(displacement, offsetBuffer, sizeof(offsetBuffer), 16);
            *symbolName += offsetBuffer;
        }
        return module;
    }

    void PrintLocation(const DWORD64 address)
    {
        std::string symbolName;
        const std::string module = ModuleFromSymbolInfo(address, &symbolName);

        std::printf("    %s!%s", module.empty() ? "?" : module.c_str(), symbolName.c_str());

        IMAGEHLP_LINE64 line {};
        line.SizeOfStruct = sizeof(line);
        DWORD lineDisplacement = 0u;
        if (SymGetLineFromAddr64(g_process, address, &lineDisplacement, &line))
        {
            std::printf("  [%s @ %lu]", line.FileName, line.LineNumber);
        }
        std::printf("\n");
    }

    void DumpThread(const DWORD threadId, const size_t index, const size_t maxFrames)
    {
        HANDLE thread = OpenThread(
            THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION,
            FALSE,
            threadId);
        if (thread == nullptr)
        {
            std::printf("=== thread index=%zu tid=%lu (unopenable, gle=%lu) ===\n",
                index, threadId, GetLastError());
            return;
        }

        // Suspending keeps the stack coherent while it is walked. A thread that
        // is mid-exit refuses the suspend, which is reported rather than walked.
        const DWORD suspendCount = SuspendThread(thread);
        if (suspendCount == static_cast<DWORD>(-1))
        {
            std::printf("=== thread index=%zu tid=%lu (suspend failed, gle=%lu) ===\n",
                index, threadId, GetLastError());
            CloseHandle(thread);
            return;
        }

        // CONTEXT must be 16-byte aligned for GetThreadContext on x64.
        alignas(16) CONTEXT context {};
        context.ContextFlags = CONTEXT_ALL;
        if (!GetThreadContext(thread, &context))
        {
            std::printf("=== thread index=%zu tid=%lu (GetThreadContext failed, gle=%lu) ===\n",
                index, threadId, GetLastError());
            ResumeThread(thread);
            CloseHandle(thread);
            return;
        }

        std::printf("=== thread index=%zu tid=%lu ===", index, threadId);
        if (suspendCount > 0u)
            std::printf(" (was suspended, count=%lu)", suspendCount);
        std::printf("\n");

        STACKFRAME64 frame {};
        frame.AddrPC.Offset = context.Rip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrStack.Offset = context.Rsp;
        frame.AddrStack.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = context.Rbp;
        frame.AddrFrame.Mode = AddrModeFlat;

        size_t frameIndex = 0u;
        while (frameIndex < maxFrames)
        {
            if (!StackWalk64(
                    IMAGE_FILE_MACHINE_AMD64,
                    g_process,
                    thread,
                    &frame,
                    &context,
                    ReadProcessMemoryProc64,
                    SymFunctionTableAccess64,
                    SymGetModuleBase64,
                    nullptr))
            {
                break;
            }

            if (frame.AddrPC.Offset == 0u)
                break;

            PrintLocation(frame.AddrPC.Offset);
            ++frameIndex;
        }

        if (frameIndex == 0u)
            std::printf("    <no frames walked>\n");

        ResumeThread(thread);
        CloseHandle(thread);
    }
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: StackDump.exe <pid> [symbolPath] [maxFrames]\n");
        return 2;
    }

    const DWORD pid = static_cast<DWORD>(std::strtoul(argv[1], nullptr, 10));
    const char* symbolPath = argc >= 3 ? argv[2] : ".";
    const size_t maxFrames = argc >= 4
        ? static_cast<size_t>(std::strtoul(argv[3], nullptr, 10))
        : 48u;

    g_process = OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_SUSPEND_RESUME,
        FALSE,
        pid);
    if (g_process == nullptr)
    {
        std::printf("OpenProcess(%lu) failed, gle=%lu\n", pid, GetLastError());
        return 1;
    }

    SymSetOptions(
        SYMOPT_CASE_INSENSITIVE | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS |
        SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS);
    if (!SymInitialize(g_process, symbolPath, TRUE))
    {
        std::printf("SymInitialize(%s) failed, gle=%lu\n", symbolPath, GetLastError());
        CloseHandle(g_process);
        return 1;
    }

    THREADENTRY32 entry {};
    entry.dwSize = sizeof(entry);
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0u);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        std::printf("CreateToolhelp32Snapshot failed, gle=%lu\n", GetLastError());
        SymCleanup(g_process);
        CloseHandle(g_process);
        return 1;
    }

    std::vector<DWORD> threadIds;
    if (Thread32First(snapshot, &entry))
    {
        do
        {
            if (entry.th32OwnerProcessID == pid)
                threadIds.push_back(entry.th32ThreadID);
        }
        while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);

    std::printf("pid=%lu threads=%zu maxFrames=%zu\n", pid, threadIds.size(), maxFrames);
    size_t index = 0u;
    for (const DWORD threadId : threadIds)
    {
        DumpThread(threadId, index, maxFrames);
        ++index;
    }

    SymCleanup(g_process);
    CloseHandle(g_process);
    return 0;
}
