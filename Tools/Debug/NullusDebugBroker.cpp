#include <Json/json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#endif

namespace
{
using Json = nlohmann::json;

struct Manifest
{
    std::filesystem::path projectRoot;
    std::filesystem::path projectFile;
    std::filesystem::path workspaceRoot;
    std::filesystem::path editorExecutable;
    std::filesystem::path engineSourceRoot;
    std::filesystem::path engineBuildRoot;
    std::filesystem::path cmakeExecutable;
    std::string nativeTarget = "Editor";
    std::string nativeBuildId;
    bool engineSourceAvailable = false;
    bool nativeSymbolsAvailable = false;
    bool mixedDebugAvailable = false;
    std::string projectId;
    uint32_t protocolMajor = 2u;
    uint32_t protocolMinor = 0u;
    std::string requiredVsixVersion;
};

struct Instance
{
    uint32_t processId = 0u;
    uint64_t processStartTime = 0u;
    std::string projectId;
    std::string sessionId;
    uint32_t protocolMajor = 1u;
    uint32_t protocolMinor = 0u;
    std::filesystem::path editorExecutable;
    std::string editorBuildId;
    std::string endpoint;
    std::vector<std::string> capabilities;
};

uint32_t ProtocolMajor(const Json& value, const uint32_t fallback = 1u)
{
    if (!value.is_object())
        return fallback;
    if (value.contains("protocolMajor") && value.at("protocolMajor").is_number_unsigned())
        return value.at("protocolMajor").get<uint32_t>();
    if (value.contains("protocolVersion"))
    {
        const auto& version = value.at("protocolVersion");
        if (version.is_object())
            return version.value("major", fallback);
        if (version.is_number_unsigned())
            return version.get<uint32_t>();
    }
    return fallback;
}

class LaunchLock final
{
public:
    ~LaunchLock()
    {
#ifdef _WIN32
        if (m_handle != nullptr)
            CloseHandle(m_handle);
#else
        if (m_descriptor >= 0)
            ::close(m_descriptor);
        if (!m_path.empty())
        {
            std::error_code error;
            std::filesystem::remove(m_path, error);
        }
#endif
    }

    bool Acquire(const Manifest& manifest, const int timeoutMs, std::string& error)
    {
#ifdef _WIN32
        const auto name = std::string("Local\\Nullus.Debug.") + manifest.projectId;
        m_handle = CreateMutexA(nullptr, FALSE, name.c_str());
        if (m_handle == nullptr)
        {
            error = "Unable to create the project debug launch lock (error " + std::to_string(GetLastError()) + ").";
            return false;
        }
        const auto result = WaitForSingleObject(m_handle, static_cast<DWORD>(timeoutMs));
        if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED)
        {
            error = "Timed out waiting for another F5 launch for this project.";
            return false;
        }
        return true;
#else
        m_path = manifest.workspaceRoot / ".debug-broker.lock";
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;)
        {
            m_descriptor = ::open(m_path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
            if (m_descriptor >= 0)
                return true;
            if (errno != EEXIST || std::chrono::steady_clock::now() >= deadline)
            {
                error = "Timed out waiting for another F5 launch for this project.";
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
#endif
    }

private:
#ifdef _WIN32
    HANDLE m_handle = nullptr;
#else
    int m_descriptor = -1;
    std::filesystem::path m_path;
#endif
};

std::optional<Json> ReadJson(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return std::nullopt;
    const auto value = Json::parse(input, nullptr, false);
    return value.is_discarded() || !value.is_object() ? std::nullopt : std::optional<Json>(value);
}

std::optional<Manifest> ReadManifest(const std::filesystem::path& path, std::string& error)
{
    const auto value = ReadJson(path);
    if (!value)
    {
        error = "Nullus debug manifest is missing or invalid: " + path.string();
        return std::nullopt;
    }
    Manifest manifest;
    manifest.projectRoot = value->value("projectRoot", "");
    manifest.projectFile = value->value("projectFile", "");
    manifest.workspaceRoot = value->value("workspaceRoot", path.parent_path().string());
    manifest.editorExecutable = value->value("editorExecutable", "");
    manifest.engineSourceRoot = value->value("engineSourceRoot", "");
    manifest.engineBuildRoot = value->value("engineBuildRoot", "");
    manifest.cmakeExecutable = value->value("cmakeExecutable", "");
    manifest.nativeTarget = value->value("nativeTarget", "Editor");
    manifest.nativeBuildId = value->value("nativeBuildId", "");
    manifest.engineSourceAvailable = value->value("engineSourceAvailable", false);
    manifest.nativeSymbolsAvailable = value->value("nativeSymbolsAvailable", false);
    manifest.mixedDebugAvailable = value->value("mixedDebugAvailable", false);
    manifest.projectId = value->value("projectId", "");
    if (value->contains("protocolVersion") && value->at("protocolVersion").is_object())
    {
        manifest.protocolMajor = value->at("protocolVersion").value("major", 2u);
        manifest.protocolMinor = value->at("protocolVersion").value("minor", 0u);
    }
    else
    {
        manifest.protocolMajor = value->value("protocolMajor", value->value("protocolVersion", 1u));
        manifest.protocolMinor = value->value("protocolMinor", 0u);
    }
    manifest.requiredVsixVersion = value->value("requiredVsixVersion", "");
    if (manifest.projectRoot.empty() || manifest.projectId.empty())
    {
        error = "Nullus debug manifest is missing projectRoot or projectId.";
        return std::nullopt;
    }
    return manifest;
}

std::string QuoteArgument(const std::string& value)
{
    std::string quoted = "\"";
    for (const auto character : value)
    {
        if (character == '"')
            quoted += '\\';
        quoted += character;
    }
    quoted += "\"";
    return quoted;
}

std::string QuoteArgument(const std::filesystem::path& value)
{
    return QuoteArgument(value.string());
}

#ifdef _WIN32
std::wstring QuoteWindowsArgument(const std::wstring& value)
{
    std::wstring quoted = L"\"";
    for (const auto character : value)
    {
        if (character == L'\"')
            quoted += L'\\';
        quoted += character;
    }
    quoted += L"\"";
    return quoted;
}

bool RunWindowsBatch(const std::filesystem::path& batch,
    const std::vector<std::wstring>& arguments,
    const std::filesystem::path& logPath,
    std::string& error)
{
    wchar_t comSpecBuffer[MAX_PATH] = {};
    const auto comSpecLength = GetEnvironmentVariableW(L"ComSpec", comSpecBuffer,
        static_cast<DWORD>(std::size(comSpecBuffer)));
    const std::wstring comSpec = comSpecLength > 0u &&
        comSpecLength < std::size(comSpecBuffer)
        ? std::wstring(comSpecBuffer, comSpecLength)
        : L"C:\\Windows\\System32\\cmd.exe";

    std::wstring command = QuoteWindowsArgument(batch.wstring());
    for (const auto& argument : arguments)
        command += L" " + QuoteWindowsArgument(argument);
    command += L" > " + QuoteWindowsArgument(logPath.wstring()) + L" 2>&1";

    // std::system() applies an extra CRT quote pass on Windows. That makes a
    // quoted batch path invalid when another quoted argument follows it.
    // Invoke cmd.exe explicitly so the repository environment wrapper gets a
    // single, deterministic command line and can normalize PATH/Path.
    std::wstring commandLine = QuoteWindowsArgument(comSpec) + L" /d /s /c \"" + command + L"\"";
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
    {
        error = "Unable to start the native build command (error " +
            std::to_string(GetLastError()) + ").";
        return false;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1u;
    (void)GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (exitCode != 0u)
    {
        error = "Native Editor build failed (exit " + std::to_string(exitCode) +
            "). See " + logPath.string();
        return false;
    }
    return true;
}
#endif

std::string FileStamp(const std::filesystem::path& path)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error))
        return {};
    const auto size = std::filesystem::file_size(path, error);
    if (error)
        return {};
    const auto timestamp = std::filesystem::last_write_time(path, error);
    if (error)
        return std::to_string(size);
    return std::to_string(size) + ":" + std::to_string(timestamp.time_since_epoch().count());
}

