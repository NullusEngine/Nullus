#include "EditorDebugEndpoint.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace NLS::Editor::Debugging
{
namespace
{
using Json = nlohmann::json;

uint64_t ProcessStartTime()
{
#ifdef _WIN32
    FILETIME creation = {}, exit = {}, kernel = {}, user = {};
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user) == FALSE)
        return 0u;
    ULARGE_INTEGER value = {};
    value.LowPart = creation.dwLowDateTime;
    value.HighPart = creation.dwHighDateTime;
    return value.QuadPart;
#else
    // Linux exposes a monotonic process start tick in /proc. Unlike a wall
    // clock, it remains comparable when the Broker checks PID reuse.
    std::ifstream stat("/proc/self/stat");
    std::string line;
    if (stat && std::getline(stat, line))
    {
        const auto closingName = line.rfind(") ");
        if (closingName != std::string::npos)
        {
            std::istringstream fields(line.substr(closingName + 2u));
            std::string value;
            // The substring starts at field 3; field 22 is the 20th value.
            for (int index = 0; index < 20; ++index)
            {
                if (!(fields >> value))
                    break;
            }
            try
            {
                if (!value.empty())
                    return std::stoull(value);
            }
            catch (...) { }
        }
    }
    return static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

std::string JsonLine(const Json& value)
{
    return value.dump() + "\n";
}

std::string MakeSessionId(const std::string& projectId, const uint32_t processId, const uint64_t startTime)
{
    uint64_t hash = 1469598103934665603ull;
    for (const auto character : projectId)
    {
        hash ^= static_cast<unsigned char>(character);
        hash *= 1099511628211ull;
    }
    hash ^= processId;
    hash *= 1099511628211ull;
    hash ^= startTime;
    hash *= 1099511628211ull;
    std::ostringstream output;
    output << std::hex << hash;
    return output.str();
}

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
    return std::to_string(size) + ":" +
        std::to_string(timestamp.time_since_epoch().count());
}

#ifndef _WIN32
bool WriteAll(const int descriptor, const std::string& text)
{
    size_t offset = 0u;
    while (offset < text.size())
    {
        const auto written = ::write(descriptor, text.data() + offset, text.size() - offset);
        if (written <= 0)
            return false;
        offset += static_cast<size_t>(written);
    }
    return true;
}

bool ReadLine(const int descriptor, std::string& output)
{
    output.clear();
    char character = 0;
    while (output.size() < 64u * 1024u)
    {
        const auto readCount = ::read(descriptor, &character, 1u);
        if (readCount <= 0)
            return false;
        if (character == '\n')
            return true;
        output.push_back(character);
    }
    return false;
}
#endif
}

EditorDebugEndpoint::EditorDebugEndpoint(
    std::filesystem::path projectRoot,
    std::string projectId,
    std::filesystem::path editorExecutable)
    : m_projectRoot(std::move(projectRoot))
    , m_projectId(std::move(projectId))
    , m_editorExecutable(std::move(editorExecutable))
{
    m_instanceRecordPath = m_projectRoot / "Library" / "IDE" / "EditorInstance.json";
    m_info.projectId = m_projectId;
    m_info.projectRoot = m_projectRoot;
    m_info.editorExecutable = m_editorExecutable;
    m_info.editorBuildId = FileStamp(m_editorExecutable);
    m_info.capabilities = {
        "managed-debug",
        "managed-build",
        "play-control",
        "diagnostic-events",
        "workspace-events"};
#ifdef _WIN32
    m_info.endpoint = "\\\\.\\pipe\\Nullus.Editor." + m_projectId.substr(0u, 48u);
#else
    m_info.endpoint = (m_projectRoot / "Library" / "IDE" / "Editor.sock").string();
#endif
    m_info.processId =
#ifdef _WIN32
        static_cast<uint32_t>(GetCurrentProcessId());
#else
        static_cast<uint32_t>(getpid());
#endif
    m_info.processStartTime = ProcessStartTime();
    m_info.sessionId = MakeSessionId(m_projectId, m_info.processId, m_info.processStartTime);
    m_instanceLockAcquired = AcquireInstanceLock(m_instanceLockError);
}

EditorDebugEndpoint::~EditorDebugEndpoint()
{
    Stop();
    ReleaseInstanceLock();
}

