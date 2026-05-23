#include "Core/AssetFileWatcher.h"

#include <array>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <Windows.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <unordered_map>
#elif defined(__APPLE__)
#include <CoreServices/CoreServices.h>
#endif

namespace NLS::Editor::Core
{
namespace
{
constexpr auto kLinuxEventWaitTimeout = std::chrono::milliseconds(100);

#if defined(_WIN32)
std::wstring ToWidePath(const std::filesystem::path& path)
{
    return path.wstring();
}
#endif
} // namespace

struct AssetFileWatcher::Impl
{
    std::filesystem::path root;
    std::atomic_bool changed{false};
    std::atomic_bool running{false};
    std::atomic_bool ready{false};
    std::mutex changedPathsMutex;
    std::vector<std::filesystem::path> changedPaths;
    std::thread worker;

#if defined(_WIN32)
    HANDLE directoryHandle = INVALID_HANDLE_VALUE;
    HANDLE wakeEvent = nullptr;
#elif defined(__linux__)
    int inotifyFd = -1;
    std::unordered_map<int, std::filesystem::path> watches;
#elif defined(__APPLE__)
    CFRunLoopRef runLoop = nullptr;
    FSEventStreamRef stream = nullptr;
#endif

    ~Impl()
    {
        Stop();
    }

    bool Start(const std::filesystem::path& watchRoot)
    {
        Stop();
        if (watchRoot.empty() || !std::filesystem::exists(watchRoot) || !std::filesystem::is_directory(watchRoot))
            return false;

        root = watchRoot;
        changed = false;
        {
            std::lock_guard lock(changedPathsMutex);
            changedPaths.clear();
        }
        ready = false;
        running = true;

#if defined(_WIN32)
        return StartWindows();
#elif defined(__linux__)
        return StartLinux();
#elif defined(__APPLE__)
        return StartMac();
#else
        running = false;
        return false;
#endif
    }

    void Stop()
    {
        const bool wasRunning = running.exchange(false);
        if (!wasRunning && !worker.joinable())
        {
            ready = false;
            return;
        }

#if defined(_WIN32)
        ready = false;
        if (wakeEvent)
            SetEvent(wakeEvent);
        if (directoryHandle != INVALID_HANDLE_VALUE)
            CancelIoEx(directoryHandle, nullptr);
#elif defined(__APPLE__)
        ready = false;
        if (runLoop)
            CFRunLoopStop(runLoop);
#else
        ready = false;
#endif

        if (worker.joinable())
            worker.join();

        ready = false;

#if defined(_WIN32)
        if (directoryHandle != INVALID_HANDLE_VALUE)
        {
            CloseHandle(directoryHandle);
            directoryHandle = INVALID_HANDLE_VALUE;
        }
        if (wakeEvent)
        {
            CloseHandle(wakeEvent);
            wakeEvent = nullptr;
        }
#elif defined(__linux__)
        if (inotifyFd >= 0)
        {
            for (const auto& [watch, path] : watches)
                inotify_rm_watch(inotifyFd, watch);
            watches.clear();
            close(inotifyFd);
            inotifyFd = -1;
        }
#elif defined(__APPLE__)
        if (stream)
        {
            FSEventStreamInvalidate(stream);
            FSEventStreamRelease(stream);
            stream = nullptr;
        }
        runLoop = nullptr;
#endif
    }

#if defined(_WIN32)
    void RecordWindowsChanges(const std::array<char, 64 * 1024>& buffer, DWORD bytesReturned)
    {
        changed = true;

        std::lock_guard lock(changedPathsMutex);
        size_t offset = 0u;
        while (offset < static_cast<size_t>(bytesReturned))
        {
            const auto* notification =
                reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer.data() + offset);
            const auto characterCount =
                notification->FileNameLength / static_cast<DWORD>(sizeof(wchar_t));
            changedPaths.push_back((root / std::wstring(notification->FileName, characterCount)).lexically_normal());

            if (notification->NextEntryOffset == 0u)
                break;
            offset += notification->NextEntryOffset;
        }
    }

