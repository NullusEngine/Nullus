#include "Core/AssetFileWatcher.h"

#include <array>
#include <chrono>
#include <thread>

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
    std::thread worker;

#if defined(_WIN32)
    HANDLE directoryHandle = INVALID_HANDLE_VALUE;
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
        if (!running.exchange(false))
            return;

#if defined(_WIN32)
        if (directoryHandle != INVALID_HANDLE_VALUE)
            CancelIoEx(directoryHandle, nullptr);
#elif defined(__APPLE__)
        if (runLoop)
            CFRunLoopStop(runLoop);
#endif

        if (worker.joinable())
            worker.join();

#if defined(_WIN32)
        if (directoryHandle != INVALID_HANDLE_VALUE)
        {
            CloseHandle(directoryHandle);
            directoryHandle = INVALID_HANDLE_VALUE;
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
    bool StartWindows()
    {
        directoryHandle = CreateFileW(
            ToWidePath(root).c_str(),
            FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS,
            nullptr);
        if (directoryHandle == INVALID_HANDLE_VALUE)
        {
            running = false;
            return false;
        }

        worker = std::thread([this]
        {
            std::array<char, 64 * 1024> buffer{};
            while (running)
            {
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
                    &bytesReturned,
                    nullptr,
                    nullptr);

                if (!running)
                    break;
                if (ok && bytesReturned > 0)
                    changed = true;
            }
        });
        return true;
    }
#endif

#if defined(__linux__)
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

                changed = true;
                size_t offset = 0;
                while (offset < static_cast<size_t>(bytesRead))
                {
                    const auto* event = reinterpret_cast<const inotify_event*>(buffer.data() + offset);
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
        size_t,
        void*,
        const FSEventStreamEventFlags[],
        const FSEventStreamEventId[])
    {
        static_cast<Impl*>(clientCallBackInfo)->changed = true;
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
            FSEventStreamStart(stream);
            CFRunLoopRun();
            FSEventStreamStop(stream);
        });
        return true;
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
    return m_impl->changed.exchange(false);
}

bool AssetFileWatcher::IsRunning() const
{
    return m_impl->running;
}
} // namespace NLS::Editor::Core