bool EditorDebugEndpoint::AcquireInstanceLock(std::string& errorMessage)
{
    errorMessage.clear();
#ifdef _WIN32
    const auto name = std::string("Local\\Nullus.Editor.") + m_projectId;
    const auto handle = CreateMutexA(nullptr, TRUE, name.c_str());
    if (handle == nullptr)
    {
        errorMessage = "Unable to create the project Editor instance lock (error " +
            std::to_string(GetLastError()) + ").";
        return false;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(handle);
        errorMessage = "Another Nullus Editor already owns this project instance.";
        return false;
    }
    m_instanceLockHandle = handle;
    return true;
#else
    std::error_code error;
    const auto lockPath = m_projectRoot / "Library" / "IDE" / "Editor.lock";
    std::filesystem::create_directories(lockPath.parent_path(), error);
    if (error)
    {
        errorMessage = "Unable to create the project Editor lock directory: " + error.message();
        return false;
    }
    const auto descriptor = ::open(lockPath.c_str(), O_CREAT | O_RDWR, 0600);
    if (descriptor < 0)
    {
        errorMessage = "Unable to open the project Editor instance lock: " +
            std::string(std::strerror(errno));
        return false;
    }
    if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0)
    {
        errorMessage = "Another Nullus Editor already owns this project instance.";
        ::close(descriptor);
        return false;
    }
    m_instanceLockHandle = descriptor;
    return true;
#endif
}

void EditorDebugEndpoint::ReleaseInstanceLock()
{
#ifdef _WIN32
    if (m_instanceLockHandle != nullptr)
    {
        ReleaseMutex(static_cast<HANDLE>(m_instanceLockHandle));
        CloseHandle(static_cast<HANDLE>(m_instanceLockHandle));
        m_instanceLockHandle = nullptr;
    }
#else
    if (m_instanceLockHandle >= 0)
    {
        (void)::flock(m_instanceLockHandle, LOCK_UN);
        ::close(m_instanceLockHandle);
        m_instanceLockHandle = -1;
    }
#endif
    m_instanceLockAcquired = false;
}

bool EditorDebugEndpoint::WriteInstanceRecord(std::string& errorMessage) const
{
    std::error_code error;
    std::filesystem::create_directories(m_instanceRecordPath.parent_path(), error);
    if (error)
    {
        errorMessage = "Unable to create the project debug workspace: " + error.message();
        return false;
    }
    const Json record = {
        {"protocolVersion", m_info.protocolVersion},
        {"protocolMajor", m_info.protocolVersion},
        {"protocolMinor", m_info.protocolMinor},
        {"processId", m_info.processId},
        {"processStartTime", m_info.processStartTime},
        {"projectId", m_info.projectId},
        {"sessionId", m_info.sessionId},
        {"projectRoot", m_info.projectRoot.string()},
        {"editorExecutable", m_info.editorExecutable.string()},
        {"editorBuildId", m_info.editorBuildId},
        {"endpoint", m_info.endpoint},
        {"capabilities", m_info.capabilities}
    };
    std::ofstream output(m_instanceRecordPath, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        errorMessage = "Unable to write " + m_instanceRecordPath.string();
        return false;
    }
    output << record.dump(4) << '\n';
    if (!output)
    {
        errorMessage = "Unable to flush " + m_instanceRecordPath.string();
        return false;
    }
    return true;
}

void EditorDebugEndpoint::RemoveInstanceRecord() const
{
    std::error_code error;
    auto record = std::filesystem::exists(m_instanceRecordPath, error)
        ? std::ifstream(m_instanceRecordPath, std::ios::binary)
        : std::ifstream{};
    if (record)
    {
        const Json value = Json::parse(record, nullptr, false);
        if (!value.is_discarded() && value.value("processId", 0u) == m_info.processId &&
            value.value("processStartTime", 0ull) == m_info.processStartTime)
        {
            std::filesystem::remove(m_instanceRecordPath, error);
        }
    }
}