    bool StartWindows()
    {
        wakeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!wakeEvent)
        {
            running = false;
            return false;
        }

        directoryHandle = CreateFileW(
            ToWidePath(root).c_str(),
            FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
            nullptr);
        if (directoryHandle == INVALID_HANDLE_VALUE)
        {
            CloseHandle(wakeEvent);
            wakeEvent = nullptr;
            running = false;
            return false;
        }

        worker = std::thread([this]
        {
            std::array<char, 64 * 1024> buffer{};
            OVERLAPPED overlapped{};
            overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!overlapped.hEvent)
            {
                running = false;
                return;
            }

            while (running)
            {
                ResetEvent(overlapped.hEvent);
                DWORD bytesReturned = 0;
                const BOOL ok = ReadDirectoryChangesW(
                    directoryHandle,
                    buffer.data(),
                    static_cast<DWORD>(buffer.size()),
                    TRUE,
                    FILE_NOTIFY_CHANGE_FILE_NAME |
                        FILE_NOTIFY_CHANGE_DIR_NAME |
                        FILE_NOTIFY_CHANGE_SIZE |
                        FILE_NOTIFY_CHANGE_LAST_WRITE |
                        FILE_NOTIFY_CHANGE_CREATION,
                    nullptr,
                    &overlapped,
                    nullptr);
                const DWORD error = ok ? ERROR_SUCCESS : GetLastError();

                if (!ready)
                    ready = ok != FALSE || error == ERROR_IO_PENDING;

                if (!ok && error != ERROR_IO_PENDING)
                    break;

                const HANDLE waitHandles[] = {overlapped.hEvent, wakeEvent};
                const DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);

                if (!running)
                    break;
                if (waitResult != WAIT_OBJECT_0)
                    continue;

                if (!GetOverlappedResult(directoryHandle, &overlapped, &bytesReturned, FALSE))
                    continue;

                if (bytesReturned > 0)
                    RecordWindowsChanges(buffer, bytesReturned);
            }

            CancelIoEx(directoryHandle, &overlapped);
            CloseHandle(overlapped.hEvent);
        });

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (ready)
                return true;
            if (!running)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        Stop();
        return false;
    }
#endif

#if defined(__linux__)
    void RecordLinuxChange(const int watchDescriptor, const char* name)
    {
        changed = true;

        const auto found = watches.find(watchDescriptor);
        if (found == watches.end())
            return;

        std::lock_guard lock(changedPathsMutex);
        const auto relativeName = name ? std::filesystem::path(name) : std::filesystem::path {};
        changedPaths.push_back((found->second / relativeName).lexically_normal());
    }

    void AddLinuxWatchRecursive(const std::filesystem::path& path)
    {
        constexpr uint32_t mask =
            IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_MODIFY |
            IN_CLOSE_WRITE | IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF;

        const int watch = inotify_add_watch(inotifyFd, path.c_str(), mask);
        if (watch >= 0)
            watches[watch] = path;

        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(path, error))
        {
            if (error)
                break;
            if (entry.is_directory(error) && !error)
                AddLinuxWatchRecursive(entry.path());
        }
    }

    bool StartLinux()
    {
        inotifyFd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (inotifyFd < 0)
        {
            running = false;
            return false;
        }

        AddLinuxWatchRecursive(root);
        if (watches.empty())
        {
            running = false;
            close(inotifyFd);
            inotifyFd = -1;
            return false;
        }

        ready = true;
        worker = std::thread([this]
        {
            std::array<char, 64 * 1024> buffer{};
            while (running)
            {
                pollfd fd{inotifyFd, POLLIN, 0};
                const int pollResult = poll(&fd, 1, static_cast<int>(kLinuxEventWaitTimeout.count()));
                if (!running)
                    break;
                if (pollResult <= 0 || !(fd.revents & POLLIN))
                    continue;

                const ssize_t bytesRead = read(inotifyFd, buffer.data(), buffer.size());
                if (bytesRead <= 0)
                    continue;

                size_t offset = 0;
                while (offset < static_cast<size_t>(bytesRead))
                {
                    const auto* event = reinterpret_cast<const inotify_event*>(buffer.data() + offset);
                    RecordLinuxChange(event->wd, event->len > 0 ? event->name : nullptr);
                    if ((event->mask & IN_ISDIR) && (event->mask & (IN_CREATE | IN_MOVED_TO)) && event->len > 0)
                    {
                        if (auto found = watches.find(event->wd); found != watches.end())
                            AddLinuxWatchRecursive(found->second / event->name);
                    }
                    offset += sizeof(inotify_event) + event->len;
                }
            }
        });
        return true;
    }
