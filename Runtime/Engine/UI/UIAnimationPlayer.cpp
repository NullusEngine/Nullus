#include "UIAnimationPlayer.h"

#include <algorithm>
#include <cmath>

namespace NLS::Engine::UI
{
    UIAnimationPlayer::TrackHandle UIAnimationPlayer::AddOpacityTrack(
        UIElement& element,
        const float from,
        const float to,
        const float durationSeconds)
    {
        const TrackHandle handle { m_nextHandle++ };
        m_tracks.push_back({ handle, &element, from, to, std::max(0.0f, durationSeconds) });
        return handle;
    }

    bool UIAnimationPlayer::RemoveTrack(const TrackHandle handle)
    {
        const auto iterator = std::find_if(
            m_tracks.begin(), m_tracks.end(),
            [handle](const OpacityTrack& track) { return track.handle.value == handle.value; });
        if (iterator == m_tracks.end())
            return false;
        m_tracks.erase(iterator);
        return true;
    }

    void UIAnimationPlayer::Play(const bool loop, const bool unscaledTime)
    {
        m_loop = loop;
        m_unscaledTime = unscaledTime;
        m_playing = true;
    }

    void UIAnimationPlayer::Stop()
    {
        m_playing = false;
    }

    void UIAnimationPlayer::Tick(const float deltaSeconds)
    {
        if (!m_playing)
            return;
        m_time += std::max(0.0f, deltaSeconds);
        float longestDuration = 0.0f;
        for (const auto& track : m_tracks)
            longestDuration = std::max(longestDuration, track.duration);
        if (longestDuration <= 0.0f)
            m_time = 0.0f;

        for (auto& track : m_tracks)
        {
            if (!track.element)
                continue;
            const float normalized = track.duration <= 0.0f
                ? 1.0f
                : std::clamp(m_time / track.duration, 0.0f, 1.0f);
            auto style = track.element->GetStyle();
            style.opacity = track.from + (track.to - track.from) * normalized;
            track.element->SetStyle(style);
        }

        if (m_time >= longestDuration)
        {
            if (m_loop && longestDuration > 0.0f)
                m_time = std::fmod(m_time, longestDuration);
            else
                m_playing = false;
        }
    }

    void UIAnimationPlayer::Clear()
    {
        m_tracks.clear();
        m_time = 0.0f;
        m_playing = false;
    }
}