std::filesystem::path EditorExecutableForConfiguration(
    const Manifest& manifest,
    const std::string& configuration)
{
    if (manifest.editorExecutable.empty() ||
        (configuration != "Debug" && configuration != "Release"))
        return manifest.editorExecutable;

    auto directoryName = manifest.editorExecutable.parent_path().filename().string();
    const std::string requestedTag = "_" + configuration + "_";
    if (directoryName.find(requestedTag) != std::string::npos)
        return manifest.editorExecutable;

    const std::string otherConfiguration = configuration == "Debug" ? "Release" : "Debug";
    const std::string otherTag = "_" + otherConfiguration + "_";
    const auto tag = directoryName.find(otherTag);
    if (tag == std::string::npos)
        return manifest.editorExecutable;
    directoryName.replace(tag, otherTag.size(), requestedTag);
    auto candidate = manifest.editorExecutable.parent_path().parent_path() /
        directoryName / manifest.editorExecutable.filename();
    if (std::filesystem::is_regular_file(candidate))
        return candidate;
    return manifest.editorExecutable;
}

Manifest ManifestForConfiguration(const Manifest& manifest, const std::string& configuration)
{
    auto selected = manifest;
    selected.editorExecutable = EditorExecutableForConfiguration(manifest, configuration);
    selected.nativeBuildId = FileStamp(selected.editorExecutable);
    auto pdbPath = selected.editorExecutable;
    pdbPath.replace_extension(".pdb");
    selected.nativeSymbolsAvailable = !FileStamp(pdbPath).empty();
    selected.mixedDebugAvailable = selected.engineSourceAvailable &&
        selected.nativeSymbolsAvailable && !selected.editorExecutable.empty();
    return selected;
}

bool BuildNative(const Manifest& manifest, const std::string& configuration, std::string& error)
{
    if (!manifest.engineSourceAvailable || manifest.engineSourceRoot.empty() ||
        manifest.engineBuildRoot.empty() || manifest.cmakeExecutable.empty())
    {
        error = "C++ source debugging is unavailable: the project has no verified EngineWorkspace descriptor.";
        return false;
    }
    if (configuration != "Debug" && configuration != "Release")
    {
        error = "Unsupported native configuration: " + configuration + ". Use Debug or Release.";
        return false;
    }
    if (!std::filesystem::is_regular_file(manifest.cmakeExecutable))
    {
        error = "The configured CMake executable does not exist: " + manifest.cmakeExecutable.string();
        return false;
    }
    const auto logPath = manifest.workspaceRoot / "NativeBuild.log";
    std::error_code filesystemError;
    std::filesystem::create_directories(logPath.parent_path(), filesystemError);
    if (filesystemError)
    {
        error = "Unable to create the native build log directory: " + filesystemError.message();
        return false;
    }

#ifdef _WIN32
    const auto cleanEnvironment = manifest.engineSourceRoot / "Tools" / "BuildCleanEnvironment.cmd";
    if (!std::filesystem::is_regular_file(cleanEnvironment))
    {
        error = "The repository-local build environment wrapper is missing: " + cleanEnvironment.string();
        return false;
    }
    const std::vector<std::wstring> arguments = {
        manifest.cmakeExecutable.wstring(),
        L"--build", manifest.engineBuildRoot.wstring(),
        L"--config", std::wstring(configuration.begin(), configuration.end()),
        L"--target", std::wstring(manifest.nativeTarget.begin(), manifest.nativeTarget.end()),
        L"--", L"/m:1"};
    return RunWindowsBatch(cleanEnvironment, arguments, logPath, error);
#else
    const auto command = QuoteArgument(manifest.cmakeExecutable) +
        " --build " + QuoteArgument(manifest.engineBuildRoot) +
        " --config " + QuoteArgument(configuration) +
        " --target " + QuoteArgument(manifest.nativeTarget) +
        " > " + QuoteArgument(logPath) + " 2>&1";
    const auto result = std::system(command.c_str());
    if (result != 0)
    {
        error = "Native Editor build failed (exit " + std::to_string(result) +"). See " + logPath.string();
        return false;
    }
    return true;
#endif
}

