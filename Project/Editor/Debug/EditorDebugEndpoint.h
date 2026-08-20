#pragma once

#include <Json/json.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace NLS::Editor::Debugging
{
struct EditorDebugInstanceInfo
{
    uint32_t protocolVersion = 2u;
    uint32_t protocolMinor = 0u;
    uint32_t processId = 0u;
    uint64_t processStartTime = 0u;
    std::string projectId;
    std::string sessionId;
    std::filesystem::path projectRoot;
    std::filesystem::path editorExecutable;
    std::string editorBuildId;
    std::string endpoint;
    std::vector<std::string> capabilities;
};

// Project-local control endpoint used by the IDE F5 integrations. The
// transport thread never calls an Editor callback directly; Poll() executes
// queued commands on the Editor thread and wakes the waiting transport.
class EditorDebugEndpoint final
{
public:
    using Json = nlohmann::json;
    using RequestHandler = std::function<Json(const Json& request)>;
    using ResponseDeliveredHandler = std::function<void(const Json& request, const Json& response)>;

    EditorDebugEndpoint(
        std::filesystem::path projectRoot,
        std::string projectId,
        std::filesystem::path editorExecutable);
    ~EditorDebugEndpoint();

    EditorDebugEndpoint(const EditorDebugEndpoint&) = delete;
    EditorDebugEndpoint& operator=(const EditorDebugEndpoint&) = delete;

    bool Start(
        RequestHandler handler,
        std::string& errorMessage,
        ResponseDeliveredHandler responseDeliveredHandler = {});
    void Poll();
    void Stop();

    // Event records are published by the Editor main thread and can be read by
    // the transport thread without invoking gameplay or Editor APIs. This
    // keeps ReadEvents safe for long-polling IDE clients.
    void PublishEvent(Json event);

    const EditorDebugInstanceInfo& GetInfo() const { return m_info; }
    bool IsRunning() const { return m_running.load(); }
    bool HasInstanceLock() const { return m_instanceLockAcquired; }
    const std::string& GetInstanceLockError() const { return m_instanceLockError; }

private:
    struct PendingRequest
    {
        Json request;
        Json response;
        bool complete = false;
        std::mutex mutex;
        std::condition_variable condition;
    };

    struct DeliveredResponse
    {
        Json request;
        Json response;
    };

    void Serve();
    Json Dispatch(const Json& request);
    Json ReadEvents(const Json& request);
    bool AcquireInstanceLock(std::string& errorMessage);
    void ReleaseInstanceLock();
    bool WriteInstanceRecord(std::string& errorMessage) const;
    void RemoveInstanceRecord() const;

    std::filesystem::path m_projectRoot;
    std::filesystem::path m_instanceRecordPath;
    std::string m_projectId;
    std::filesystem::path m_editorExecutable;
    EditorDebugInstanceInfo m_info;
    RequestHandler m_handler;
    ResponseDeliveredHandler m_responseDeliveredHandler;
    bool m_instanceLockAcquired = false;
    std::string m_instanceLockError;
    std::thread m_thread;
    std::atomic<bool> m_running = false;
    std::atomic<bool> m_stopRequested = false;
    mutable std::mutex m_queueMutex;
    std::condition_variable m_queueCondition;
    std::deque<std::shared_ptr<PendingRequest>> m_requests;
    std::deque<DeliveredResponse> m_deliveredResponses;
    mutable std::mutex m_eventMutex;
    std::condition_variable m_eventCondition;
    std::deque<Json> m_events;
    uint64_t m_nextEventSequence = 1u;
#ifdef _WIN32
    void* m_serverHandle = nullptr;
    void* m_instanceLockHandle = nullptr;
#else
    int m_serverHandle = -1;
    int m_instanceLockHandle = -1;
#endif
};
}
