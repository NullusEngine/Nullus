#include "Debug/StackTrace.h"

#include <sstream>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>

#include <array>
#include <condition_variable>
#include <deque>
#include <string_view>
#include <thread>
#include <utility>

extern "C" void NlsDbgHelpInit();
extern "C" void NlsDbgHelpLock();
extern "C" void NlsDbgHelpUnlock();

namespace
{
struct SymbolLock
{
    SymbolLock() { NlsDbgHelpInit(); NlsDbgHelpLock(); }
    ~SymbolLock() { NlsDbgHelpUnlock(); }
};

std::string Utf8(const wchar_t* value)
{
    if (!value || !*value) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), size, nullptr, nullptr);
    result.pop_back();
    return result;
}

class StackResolver final
{
public:
    StackResolver() : m_thread([this] { Run(); }) {}
    ~StackResolver() { Shutdown(); }

    void Submit(const std::shared_ptr<NLS::Debug::NativeStackTrace>& trace)
    {
        std::lock_guard lock(m_mutex);
        if (m_stopping || m_queue.size() >= 2048)
        {
            trace->resolved = NLS::Debug::StackTrace::RawText(trace->frames);
            trace->ready.store(true, std::memory_order_release);
            return;
        }
        m_queue.push_back(trace);
        m_changed.notify_one();
    }

    void Shutdown()
    {
        std::lock_guard shutdownLock(m_shutdownMutex);
        {
            std::lock_guard lock(m_mutex);
            m_stopping = true;
        }
        m_changed.notify_all();
        if (m_thread.joinable()) m_thread.join();
    }

private:
    void Run()
    {
        HANDLE process = nullptr;
        DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &process,
            0, FALSE, DUPLICATE_SAME_ACCESS);
        bool initialized = false;
        if (process)
        {
            std::array<wchar_t, 32768> executable{};
            const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
            std::wstring search(executable.data(), length);
            const auto separator = search.find_last_of(L"\\/");
            if (separator != std::wstring::npos) search.resize(separator);
            SymbolLock lock;
            SymSetOptions(SymGetOptions() | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS |
                SYMOPT_NO_PROMPTS | SYMOPT_FAIL_CRITICAL_ERRORS);
            initialized = SymInitializeW(process, search.empty() ? nullptr : search.c_str(), TRUE) == TRUE;
        }
        for (;;)
        {
            std::shared_ptr<NLS::Debug::NativeStackTrace> trace;
            {
                std::unique_lock lock(m_mutex);
                m_changed.wait(lock, [this] { return m_stopping || !m_queue.empty(); });
                if (m_queue.empty() && m_stopping) break;
                trace = std::move(m_queue.front());
                m_queue.pop_front();
            }
            const auto resolved = initialized ? Resolve(process, trace->frames) : NLS::Debug::StackTrace::RawText(trace->frames);
            {
                std::lock_guard lock(trace->mutex);
                trace->resolved = resolved;
            }
            trace->ready.store(true, std::memory_order_release);
        }
        if (process)
        {
            if (initialized) { SymbolLock lock; SymCleanup(process); }
            CloseHandle(process);
        }
    }

    static std::string Resolve(HANDLE process, const std::vector<uintptr_t>& frames)
    {
        std::ostringstream output;
        SymbolLock lock;
        bool refreshed = false;
        for (const auto frame : frames)
        {
            if (!refreshed && !SymGetModuleBase64(process, static_cast<DWORD64>(frame)))
            {
                SymRefreshModuleList(process);
                refreshed = true;
            }
            output << "0x" << std::hex << frame << std::dec;
            HMODULE module = nullptr;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(frame), &module))
            {
                std::array<wchar_t, 32768> name{};
                if (GetModuleFileNameW(module, name.data(), static_cast<DWORD>(name.size())))
                {
                    std::wstring_view path(name.data());
                    const auto separator = path.find_last_of(L"\\/");
                    const auto filename = separator == std::wstring_view::npos ? path : path.substr(separator + 1);
                    output << "  " << Utf8(std::wstring(filename).c_str());
                }
            }
            alignas(SYMBOL_INFOW) std::array<unsigned char, sizeof(SYMBOL_INFOW) + MAX_SYM_NAME * sizeof(wchar_t)> storage{};
            auto* symbol = reinterpret_cast<SYMBOL_INFOW*>(storage.data());
            symbol->SizeOfStruct = sizeof(SYMBOL_INFOW);
            symbol->MaxNameLen = MAX_SYM_NAME;
            DWORD64 displacement = 0;
            if (SymFromAddrW(process, static_cast<DWORD64>(frame), &displacement, symbol))
                output << "!" << Utf8(symbol->Name);
            IMAGEHLP_LINEW64 line{};
            line.SizeOfStruct = sizeof(line);
            DWORD lineDisplacement = 0;
            if (SymGetLineFromAddrW64(process, static_cast<DWORD64>(frame), &lineDisplacement, &line) && line.FileName)
                output << " (" << Utf8(line.FileName) << ":" << line.LineNumber << ")";
            output << '\n';
        }
        return output.str();
    }

    std::mutex m_mutex;
    std::mutex m_shutdownMutex;
    std::condition_variable m_changed;
    std::deque<std::shared_ptr<NLS::Debug::NativeStackTrace>> m_queue;
    bool m_stopping = false;
    std::thread m_thread;
};

StackResolver& Resolver()
{
    static StackResolver resolver;
    return resolver;
}

std::mutex g_resolverGate;
StackResolver* g_activeResolver = nullptr;
bool g_resolverStopped = false;

void SubmitTrace(const std::shared_ptr<NLS::Debug::NativeStackTrace>& trace)
{
    std::lock_guard lock(g_resolverGate);
    if (g_resolverStopped)
    {
        trace->resolved = NLS::Debug::StackTrace::RawText(trace->frames);
        trace->ready.store(true, std::memory_order_release);
        return;
    }
    g_activeResolver = &Resolver();
    g_activeResolver->Submit(trace);
}

void StopResolver()
{
    StackResolver* resolver = nullptr;
    {
        std::lock_guard lock(g_resolverGate);
        g_resolverStopped = true;
        resolver = g_activeResolver;
    }
    if (resolver) resolver->Shutdown();
}
}
#endif

namespace NLS::Debug
{
std::shared_ptr<NativeStackTrace> StackTrace::Capture()
{
#if defined(_WIN32)
    std::array<void*, 64> captured{};
    const auto count = CaptureStackBackTrace(2, static_cast<DWORD>(captured.size()), captured.data(), nullptr);
    if (!count) return {};
    auto trace = std::make_shared<NativeStackTrace>();
    trace->frames.reserve(count);
    for (USHORT i = 0; i < count; ++i) trace->frames.push_back(reinterpret_cast<uintptr_t>(captured[i]));
    SubmitTrace(trace);
    return trace;
#else
    return {};
#endif
}

std::string StackTrace::RawText(const std::vector<uintptr_t>& frames)
{
    std::ostringstream output;
    for (const auto frame : frames) output << "0x" << std::hex << frame << '\n';
    return output.str();
}

void StackTrace::Shutdown()
{
#if defined(_WIN32)
    StopResolver();
#endif
}
}