std::optional<Instance> ReadInstance(const std::filesystem::path& path)
{
    const auto value = ReadJson(path);
    if (!value)
        return std::nullopt;
    Instance instance;
    instance.processId = value->value("processId", 0u);
    instance.processStartTime = value->value("processStartTime", 0ull);
    instance.projectId = value->value("projectId", "");
    instance.sessionId = value->value("sessionId", "");
    instance.protocolMajor = ProtocolMajor(*value);
    instance.protocolMinor = value->value("protocolMinor", 0u);
    instance.editorExecutable = value->value("editorExecutable", "");
    instance.editorBuildId = value->value("editorBuildId", "");
    instance.endpoint = value->value("endpoint", "");
    if (value->contains("capabilities") && value->at("capabilities").is_array())
        instance.capabilities = value->at("capabilities").get<std::vector<std::string>>();
    if (instance.processId == 0u || instance.projectId.empty() || instance.endpoint.empty())
        return std::nullopt;
    return instance;
}

std::filesystem::path NormalizePath(const std::filesystem::path& path)
{
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(path, error);
    return error ? path.lexically_normal() : canonical;
}

bool SamePath(const std::filesystem::path& left, const std::filesystem::path& right)
{
    auto a = NormalizePath(left).generic_string();
    auto b = NormalizePath(right).generic_string();
#ifdef _WIN32
    std::transform(a.begin(), a.end(), a.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(b.begin(), b.end(), b.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
    return a == b;
}

#ifdef _WIN32
bool ProcessStartTime(uint32_t processId, uint64_t& output)
{
    const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process)
        return false;
    FILETIME creation = {}, exit = {}, kernel = {}, user = {};
    const bool success = GetProcessTimes(process, &creation, &exit, &kernel, &user) != FALSE;
    CloseHandle(process);
    if (!success)
        return false;
    ULARGE_INTEGER value = {};
    value.LowPart = creation.dwLowDateTime;
    value.HighPart = creation.dwHighDateTime;
    output = value.QuadPart;
    return true;
}

bool ProcessExecutable(uint32_t processId, std::filesystem::path& output)
{
    const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process)
        return false;
    wchar_t buffer[MAX_PATH] = {};
    DWORD length = static_cast<DWORD>(std::size(buffer));
    const bool success = QueryFullProcessImageNameW(process, 0u, buffer, &length) != FALSE;
    CloseHandle(process);
    if (!success)
        return false;
    output = std::filesystem::path(std::wstring(buffer, length));
    return true;
}

