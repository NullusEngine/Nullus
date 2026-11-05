#pragma once
#include "PlatformDef.h"
#include <filesystem>
#include <string>
#include <vector>

namespace NLS::Platform
{
// Mappings are pinned by default. Only a caller that has completed the entire
// module drain/cleanup protocol may opt into closing a mapping.
class NLS_PLATFORM_API NativeLibrary
{
public:
    explicit NativeLibrary(const std::filesystem::path& absolutePath);
    static void ValidateTarget(const std::filesystem::path& path,const std::string& platform,const std::string& architecture);
    static std::vector<std::string> WindowsImports(const std::filesystem::path& path);
    static bool IsWindowsSystemLibrary(const std::string& name);
    ~NativeLibrary();
    NativeLibrary(const NativeLibrary&) = delete;
    NativeLibrary& operator=(const NativeLibrary&) = delete;
    void* Symbol(const char* name) const;
    void AllowUnload() { m_canUnload = true; }
    const std::filesystem::path& Path() const { return m_path; }
private:
    std::filesystem::path m_path;
    void* m_handle = nullptr;
    bool m_canUnload = false;
};
}
