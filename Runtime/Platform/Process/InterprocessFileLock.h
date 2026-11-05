#pragma once
#include "PlatformDef.h"
#include <chrono>
#include <filesystem>
#include <memory>
#include <string_view>

namespace NLS::Platform::Process
{
class NLS_PLATFORM_API InterprocessFileLock
{
public:
    InterprocessFileLock(const std::filesystem::path& path, bool exclusive = true,
        std::chrono::milliseconds timeout = std::chrono::seconds(30));
    ~InterprocessFileLock();
    InterprocessFileLock(const InterprocessFileLock&) = delete;
    InterprocessFileLock& operator=(const InterprocessFileLock&) = delete;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
NLS_PLATFORM_API void WriteDurableFile(const std::filesystem::path& path, std::string_view bytes);
NLS_PLATFORM_API void ReplaceFileAtomically(const std::filesystem::path& source, const std::filesystem::path& destination);
NLS_PLATFORM_API bool IsLocalTransactionFilesystem(const std::filesystem::path& path);
}