bool EditorDebugEndpoint::Start(
    RequestHandler handler,
    std::string& errorMessage,
    ResponseDeliveredHandler responseDeliveredHandler)
{
    if (m_running.load())
        return true;
    if (!m_instanceLockAcquired)
    {
        errorMessage = m_instanceLockError.empty()
            ? "Another Nullus Editor already owns this project instance."
            : m_instanceLockError;
        return false;
    }
    if (!handler)
    {
        errorMessage = "Editor debug endpoint requires a request handler.";
        return false;
    }
    m_handler = std::move(handler);
    m_responseDeliveredHandler = std::move(responseDeliveredHandler);
    m_stopRequested.store(false);

#ifdef _WIN32
    m_serverHandle = CreateNamedPipeA(
        m_info.endpoint.c_str(),
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1u,
        64u * 1024u,
        64u * 1024u,
        0u,
        nullptr);
    if (m_serverHandle == INVALID_HANDLE_VALUE)
    {
        m_serverHandle = nullptr;
        errorMessage = "Unable to create the Editor debug named pipe (error " +
            std::to_string(GetLastError()) + ").";
        return false;
    }
#else
    std::error_code error;
    std::filesystem::create_directories(m_instanceRecordPath.parent_path(), error);
    if (error)
    {
        errorMessage = "Unable to create the Editor debug socket directory: " + error.message();
        return false;
    }
    std::filesystem::remove(m_info.endpoint, error);
    m_serverHandle = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (m_serverHandle < 0)
    {
        errorMessage = "Unable to create the Editor debug socket: " + std::string(std::strerror(errno));
        return false;
    }
    sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    if (m_info.endpoint.size() >= sizeof(address.sun_path))
    {
        errorMessage = "Editor debug socket path is too long: " + m_info.endpoint;
        ::close(m_serverHandle);
        m_serverHandle = -1;
        return false;
    }
    std::copy(m_info.endpoint.begin(), m_info.endpoint.end(), address.sun_path);
    if (::bind(m_serverHandle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(m_serverHandle, 4) != 0)
    {
        errorMessage = "Unable to bind the Editor debug socket: " + std::string(std::strerror(errno));
        ::close(m_serverHandle);
        m_serverHandle = -1;
        return false;
    }
#endif

    if (!WriteInstanceRecord(errorMessage))
    {
#ifdef _WIN32
        CloseHandle(static_cast<HANDLE>(m_serverHandle));
        m_serverHandle = nullptr;
#else
        ::close(m_serverHandle);
        m_serverHandle = -1;
        std::error_code removeError;
        std::filesystem::remove(m_info.endpoint, removeError);
#endif
        return false;
    }

    m_running.store(true);
    m_thread = std::thread(&EditorDebugEndpoint::Serve, this);
    return true;
}

void EditorDebugEndpoint::Stop()
{
    if (!m_running.exchange(false))
    {
        if (m_thread.joinable())
            m_thread.join();
        return;
    }
    m_stopRequested.store(true);
    m_queueCondition.notify_all();
    m_eventCondition.notify_all();
#ifdef _WIN32
    if (m_serverHandle != nullptr)
    {
        // Wake ConnectNamedPipe, but keep the handle alive until Serve() has
        // left the transport loop. Closing it from this thread while the
        // server thread is writing a response is a shutdown race.
        CancelIoEx(static_cast<HANDLE>(m_serverHandle), nullptr);
        DisconnectNamedPipe(static_cast<HANDLE>(m_serverHandle));
    }
#else
    if (m_serverHandle >= 0)
    {
        ::shutdown(m_serverHandle, SHUT_RDWR);
    }
#endif
    if (m_thread.joinable())
        m_thread.join();
#ifdef _WIN32
    if (m_serverHandle != nullptr)
    {
        CloseHandle(static_cast<HANDLE>(m_serverHandle));
        m_serverHandle = nullptr;
    }
#else
    if (m_serverHandle >= 0)
    {
        ::close(m_serverHandle);
        m_serverHandle = -1;
    }
    std::error_code error;
    std::filesystem::remove(m_info.endpoint, error);
#endif
    {
        std::lock_guard lock(m_queueMutex);
        for (auto& request : m_requests)
        {
            std::lock_guard requestLock(request->mutex);
            request->response = {{"ok", false}, {"error", "Editor debug endpoint stopped."}};
            request->complete = true;
            request->condition.notify_one();
        }
        m_requests.clear();
        m_deliveredResponses.clear();
    }
    RemoveInstanceRecord();
}

void EditorDebugEndpoint::Poll()
{
    for (;;)
    {
        DeliveredResponse delivered;
        bool hasDeliveredResponse = false;
        std::shared_ptr<PendingRequest> request;
        {
            std::lock_guard lock(m_queueMutex);
            if (!m_deliveredResponses.empty())
            {
                delivered = std::move(m_deliveredResponses.front());
                m_deliveredResponses.pop_front();
                hasDeliveredResponse = true;
            }
            else if (!m_requests.empty())
            {
                request = std::move(m_requests.front());
                m_requests.pop_front();
            }
            else
            {
                return;
            }
        }
        if (hasDeliveredResponse)
        {
            if (m_responseDeliveredHandler)
                m_responseDeliveredHandler(delivered.request, delivered.response);
            continue;
        }
        Json response = Dispatch(request->request);
        {
            std::lock_guard lock(request->mutex);
            request->response = std::move(response);
            request->complete = true;
        }
        request->condition.notify_one();
    }
}

EditorDebugEndpoint::Json EditorDebugEndpoint::Dispatch(const Json& request)
{
    const auto requestId = request.is_object() ? request.value("requestId", "") : "";
    Json data = {
        {"projectId", m_projectId},
        {"sessionId", m_info.sessionId},
        {"processId", m_info.processId},
        {"processStartTime", m_info.processStartTime},
        {"projectRoot", m_info.projectRoot.string()},
        {"editorExecutable", m_info.editorExecutable.string()},
        {"editorBuildId", m_info.editorBuildId},
        {"endpoint", m_info.endpoint},
        {"capabilities", m_info.capabilities}
    };
    bool ok = true;
    std::string error;
    if (!m_handler)
    {
        ok = false;
        error = "Editor debug endpoint has no request handler.";
    }
    else if (!request.is_object())
    {
        ok = false;
        error = "Editor debug request must be a JSON object.";
    }
    else
    {
        const auto result = m_handler(request);
        ok = result.value("ok", true);
        if (!ok)
            error = result.value("error", "Editor debug command failed.");
        for (auto iterator = result.begin(); iterator != result.end(); ++iterator)
        {
            if (iterator.key() != "ok" && iterator.key() != "error")
                data[iterator.key()] = iterator.value();
        }
    }

    Json response = {
        {"requestId", requestId},
        {"protocolVersion", Json{{"major", m_info.protocolVersion}, {"minor", m_info.protocolMinor}}},
        {"ok", ok},
        {"data", data}
    };
    if (!ok)
        response["error"] = error;

    // Keep the flattened fields for one minor release so older VS Code
    // clients can talk to a schema-v2 Editor while they upgrade. New clients
    // must consume response.data and response.error.
    for (auto iterator = data.begin(); iterator != data.end(); ++iterator)
        response[iterator.key()] = iterator.value();
    response["protocolMajor"] = m_info.protocolVersion;
    response["protocolMinor"] = m_info.protocolMinor;
    return response;
}

void EditorDebugEndpoint::PublishEvent(Json event)
{
    if (!event.is_object())
        return;
    std::lock_guard lock(m_eventMutex);
    event["sequence"] = m_nextEventSequence++;
    event["sessionId"] = m_info.sessionId;
    if (!event.contains("protocolVersion"))
        event["protocolVersion"] = m_info.protocolVersion;
    m_events.push_back(std::move(event));
    while (m_events.size() > 512u)
        m_events.pop_front();
    m_eventCondition.notify_all();
}

EditorDebugEndpoint::Json EditorDebugEndpoint::ReadEvents(const Json& request)
{
    const auto arguments = request.is_object() ? request.value("arguments", Json::object()) : Json::object();
    const auto& values = arguments.is_object() ? arguments : request;
    const auto after = values.value("afterSequence", 0ull);
    const auto waitMs = (std::min)(values.value("waitMs", 0u), 30000u);
    std::unique_lock lock(m_eventMutex);
    const auto hasEvents = [this, after]
    {
        return m_stopRequested.load() || (!m_events.empty() && m_events.back().value("sequence", 0ull) > after);
    };
    if (waitMs != 0u && !hasEvents())
        m_eventCondition.wait_for(lock, std::chrono::milliseconds(waitMs), hasEvents);

    Json events = Json::array();
    for (const auto& event : m_events)
    {
        if (event.value("sequence", 0ull) > after)
            events.push_back(event);
    }
    const bool ok = !m_stopRequested.load();
    Json data = {
        {"events", std::move(events)},
        {"nextSequence", m_nextEventSequence - 1u},
        {"projectId", m_projectId},
        {"sessionId", m_info.sessionId}};
    Json response = {
        {"requestId", request.is_object() ? request.value("requestId", "") : ""},
        {"protocolVersion", Json{{"major", m_info.protocolVersion}, {"minor", m_info.protocolMinor}}},
        {"ok", ok},
        {"data", data},
        {"events", data["events"]},
        {"nextSequence", data["nextSequence"]},
        {"projectId", m_projectId},
        {"sessionId", m_info.sessionId},
        {"protocolMajor", m_info.protocolVersion},
        {"protocolMinor", m_info.protocolMinor}};
    if (!ok)
        response["error"] = "Editor debug endpoint stopped.";
    return response;
}

void EditorDebugEndpoint::Serve()
{
#ifdef _WIN32
    const auto pipe = static_cast<HANDLE>(m_serverHandle);
    const auto disconnectClient = [pipe]
    {
        (void)DisconnectNamedPipe(pipe);
    };
    const auto writeResponse = [pipe, &disconnectClient](const std::string& line)
    {
        DWORD written = 0u;
        const bool wrote = WriteFile(
            pipe,
            line.data(),
            static_cast<DWORD>(line.size()),
            &written,
            nullptr) != FALSE && written == line.size();
        // FlushFileBuffers waits for the client to consume the response. Do
        // not call it after a timed-out Broker has already closed its handle.
        if (wrote)
            (void)FlushFileBuffers(pipe);
        disconnectClient();
        return wrote;
    };
    while (!m_stopRequested.load())
    {
        const auto connectResult = ConnectNamedPipe(pipe, nullptr);
        const auto connectError = connectResult != FALSE ? ERROR_SUCCESS : GetLastError();
        const auto connected = connectResult != FALSE || connectError == ERROR_PIPE_CONNECTED;
        if (!connected)
        {
            if (m_stopRequested.load() || connectError == ERROR_OPERATION_ABORTED ||
                connectError == ERROR_INVALID_HANDLE)
                break;

            // A client can connect and time out before the server reaches
            // ConnectNamedPipe. Windows then reports ERROR_NO_DATA and leaves
            // this pipe instance in a closing state. Disconnect it and keep
            // serving instead of permanently losing the project endpoint.
            disconnectClient();
            Sleep(1u);
            continue;
        }
        std::string requestText;
        char buffer[4096] = {};
        DWORD readBytes = 0u;
        if (ReadFile(pipe, buffer, sizeof(buffer) - 1u, &readBytes, nullptr) == FALSE || readBytes == 0u)
        {
            disconnectClient();
            continue;
        }
        requestText.assign(buffer, buffer + readBytes);
        const auto newline = requestText.find('\n');
        if (newline != std::string::npos)
            requestText.resize(newline);
        const Json request = Json::parse(requestText, nullptr, false);
        const std::string command = request.is_object() ? request.value("command", "") : "";
        if (command == "ReadEvents")
        {
            const auto response = ReadEvents(request);
            (void)writeResponse(JsonLine(response));
            continue;
        }
        auto pending = std::make_shared<PendingRequest>();
        pending->request = request.is_object() ? request : Json::object();
        {
            std::lock_guard lock(m_queueMutex);
            m_requests.push_back(pending);
        }
        m_queueCondition.notify_one();
        Json response;
        {
            std::unique_lock lock(pending->mutex);
            pending->condition.wait(lock, [this, &pending]
            {
                return pending->complete || m_stopRequested.load();
            });
            response = pending->response;
        }
        if (writeResponse(JsonLine(response)))
        {
            std::lock_guard lock(m_queueMutex);
            m_deliveredResponses.push_back({pending->request, response});
        }
    }
#else
    while (!m_stopRequested.load())
    {
        const int client = ::accept(m_serverHandle, nullptr, nullptr);
        if (client < 0)
        {
            if (m_stopRequested.load())
                break;
            continue;
        }
        std::string requestText;
        const bool read = ReadLine(client, requestText);
        const Json request = read ? Json::parse(requestText, nullptr, false) : Json();
        const std::string command = request.is_object() ? request.value("command", "") : "";
        if (command == "ReadEvents")
        {
            (void)WriteAll(client, JsonLine(ReadEvents(request)));
            ::close(client);
            continue;
        }
        auto pending = std::make_shared<PendingRequest>();
        pending->request = request.is_object() ? request : Json::object();
        {
            std::lock_guard lock(m_queueMutex);
            m_requests.push_back(pending);
        }
        m_queueCondition.notify_one();
        Json response;
        {
            std::unique_lock lock(pending->mutex);
            pending->condition.wait(lock, [this, &pending]
            {
                return pending->complete || m_stopRequested.load();
            });
            response = pending->response;
        }
        const bool wrote = WriteAll(client, JsonLine(response));
        ::close(client);
        if (wrote)
        {
            std::lock_guard lock(m_queueMutex);
            m_deliveredResponses.push_back({pending->request, response});
        }
    }
#endif
}
}
