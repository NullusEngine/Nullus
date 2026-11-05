#include "InterprocessFileLock.h"
#include <fstream>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#ifdef __APPLE__
#include <sys/mount.h>
#else
#include <sys/statfs.h>
#endif
#include <unistd.h>
#endif

namespace NLS::Platform::Process
{
struct InterprocessFileLock::Impl
{
#ifdef _WIN32
    HANDLE file = INVALID_HANDLE_VALUE;
    OVERLAPPED overlapped{};
#else
    int file = -1;
#endif
    ~Impl()
    {
#ifdef _WIN32
        if (file != INVALID_HANDLE_VALUE) { UnlockFileEx(file,0,1,0,&overlapped); CloseHandle(file); }
#else
        if (file >= 0) { flock(file,LOCK_UN); close(file); }
#endif
    }
};
InterprocessFileLock::InterprocessFileLock(const std::filesystem::path& path, bool exclusive, std::chrono::milliseconds timeout)
    : m_impl(std::make_unique<Impl>())
{
    std::filesystem::create_directories(path.parent_path());
#ifdef _WIN32
    m_impl->file = CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,
        nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (m_impl->file == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot open package lock: " + path.generic_string());
#else
    m_impl->file = open(path.c_str(),O_CREAT|O_RDWR|O_CLOEXEC,0600);
    if (m_impl->file < 0) throw std::runtime_error("cannot open package lock: " + path.generic_string());
#endif
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;)
    {
#ifdef _WIN32
        if (LockFileEx(m_impl->file,LOCKFILE_FAIL_IMMEDIATELY|(exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0),0,1,0,&m_impl->overlapped)) break;
        const auto error = GetLastError();
        if (error != ERROR_LOCK_VIOLATION && error != ERROR_IO_PENDING) throw std::runtime_error("package lock failed");
#else
        if (flock(m_impl->file,(exclusive ? LOCK_EX : LOCK_SH)|LOCK_NB) == 0) break;
        if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR) throw std::runtime_error("package lock failed");
#endif
        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("package lock timed out: " + path.generic_string());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
InterprocessFileLock::~InterprocessFileLock() = default;
void WriteDurableFile(const std::filesystem::path& path, std::string_view bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path,std::ios::binary|std::ios::trunc); stream.write(bytes.data(),bytes.size()); stream.close();
    if (!stream) throw std::runtime_error("cannot write transaction file: " + path.generic_string());
#ifdef _WIN32
    const auto file = CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot flush transaction file");
    const bool success = FlushFileBuffers(file) != 0; CloseHandle(file);
#else
    const auto file = open(path.c_str(),O_RDONLY|O_CLOEXEC);
    if (file < 0) throw std::runtime_error("cannot flush transaction file");
    const bool success = fsync(file) == 0; close(file);
#endif
    if (!success) throw std::runtime_error("transaction file flush failed");
}
void ReplaceFileAtomically(const std::filesystem::path& source, const std::filesystem::path& destination)
{
#ifdef _WIN32
    if (!MoveFileExW(source.c_str(),destination.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("atomic transaction replacement failed: " + destination.generic_string());
#else
    if (rename(source.c_str(),destination.c_str()) != 0) throw std::runtime_error("atomic transaction replacement failed");
    const auto directory = open(destination.parent_path().c_str(),O_RDONLY|O_CLOEXEC);
    if (directory >= 0) { const auto status = fsync(directory); close(directory); if (status != 0) throw std::runtime_error("transaction directory flush failed"); }
#endif
}
bool IsLocalTransactionFilesystem(const std::filesystem::path& path)
{
#ifdef _WIN32
    wchar_t volume[MAX_PATH]{};
    if (!GetVolumePathNameW(std::filesystem::absolute(path).c_str(),volume,MAX_PATH)) return false;
    if (GetDriveTypeW(volume) != DRIVE_FIXED) return false;
    wchar_t type[64]{};
    if (!GetVolumeInformationW(volume,nullptr,0,nullptr,nullptr,nullptr,type,64)) return false;
    return std::wstring_view(type) == L"NTFS" || std::wstring_view(type) == L"ReFS";
#else
    struct statfs info{};
    if (statfs(path.c_str(),&info) != 0) return false;
#ifdef __APPLE__
    return std::string_view(info.f_fstypename) == "apfs" || std::string_view(info.f_fstypename) == "hfs";
#else
    return info.f_type == 0xef53 || info.f_type == 0x9123683e || info.f_type == 0x58465342 || info.f_type == 0x01021994;
#endif
#endif
}
}
