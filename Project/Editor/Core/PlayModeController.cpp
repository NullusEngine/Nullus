#include "PlayModeController.h"
#include <algorithm>
#include <atomic>
#include <exception>

namespace NLS::Editor::Core
{
namespace { std::atomic<uint64_t> nextSession { 1 }; }
void PlayModeController::Enqueue(Request request) { std::lock_guard lock(m_requestsMutex);m_requests.push_back(request); }
void PlayModeController::RequestPlay() { Enqueue(Request::Play); }
void PlayModeController::RequestPause() { Enqueue(Request::Pause); }
void PlayModeController::RequestResume() { Enqueue(Request::Resume); }
void PlayModeController::RequestStep() { Enqueue(Request::Step); }
void PlayModeController::RequestStop() { Enqueue(Request::Stop); }
void PlayModeController::RequestTogglePlay() { Enqueue(Request::Toggle); }
bool PlayModeController::IsStopRequested() const
{
    std::lock_guard lock(m_requestsMutex);
    return std::find(m_requests.begin(), m_requests.end(), Request::Stop) != m_requests.end();
}
void PlayModeController::SetState(PlaySessionState state)
{
    if (m_state == state) return;
    m_state = state;
    StateChanged.Invoke(state);
}
bool PlayModeController::Stop()
{
    if (m_state == PlaySessionState::Edit) return true;
    SetState(PlaySessionState::Exiting);
    bool restored=false;
    try { restored=m_callbacks.end && m_callbacks.end(m_error); }
    catch(const std::exception& exception) {m_error=exception.what();}
    catch(...) {m_error="Unknown Play recovery failure.";}
    if (!restored)
    {
        if (m_error.empty()) m_error = "Play session could not restore the Edit scene.";
        SetState(PlaySessionState::Faulted);
        return false;
    }
    m_error.clear();
    SetState(PlaySessionState::Edit);
    return true;
}
void PlayModeController::ProcessRequests()
{
    if (m_processing) return;
    m_processing = true;
    try
    {
        // Process a snapshot. Requests from callbacks wait for another safe
        // boundary, except Stop, which can cancel entry before Playing is published.
        std::deque<Request> requests;
        { std::lock_guard lock(m_requestsMutex);requests.swap(m_requests); }
        for (auto request : requests)
        {
            if (request == Request::Toggle)
                request = IsActive() ? Request::Stop : Request::Play;
            if (request == Request::Stop) { Stop(); continue; }
            if (request == Request::Play && m_state == PlaySessionState::Edit)
            {
                m_error.clear();
                m_sessionId = nextSession.fetch_add(1);
                SetState(PlaySessionState::Entering);
                if (!m_callbacks.begin || !m_callbacks.begin(m_sessionId, m_error))
                {
                    const auto entryError = m_error;
                    if (Stop()) m_error = entryError;
                    continue;
                }
                if (IsStopRequested()) { Stop(); continue; }
                SetState(PlaySessionState::Playing);
            }
            else if ((request == Request::Resume || request == Request::Play) && m_state == PlaySessionState::Paused)
            {
                if (m_callbacks.pause) m_callbacks.pause(false);
                SetState(PlaySessionState::Playing);
            }
            else if (request == Request::Pause && m_state == PlaySessionState::Playing)
            {
                if (m_callbacks.pause) m_callbacks.pause(true);
                SetState(PlaySessionState::Paused);
            }
            else if (request == Request::Step && (m_state == PlaySessionState::Playing || m_state == PlaySessionState::Paused))
            {
                if (m_callbacks.pause) m_callbacks.pause(true);
                SetState(PlaySessionState::Stepping);
                // One request means one game frame, even when input repeats.
                // Later Stop requests still take precedence over this step.
            }
        }
    }
    catch (const std::exception& exception)
    {
        m_error = exception.what();
        const auto failure=m_error;
        if(m_state==PlaySessionState::Entering && Stop())m_error=failure;
        else if(m_state!=PlaySessionState::Edit)SetState(PlaySessionState::Faulted);
    }
    catch (...) { m_error = "Unknown Play transition failure.";const auto failure=m_error;
        if(m_state==PlaySessionState::Entering && Stop())m_error=failure;
        else if(m_state!=PlaySessionState::Edit)SetState(PlaySessionState::Faulted); }
    m_processing = false;
}
void PlayModeController::CompleteStep()
{
    if (m_state == PlaySessionState::Stepping) SetState(PlaySessionState::Paused);
}
}