#endif

#if defined(__APPLE__)
    static void OnMacEvent(
        ConstFSEventStreamRef,
        void* clientCallBackInfo,
        size_t eventCount,
        void* eventPaths,
        const FSEventStreamEventFlags[],
        const FSEventStreamEventId[])
    {
        auto& impl = *static_cast<Impl*>(clientCallBackInfo);
        impl.changed = true;
        std::lock_guard lock(impl.changedPathsMutex);
        auto** paths = static_cast<char**>(eventPaths);
        for (size_t index = 0u; index < eventCount; ++index)
            impl.changedPaths.push_back(std::filesystem::path(paths[index]).lexically_normal());
    }

    bool StartMac()
    {
        worker = std::thread([this]
        {
            CFStringRef path = CFStringCreateWithCString(nullptr, root.string().c_str(), kCFStringEncodingUTF8);
            CFArrayRef paths = CFArrayCreate(nullptr, reinterpret_cast<const void**>(&path), 1, nullptr);
            FSEventStreamContext context{};
            context.info = this;

            stream = FSEventStreamCreate(
                nullptr,
                &Impl::OnMacEvent,
                &context,
                paths,
                kFSEventStreamEventIdSinceNow,
                0.1,
                kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer);

            CFRelease(paths);
            CFRelease(path);

            if (!stream)
            {
                running = false;
                return;
            }

            runLoop = CFRunLoopGetCurrent();
            FSEventStreamScheduleWithRunLoop(stream, runLoop, kCFRunLoopDefaultMode);
            if (!FSEventStreamStart(stream))
            {
                running = false;
                return;
            }

            ready = true;
            CFRunLoopRun();
            FSEventStreamStop(stream);
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (ready)
                return true;
            if (!running)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        Stop();
        return false;
    }
#endif
};

AssetFileWatcher::AssetFileWatcher()
    : m_impl(std::make_unique<Impl>())
{
}

AssetFileWatcher::~AssetFileWatcher() = default;

AssetFileWatcher::AssetFileWatcher(AssetFileWatcher&&) noexcept = default;
AssetFileWatcher& AssetFileWatcher::operator=(AssetFileWatcher&&) noexcept = default;

bool AssetFileWatcher::Start(const std::filesystem::path& root)
{
    return m_impl->Start(root);
}

void AssetFileWatcher::Stop()
{
    m_impl->Stop();
}

bool AssetFileWatcher::ConsumeChanged()
{
    const bool hadChanged = m_impl->changed.exchange(false);
    if (hadChanged)
    {
        std::lock_guard lock(m_impl->changedPathsMutex);
        m_impl->changedPaths.clear();
    }
    return hadChanged;
}

std::vector<std::filesystem::path> AssetFileWatcher::ConsumeChangedPaths()
{
    const bool hadChanged = m_impl->changed.exchange(false);
    std::vector<std::filesystem::path> paths;
    {
        std::lock_guard lock(m_impl->changedPathsMutex);
        paths = std::move(m_impl->changedPaths);
        m_impl->changedPaths.clear();
    }

    if (hadChanged && paths.empty())
        paths.push_back(m_impl->root.lexically_normal());
    return paths;
}

bool AssetFileWatcher::IsRunning() const
{
    return m_impl->running;
}

bool AssetFileWatcher::IsReady() const
{
    return m_impl->ready;
}
} // namespace NLS::Editor::Core
