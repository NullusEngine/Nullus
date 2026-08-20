#include "VisualStudioExtensionInstaller.h"

#include <algorithm>
#include <cctype>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace NLS::Editor::Debug
{
namespace
{
#ifdef _WIN32
std::string Narrow(const std::wstring& value)
{
    if (value.empty())
        return {};
    const auto size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        return {};
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring ReadValue(HKEY key, const wchar_t* name)
{
    DWORD type = 0u;
    DWORD bytes = 0u;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t))
        return {};
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(value.data()), &bytes) != ERROR_SUCCESS)
        return {};
    while (!value.empty() && value.back() == L'\0')
        value.pop_back();
    return value;
}
#endif
}

std::vector<VisualStudioInstance> DiscoverVisualStudioInstances()
{
    std::vector<VisualStudioInstance> instances;
#ifdef _WIN32
    constexpr const wchar_t* roots[] = {
        L"SOFTWARE\\Microsoft\\VisualStudio\\SxS\\VS7",
        L"SOFTWARE\\WOW6432Node\\Microsoft\\VisualStudio\\SxS\\VS7"};
    for (const auto* root : roots)
    {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, root, 0u, KEY_READ, &key) != ERROR_SUCCESS)
            continue;
        for (DWORD index = 0u;; ++index)
        {
            wchar_t name[128] = {};
            wchar_t value[1024] = {};
            DWORD nameSize = static_cast<DWORD>(std::size(name));
            DWORD valueSize = static_cast<DWORD>(sizeof(value));
            DWORD type = 0u;
            const auto result = RegEnumValueW(key, index, name, &nameSize, nullptr, &type,
                reinterpret_cast<BYTE*>(value), &valueSize);
            if (result != ERROR_SUCCESS)
                break;
            if (type != REG_SZ || std::wstring(name, nameSize).rfind(L"17.", 0u) != 0u)
                continue;
            VisualStudioInstance instance;
            instance.installationPath = std::filesystem::path(std::wstring(value, valueSize / sizeof(wchar_t) - 1u));
            instance.version = Narrow(std::wstring(name, nameSize));
            instance.edition = instance.installationPath.filename().string();
            instance.compatible = std::filesystem::is_regular_file(
                instance.installationPath / "Common7" / "IDE" / "VSIXInstaller.exe");
            instances.push_back(std::move(instance));
        }
        RegCloseKey(key);
    }
#endif
    std::sort(instances.begin(), instances.end(), [](const auto& left, const auto& right)
    {
        return left.installationPath.generic_string() < right.installationPath.generic_string();
    });
    instances.erase(std::unique(instances.begin(), instances.end(), [](const auto& left, const auto& right)
    {
        return left.installationPath == right.installationPath;
    }), instances.end());
    return instances;
}

bool InstallVisualStudioExtension(
    const VisualStudioInstance& instance,
    const std::filesystem::path& vsixPath,
    const bool userConfirmed,
    std::string& errorMessage)
{
    if (!userConfirmed)
    {
        errorMessage = "Visual Studio extension installation requires explicit user confirmation.";
        return false;
    }
    if (!instance.compatible || instance.installationPath.empty())
    {
        errorMessage = "The selected Visual Studio instance is not compatible with the Nullus VSIX.";
        return false;
    }
    if (!std::filesystem::is_regular_file(vsixPath))
    {
        errorMessage = "The bundled Nullus VSIX does not exist: " + vsixPath.string();
        return false;
    }
#ifdef _WIN32
    const auto installer = instance.installationPath / "Common7" / "IDE" / "VSIXInstaller.exe";
    std::wstring command = L"\"" + installer.wstring() + L"\" \"" + vsixPath.wstring() + L"\"";
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');
    if (CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE, 0u, nullptr, nullptr, &startup, &process) == FALSE)
    {
        errorMessage = "Unable to start VSIXInstaller.exe (error " + std::to_string(GetLastError()) + ").";
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
#else
    errorMessage = "Visual Studio VSIX installation is only supported on Windows.";
    return false;
#endif
}
}