std::optional<Json> SendJsonRequest(const std::string& endpoint, Json request, const int timeoutMs)
{
    if (!request.is_object())
        return std::nullopt;
    if (!request.contains("protocolVersion"))
        request["protocolVersion"] = Json{{"major", 2u}, {"minor", 0u}};
    if (!request.contains("arguments"))
        request["arguments"] = Json::object();
    request["requestId"] = request.value("requestId", std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    // CreateFile is intentionally retried instead of relying on
    // WaitNamedPipe. On Windows, WaitNamedPipe can remain inside the kernel
    // longer than its nominal timeout while the single Editor pipe instance
    // is being disconnected. A bounded CreateFile loop keeps F5 responsive.
    HANDLE pipe = INVALID_HANDLE_VALUE;
    const auto deadline = GetTickCount64() + static_cast<ULONGLONG>((std::max)(1, timeoutMs));
    while (GetTickCount64() <= deadline)
    {
        pipe = CreateFileA(endpoint.c_str(), GENERIC_READ | GENERIC_WRITE, 0u, nullptr, OPEN_EXISTING, 0u, nullptr);
        if (pipe != INVALID_HANDLE_VALUE)
            break;
        const auto lastError = GetLastError();
        if (lastError != ERROR_PIPE_BUSY && lastError != ERROR_FILE_NOT_FOUND)
            return std::nullopt;
        Sleep(1u);
    }
    if (pipe == INVALID_HANDLE_VALUE)
    {
        return std::nullopt;
    }
    const auto requestText = request.dump() + "\n";
    DWORD written = 0u;
    const bool wrote = WriteFile(pipe, requestText.data(), static_cast<DWORD>(requestText.size()), &written, nullptr) != FALSE &&
        written == requestText.size();
    std::string response;
    if (wrote)
    {
        // WaitNamedPipe only bounds acquisition of the pipe. A synchronous
        // ReadFile after the connection can still wait forever when the
        // Editor main thread is stalled in startup or a previous request. Use
        // PeekNamedPipe to poll for bytes and enforce the same command
        // deadline for the complete response.
        const auto deadline = GetTickCount64() + static_cast<ULONGLONG>((std::max)(1, timeoutMs));
        char buffer[4096] = {};
        while (response.size() < 64u * 1024u && GetTickCount64() <= deadline)
        {
            DWORD available = 0u;
            if (PeekNamedPipe(pipe, nullptr, 0u, nullptr, &available, nullptr) == FALSE)
                break;
            if (available == 0u)
            {
                Sleep(1u);
                continue;
            }
            DWORD read = 0u;
            const auto capacity = static_cast<DWORD>((std::min)(sizeof(buffer) - 1u,
                static_cast<size_t>(available)));
            if (ReadFile(pipe, buffer, capacity, &read, nullptr) == FALSE || read == 0u)
                break;
            response.append(buffer, buffer + read);
            if (response.find('\n') != std::string::npos)
                break;
        }
    }
    CloseHandle(pipe);
    const auto value = Json::parse(response, nullptr, false);
    return value.is_discarded() ? std::nullopt : std::optional<Json>(value);
}

std::optional<Json> SendRequest(const std::string& endpoint, const std::string& command, const int timeoutMs)
{
    return SendJsonRequest(endpoint, Json{{"command", command}}, timeoutMs);
}

bool LaunchEditor(const Manifest& manifest, std::string& error)
{
    if (manifest.editorExecutable.empty() || !std::filesystem::is_regular_file(manifest.editorExecutable))
    {
        error = "The project debug manifest does not contain a valid absolute Editor path.";
        return false;
    }
    std::wstring command = L"\"" + manifest.editorExecutable.wstring() + L"\" \"" + manifest.projectFile.wstring() + L"\"";
    // The Broker is normally launched with stdout/stderr redirected by an IDE.
    // Do not let the long-lived Editor inherit those pipe handles: the IDE's
    // ReadToEnd would otherwise wait until the Editor exits instead of
    // returning when this short-lived Broker process exits.
    const auto nullInput = CreateFileW(L"NUL", GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    const auto nullOutput = CreateFileW(L"NUL", GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    const auto nullError = CreateFileW(L"NUL", GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (nullInput == INVALID_HANDLE_VALUE || nullOutput == INVALID_HANDLE_VALUE ||
        nullError == INVALID_HANDLE_VALUE)
    {
        const auto lastError = GetLastError();
        if (nullInput != INVALID_HANDLE_VALUE) CloseHandle(nullInput);
        if (nullOutput != INVALID_HANDLE_VALUE) CloseHandle(nullOutput);
        if (nullError != INVALID_HANDLE_VALUE) CloseHandle(nullError);
        error = "Unable to open NUL handles for the Editor process (error " +
            std::to_string(lastError) + ").";
        return false;
    }
    SetHandleInformation(nullInput, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    SetHandleInformation(nullOutput, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    SetHandleInformation(nullError, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    STARTUPINFOEXW startup = {};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = nullInput;
    startup.StartupInfo.hStdOutput = nullOutput;
    startup.StartupInfo.hStdError = nullError;
    SIZE_T attributeBytes = 0u;
    (void)InitializeProcThreadAttributeList(nullptr, 1u, 0u, &attributeBytes);
    startup.lpAttributeList = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
        HeapAlloc(GetProcessHeap(), 0u, attributeBytes));
    if (startup.lpAttributeList == nullptr ||
        InitializeProcThreadAttributeList(startup.lpAttributeList, 1u, 0u, &attributeBytes) == FALSE)
    {
        const auto lastError = GetLastError();
        if (startup.lpAttributeList != nullptr)
            HeapFree(GetProcessHeap(), 0u, startup.lpAttributeList);
        CloseHandle(nullInput);
        CloseHandle(nullOutput);
        CloseHandle(nullError);
        error = "Unable to prepare isolated Editor process handles (error " +
            std::to_string(lastError) + ").";
        return false;
    }
    const HANDLE inheritedHandles[] = { nullInput, nullOutput, nullError };
    if (UpdateProcThreadAttribute(startup.lpAttributeList, 0u,
        PROC_THREAD_ATTRIBUTE_HANDLE_LIST, const_cast<HANDLE*>(inheritedHandles),
        sizeof(inheritedHandles), nullptr, nullptr) == FALSE)
    {
        const auto lastError = GetLastError();
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        HeapFree(GetProcessHeap(), 0u, startup.lpAttributeList);
        CloseHandle(nullInput);
        CloseHandle(nullOutput);
        CloseHandle(nullError);
        error = "Unable to isolate Editor process handles (error " +
            std::to_string(lastError) + ").";
        return false;
    }
    PROCESS_INFORMATION process = {};
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');
    const auto created = CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, TRUE,
        EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &process) != FALSE;
    const auto lastError = created ? ERROR_SUCCESS : GetLastError();
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    HeapFree(GetProcessHeap(), 0u, startup.lpAttributeList);
    CloseHandle(nullInput);
    CloseHandle(nullOutput);
    CloseHandle(nullError);
    if (!created)
    {
        error = "Unable to start Editor.exe (error " + std::to_string(lastError) + ").";
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}
#else
bool SendAll(const int descriptor, const std::string& text)
{
    size_t offset = 0u;
    while (offset < text.size())
    {
        const auto count = ::write(descriptor, text.data() + offset, text.size() - offset);
        if (count <= 0)
            return false;
        offset += static_cast<size_t>(count);
    }
    return true;
}

std::optional<Json> SendJsonRequest(const std::string& endpoint, Json request, const int timeoutMs)
{
    if (!request.is_object())
        return std::nullopt;
    if (!request.contains("protocolVersion"))
        request["protocolVersion"] = Json{{"major", 2u}, {"minor", 0u}};
    if (!request.contains("arguments"))
        request["arguments"] = Json::object();
    request["requestId"] = request.value("requestId", std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto descriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (descriptor < 0)
        return std::nullopt;
    timeval timeout = {};
    timeout.tv_sec = timeoutMs / 1000;
    timeout.tv_usec = (timeoutMs % 1000) * 1000;
    setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    if (endpoint.size() >= sizeof(address.sun_path))
    {
        ::close(descriptor);
        return std::nullopt;
    }
    std::copy(endpoint.begin(), endpoint.end(), address.sun_path);
    if (::connect(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        !SendAll(descriptor, request.dump() + "\n"))
    {
        ::close(descriptor);
        return std::nullopt;
    }
    std::string response;
    char character = 0;
    while (response.size() < 64u * 1024u)
    {
        const auto count = ::read(descriptor, &character, 1u);
        if (count <= 0 || character == '\n')
            break;
        response.push_back(character);
    }
    ::close(descriptor);
    const auto value = Json::parse(response, nullptr, false);
    return value.is_discarded() ? std::nullopt : std::optional<Json>(value);
}

std::optional<Json> SendRequest(const std::string& endpoint, const std::string& command, const int timeoutMs)
{
    return SendJsonRequest(endpoint, Json{{"command", command}}, timeoutMs);
}

bool LaunchEditor(const Manifest& manifest, std::string& error)
{
    if (manifest.editorExecutable.empty() || !std::filesystem::is_regular_file(manifest.editorExecutable))
    {
        error = "The project debug manifest does not contain a valid absolute Editor path.";
        return false;
    }
    const auto pid = fork();
    if (pid < 0)
    {
        error = "Unable to fork Editor: " + std::string(std::strerror(errno));
        return false;
    }
    if (pid == 0)
    {
        // The IDE captures Broker stdout/stderr. Detach the long-lived Editor
        // from those descriptors before replacing the child image.
        (void)freopen("/dev/null", "r", stdin);
        (void)freopen("/dev/null", "w", stdout);
        (void)freopen("/dev/null", "w", stderr);
        execl(manifest.editorExecutable.c_str(), manifest.editorExecutable.c_str(), manifest.projectFile.c_str(), nullptr);
        _exit(127);
    }
    return true;
}
#endif

#ifndef _WIN32
bool ProcessIsAlive(uint32_t processId)
{
    return processId != 0u && (::kill(static_cast<pid_t>(processId), 0) == 0 || errno == EPERM);
}

uint64_t ProcessStartTime(uint32_t processId)
{
#if defined(__linux__)
    std::ifstream stat("/proc/" + std::to_string(processId) + "/stat");
    std::string line;
    if (stat && std::getline(stat, line))
    {
        const auto closingName = line.rfind(") ");
        if (closingName != std::string::npos)
        {
            std::istringstream fields(line.substr(closingName + 2u));
            std::string value;
            for (int index = 0; index < 20; ++index)
            {
                if (!(fields >> value))
                    return 0u;
            }
            try { return std::stoull(value); }
            catch (...) { return 0u; }
        }
    }
#else
    (void)processId;
#endif
    return 0u;
}

bool ProcessExecutable(uint32_t processId, std::filesystem::path& output)
{
#if defined(__linux__)
    std::error_code error;
    const auto link = std::filesystem::read_symlink(
        std::filesystem::path("/proc") / std::to_string(processId) / "exe", error);
    if (error)
        return false;
    output = link;
    return true;
#else
    (void)processId;
    (void)output;
    return true;
#endif
}
#endif

bool EndpointReportsInstance(const Instance& instance)
{
    const auto response = SendRequest(instance.endpoint, "Ping", 100);
    if (!response)
        return false;
    if (response->value("projectId", "") != instance.projectId ||
        response->value("processId", 0u) != instance.processId)
        return false;
    const auto reportedStart = response->value("processStartTime", 0ull);
    return instance.processStartTime == 0u || reportedStart == 0u ||
        reportedStart == instance.processStartTime;
}

bool EndpointMatchesManifest(const Manifest& manifest, const Instance& instance)
{
    if (!EndpointReportsInstance(instance))
        return false;
    const auto response = SendRequest(instance.endpoint, "Ping", 100);
    if (!response)
        return false;
    const auto executable = response->value("editorExecutable", "");
    return manifest.editorExecutable.empty() ||
        (!executable.empty() && SamePath(manifest.editorExecutable, executable));
}

bool IsMatchingInstance(const Manifest& manifest, const Instance& instance)
{
    if (instance.projectId != manifest.projectId)
        return false;
#ifdef _WIN32
    uint64_t startTime = 0u;
    std::filesystem::path executable;
    if (!ProcessStartTime(instance.processId, startTime) || startTime != instance.processStartTime ||
        !ProcessExecutable(instance.processId, executable))
        return EndpointMatchesManifest(manifest, instance);
    if (!manifest.editorExecutable.empty() && !SamePath(manifest.editorExecutable, executable))
        return false;
#else
    if (!ProcessIsAlive(instance.processId))
        return false;
    if (instance.processStartTime != 0u)
    {
        const auto currentStart = ProcessStartTime(instance.processId);
        if (currentStart != 0u && currentStart != instance.processStartTime)
            return false;
    }
    std::filesystem::path executable;
    if (!ProcessExecutable(instance.processId, executable))
        return false;
    if (!manifest.editorExecutable.empty() && !SamePath(manifest.editorExecutable, executable))
        return false;
#endif
    return true;
}

bool IsLiveProjectInstance(const Manifest& manifest, const Instance& instance)
{
    if (instance.projectId != manifest.projectId)
        return false;
#ifdef _WIN32
    uint64_t startTime = 0u;
    if (!ProcessStartTime(instance.processId, startTime) ||
        startTime != instance.processStartTime)
        return false;
    std::filesystem::path executable;
    // A PID can remain queryable briefly while an Editor is terminating, and
    // elevated processes may deny image-path queries to the Broker. In both
    // cases the project pipe is the authoritative liveness signal.
    return ProcessExecutable(instance.processId, executable) ||
        EndpointReportsInstance(instance);
#else
    if (!ProcessIsAlive(instance.processId))
        return false;
    if (instance.processStartTime != 0u)
    {
        const auto currentStart = ProcessStartTime(instance.processId);
        if (currentStart != 0u && currentStart != instance.processStartTime)
            return false;
    }
    return true;
#endif
}

void RemoveStaleInstanceRecord(
    const Manifest& manifest,
    const std::filesystem::path& instancePath)
{
    const auto instance = ReadInstance(instancePath);
    if (!instance || instance->projectId != manifest.projectId ||
        IsLiveProjectInstance(manifest, *instance))
        return;

    // An Editor crash can leave the JSON record behind even though the OS
    // process and its named mutex are gone. Remove only a record proven to be
    // stale; never delete a live instance record for another build.
    std::error_code error;
    std::filesystem::remove(instancePath, error);
}

#ifdef _WIN32
bool ProcessHasDebugger(uint32_t processId)
{
    const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process)
        return false;
    BOOL present = FALSE;
    const auto queried = CheckRemoteDebuggerPresent(process, &present) != FALSE;
    CloseHandle(process);
    return queried && present != FALSE;
}
#else
bool ProcessHasDebugger(uint32_t)
{
    // Linux/macOS do not expose a portable, non-invasive equivalent of
    // CheckRemoteDebuggerPresent.  The native debug engine reports an attach
    // conflict when ptrace ownership prevents the attach, so leave this
    // advisory field false here.
    return false;
}
#endif

struct NativeInstanceState
{
    bool running = false;
    bool executableMatches = true;
    bool debuggerAttached = false;
    bool binaryCurrent = true;
    bool restartRequired = false;
    uint32_t processId = 0u;
};

NativeInstanceState ReadNativeInstanceState(const Manifest& manifest)
{
    NativeInstanceState state;
    const auto instancePath = manifest.workspaceRoot / "EditorInstance.json";
    const auto instance = ReadInstance(instancePath);
    if (!instance || !IsLiveProjectInstance(manifest, *instance))
    {
        RemoveStaleInstanceRecord(manifest, instancePath);
        return state;
    }

    state.running = true;
    state.processId = instance->processId;
    state.debuggerAttached = ProcessHasDebugger(instance->processId);
    std::filesystem::path runningExecutable;
    if (ProcessExecutable(instance->processId, runningExecutable) &&
        !manifest.editorExecutable.empty() &&
        !SamePath(manifest.editorExecutable, runningExecutable))
    {
        state.executableMatches = false;
        state.restartRequired = true;
    }
    const auto currentBuildId = FileStamp(manifest.editorExecutable);
    if (state.executableMatches && !instance->editorBuildId.empty() && !currentBuildId.empty() &&
        instance->editorBuildId != currentBuildId)
    {
        state.binaryCurrent = false;
        state.restartRequired = true;
    }
    return state;
}

bool PrepareNativeEditor(const Manifest& manifest, const std::string& configuration, std::string& error)
{
    const auto state = ReadNativeInstanceState(manifest);
    if (state.running)
    {
        if (state.debuggerAttached)
        {
            error = "Editor is already attached by another Visual Studio session. "
                "Start Mixed debugging from Nullus.Project.sln. (PID " +
                std::to_string(state.processId) + ")";
            return false;
        }
        if (!state.executableMatches)
        {
            error = "A different Editor build is already running for this project (PID " +
                std::to_string(state.processId) +
                "). Close it or start Mixed debugging from the matching Nullus.Project.sln; "
                "the existing Editor was left untouched.";
            return false;
        }
        if (state.restartRequired)
        {
            error = "The running Editor uses an older native build (PID " +
                std::to_string(state.processId) +
                "). Restart the Editor before Mixed debugging; the current Editor was left untouched.";
            return false;
        }
        // A matching process already loaded the requested binary.  Rebuilding
        // it would fail on Windows with LNK1168 and would change the running
        // Editor underneath the debugger, so reuse it unchanged.
        return true;
    }
    return BuildNative(manifest, configuration, error);
}

std::optional<Json> EnsureReady(const Manifest& manifest, const int timeoutMs, std::string& error)
{
    const auto instancePath = manifest.workspaceRoot / "EditorInstance.json";
    // Clean records left by a crashed/force-terminated Editor before looking
    // for a conflicting live process. This makes the next F5 self-healing.
    RemoveStaleInstanceRecord(manifest, instancePath);
    // Do not treat an Editor started from another build tree as absent.  That
    // would allow a second process to be launched for the same project and
    // would make CoreCLR attach nondeterministic.  A native debugger owns the
    // process even when its executable path differs, so report that case
    // before the path-conflict diagnostic.
    if (const auto existing = ReadInstance(instancePath);
        existing && IsLiveProjectInstance(manifest, *existing) &&
        ProcessHasDebugger(existing->processId))
    {
        error = "Editor is already attached by another Visual Studio session. "
            "Start Mixed debugging from Nullus.Project.sln. (PID " +
            std::to_string(existing->processId) + ")";
        return std::nullopt;
    }
    if (const auto existing = ReadInstance(instancePath);
        existing && IsLiveProjectInstance(manifest, *existing) &&
        !IsMatchingInstance(manifest, *existing))
    {
        error = "A different Editor build is already running for this project (PID " +
            std::to_string(existing->processId) +
            "). Close it or use the matching project workspace; the existing Editor was left untouched.";
        return std::nullopt;
    }
    bool liveInstanceSeen = false;
    auto tryExisting = [&](const int retryMs) -> std::optional<Json>
    {
        liveInstanceSeen = false;
        const auto instance = ReadInstance(instancePath);
        if (!instance || !IsMatchingInstance(manifest, *instance))
            return std::nullopt;
        liveInstanceSeen = true;
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds((std::max)(250, retryMs));
        std::optional<Json> ping;
        do
        {
            ping = SendRequest(instance->endpoint, "Ping", 250);
            if (ping && ping->value("projectId", "") == manifest.projectId &&
                ProtocolMajor(*ping, 0u) == manifest.protocolMajor)
                break;
            ping.reset();
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        while (std::chrono::steady_clock::now() < deadline);
        if (!ping)
            return std::nullopt;
        (void)SendRequest(instance->endpoint, "Focus", 250);
        // A clean Debug build routinely takes more than one second. Closing
        // the client while the Editor is still processing this command leaves
        // the single pipe instance flushing a response nobody can consume and
        // makes every later Ping look like a missing Editor. Keep this request
        // alive for the caller's preparation budget.
        (void)SendRequest(
            instance->endpoint,
            "PrepareManagedDebug",
            (std::max)(5000, timeoutMs));
        return ping;
    };

    const auto endpointRetryMs = (std::min)(2000, timeoutMs);
    auto state = tryExisting(endpointRetryMs);
    if (!state && liveInstanceSeen)
    {
        error = "The project Editor is running, but its debug endpoint stayed busy or unresponsive.";
        return std::nullopt;
    }
    if (!state)
    {
        LaunchLock launchLock;
        if (!launchLock.Acquire(manifest, timeoutMs, error))
            return std::nullopt;
        // A second F5 may have registered the Editor while this invocation
        // was waiting for the project lock.
        state = tryExisting(endpointRetryMs);
        if (state)
            return state;
        if (liveInstanceSeen)
        {
            error = "The project Editor is running, but its debug endpoint stayed busy or unresponsive.";
            return std::nullopt;
        }
        if (!LaunchEditor(manifest, error))
            return std::nullopt;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline && !state)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            state = tryExisting(endpointRetryMs);
        }
    }
    if (!state)
    {
        error = "Timed out waiting for the project Editor to register its debug endpoint.";
        return std::nullopt;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;)
    {
        const auto instance = ReadInstance(instancePath);
        if (!instance)
        {
            error = "The project Editor debug instance disappeared while preparing CoreCLR.";
            return std::nullopt;
        }
        state = SendRequest(instance->endpoint, "GetDebugState", 500);
        if (state && state->value("managedReady", false))
            return state;
        if (state && state->value("state", "") == "Failed")
        {
            error = state->value("message", "C# Debug preparation failed.");
            return std::nullopt;
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            error = state ? state->value("message", "Timed out preparing C# Debug.") :
                "Timed out querying the project Editor debug endpoint.";
            return std::nullopt;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

std::optional<Json> ReadLiveInstance(const Manifest& manifest, const int timeoutMs, std::string& error)
{
    const auto instancePath = manifest.workspaceRoot / "EditorInstance.json";
    const auto instance = ReadInstance(instancePath);
    if (!instance || !IsMatchingInstance(manifest, *instance))
    {
        error = "The project Editor is not running or belongs to a different project.";
        return std::nullopt;
    }
    const auto ping = SendRequest(instance->endpoint, "Ping", timeoutMs);
    if (!ping || ping->value("projectId", "") != manifest.projectId)
    {
        error = "The project Editor debug endpoint did not respond with the expected project identity.";
        return std::nullopt;
    }
    return ping;
}

std::optional<Json> ExecuteAction(
    const Manifest& manifest,
    const std::string& action,
    const int timeoutMs,
    const uint64_t afterSequence,
    const uint32_t waitMs,
    const std::string& configuration,
    std::string& error)
{
    if (action == "prepare")
    {
        // Managed-only F5 still needs the Editor binary selected by the
        // active VS configuration. The managed build remains Debug-capable;
        // this only chooses the native Editor/ALC host to attach to.
        const auto selectedManifest = ManifestForConfiguration(manifest, configuration);
        return EnsureReady(selectedManifest, timeoutMs, error);
    }

    if (action == "build-native")
    {
        const auto nativeManifest = ManifestForConfiguration(manifest, configuration);
        if (!PrepareNativeEditor(nativeManifest, configuration, error))
            return std::nullopt;
        const auto nativeState = ReadNativeInstanceState(nativeManifest);
        return Json{
            {"ok", true},
            {"configuration", configuration},
            {"target", manifest.nativeTarget},
            {"editorExecutable", nativeManifest.editorExecutable.string()},
            {"editorBuildId", FileStamp(nativeManifest.editorExecutable)},
            {"reusedRunningEditor", nativeState.running}};
    }

    if (action == "prepare-mixed")
    {
        const auto nativeManifest = ManifestForConfiguration(manifest, configuration);
        if (!nativeManifest.mixedDebugAvailable)
        {
            error = "Mixed debugging is unavailable: matching C++ source and PDB were not found.";
            return std::nullopt;
        }
        if (!PrepareNativeEditor(nativeManifest, configuration, error))
            return std::nullopt;
        return EnsureReady(nativeManifest, timeoutMs, error);
    }

    if (action == "native-status")
    {
        const auto nativeManifest = ManifestForConfiguration(manifest, configuration);
        const auto nativeState = ReadNativeInstanceState(nativeManifest);
        return Json{
            {"ok", true},
            {"engineSourceAvailable", manifest.engineSourceAvailable},
            {"nativeSymbolsAvailable", nativeManifest.nativeSymbolsAvailable},
            {"mixedDebugAvailable", nativeManifest.mixedDebugAvailable},
            {"editorExecutable", nativeManifest.editorExecutable.string()},
            {"editorBuildId", FileStamp(nativeManifest.editorExecutable)},
            {"manifestBuildId", nativeManifest.nativeBuildId},
            {"configuration", configuration},
            {"editorRunning", nativeState.running},
            {"editorProcessId", nativeState.processId},
            {"debuggerAttached", nativeState.debuggerAttached},
            {"nativeBinaryCurrent", nativeState.binaryCurrent},
            {"restartRequired", nativeState.restartRequired}};
    }

    const auto live = ReadLiveInstance(manifest, timeoutMs, error);
    if (!live)
        return std::nullopt;
    const auto endpoint = manifest.workspaceRoot / "EditorInstance.json";
    const auto instance = ReadInstance(endpoint);
    if (!instance)
    {
        error = "The project Editor instance record disappeared.";
        return std::nullopt;
    }
    if (action == "status" || action == "focus")
    {
        if (action == "focus")
            return SendRequest(instance->endpoint, "Focus", timeoutMs);
        return SendRequest(instance->endpoint, "GetDebugState", timeoutMs);
    }
    if (action == "watch-events")
    {
        return SendJsonRequest(instance->endpoint, Json{
            {"command", "ReadEvents"},
            {"arguments", Json{{"afterSequence", afterSequence}, {"waitMs", waitMs}}}}, timeoutMs);
    }

    const std::map<std::string, std::string> commands = {
        {"build", "BuildManagedScripts"},
        {"play", "EnterPlay"},
        {"pause", "PausePlay"},
        {"resume", "ResumePlay"},
        {"stop", "StopPlay"}};
    const auto command = commands.find(action);
    if (command == commands.end())
    {
        error = "Unknown Nullus debug action: " + action;
        return std::nullopt;
    }
    return SendRequest(instance->endpoint, command->second, timeoutMs);
}
}

int main(int argc, char** argv)
{
    std::filesystem::path manifestPath;
    std::string action = "prepare";
    uint64_t afterSequence = 0u;
    uint32_t waitMs = 25000u;
    std::string configuration = "Debug";
    // The first Editor launch may restore a large scene before its endpoint
    // is registered. Keep the default long enough for that one-time startup;
    // callers can still pass a shorter project-specific timeout.
    int timeoutMs = 120000;
    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index];
        if ((argument == "--manifest" || argument == "-m") && index + 1 < argc)
            manifestPath = argv[++index];
        else if (argument == "--action" && index + 1 < argc)
            action = argv[++index];
        else if (argument == "--after" && index + 1 < argc)
            afterSequence = std::strtoull(argv[++index], nullptr, 10);
        else if (argument == "--wait-ms" && index + 1 < argc)
            waitMs = static_cast<uint32_t>((std::min)(30000, (std::max)(0, std::atoi(argv[++index]))));
        else if (argument == "--timeout-ms" && index + 1 < argc)
            timeoutMs = (std::max)(1000, std::atoi(argv[++index]));
        else if (argument == "--configuration" && index + 1 < argc)
            configuration = argv[++index];
        else if (argument == "--help" || argument == "-h")
        {
            std::cout << "Usage: NullusDebugBroker --manifest <Library/IDE/Nullus.Debug.json> "
                "[--action prepare|prepare-mixed|build-native|status|focus|build|play|pause|resume|stop|watch-events] "
                "[--configuration Debug|Release] "
                "[--after N] [--wait-ms N] [--timeout-ms N]\n";
            return 0;
        }
    }
    if (manifestPath.empty())
    {
        std::cerr << "--manifest is required.\n";
        return 2;
    }

    std::string error;
    const auto manifest = ReadManifest(manifestPath, error);
    if (!manifest)
    {
        std::cerr << error << '\n';
        return 3;
    }
    const auto result = ExecuteAction(*manifest, action, timeoutMs, afterSequence, waitMs, configuration, error);
    if (!result)
    {
        Json failure = {
            {"ok", false},
            {"error", error},
            {"action", action},
            {"projectId", manifest->projectId}};
        if (error.find("already attached by another Visual Studio session") != std::string::npos)
        {
            failure["code"] = "EditorDebuggerOccupied";
            failure["debuggerType"] = "VisualStudio";
            failure["suggestion"] = "Start Mixed debugging from Nullus.Project.sln.";
        }
        else if (error.find("older native build") != std::string::npos ||
            error.find("different Editor build") != std::string::npos)
        {
            failure["code"] = "EditorRestartRequired";
            failure["suggestion"] = "Close the running Editor and retry from the matching project workspace.";
        }
        else if (error.find("C++ source debugging is unavailable") != std::string::npos ||
            error.find("Mixed debugging is unavailable") != std::string::npos)
        {
            failure["code"] = "MixedDebugUnavailable";
        }
        const auto pidMarker = error.find("(PID ");
        if (pidMarker != std::string::npos)
        {
            const auto begin = pidMarker + 5u;
            const auto end = error.find(')', begin);
            if (end != std::string::npos)
            {
                try
                {
                    failure["processId"] = static_cast<uint32_t>(
                        std::stoul(error.substr(begin, end - begin)));
                }
                catch (...) { }
            }
        }
        std::cerr << failure.dump() << '\n';
        return 4;
    }
    if (action == "watch-events" && result->contains("events") && result->at("events").is_array())
    {
        for (const auto& event : result->at("events"))
            std::cout << event.dump() << '\n';
    }
    else
    {
        std::cout << result->dump() << '\n';
    }
    return 0;
}
