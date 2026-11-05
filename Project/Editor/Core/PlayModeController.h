#pragma once

#include <Eventing/Event.h>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <mutex>
#include <atomic>

namespace NLS::Editor::Core
{
enum class PlaySessionState : uint8_t { Edit, Entering, Playing, Paused, Stepping, Exiting, Faulted };

// All producers enqueue commands. Only the editor's main-thread frame boundary
// may publish a transition or invoke the scene/script lifecycle callbacks.
class PlayModeController final
{
public:
    struct Callbacks
    {
        std::function<bool(uint64_t, std::string&)> begin;
        std::function<bool(std::string&)> end;
        std::function<void(bool)> pause;
    };
    void SetCallbacks(Callbacks callbacks) { m_callbacks = std::move(callbacks); }
    void RequestPlay();
    void RequestPause();
    void RequestResume();
    void RequestStep();
    void RequestStop();
    void RequestTogglePlay();
    void ProcessRequests();
    void CompleteStep();
    [[nodiscard]] PlaySessionState GetState() const { return m_state; }
    [[nodiscard]] uint64_t GetSessionId() const { return m_sessionId; }
    [[nodiscard]] bool IsActive() const { return m_state != PlaySessionState::Edit; }
    [[nodiscard]] bool IsTransitioning() const { return m_state == PlaySessionState::Entering || m_state == PlaySessionState::Exiting; }
    [[nodiscard]] bool IsStopRequested() const;
    [[nodiscard]] const std::string& GetError() const { return m_error; }
    Event<PlaySessionState> StateChanged;
private:
    enum class Request { Play, Pause, Resume, Step, Stop, Toggle };
    void Enqueue(Request request);
    void SetState(PlaySessionState state);
    bool Stop();
    Callbacks m_callbacks;
    std::deque<Request> m_requests;
    mutable std::mutex m_requestsMutex;
    std::atomic<PlaySessionState> m_state = PlaySessionState::Edit;
    std::atomic<uint64_t> m_sessionId = 0;
    std::string m_error;
    bool m_processing = false;
};
}
