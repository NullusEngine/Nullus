#pragma once

#include "UIElement.h"
#include "EngineDef.h"

#include <cstdint>
#include <vector>

namespace NLS::Engine::UI
{
    class NLS_ENGINE_API UIAnimationPlayer final
    {
    public:
        struct TrackHandle
        {
            uint64_t value = 0u;
            [[nodiscard]] bool IsValid() const { return value != 0u; }
        };

        TrackHandle AddOpacityTrack(UIElement& element, float from, float to, float durationSeconds);
        bool RemoveTrack(TrackHandle handle);
        void Play(bool loop = false, bool unscaledTime = true);
        void Stop();
        void Tick(float deltaSeconds);
        [[nodiscard]] bool IsPlaying() const { return m_playing; }
        [[nodiscard]] float GetTime() const { return m_time; }
        void Clear();

    private:
        struct OpacityTrack
        {
            TrackHandle handle;
            UIElement* element = nullptr;
            float from = 1.0f;
            float to = 1.0f;
            float duration = 0.0f;
        };

        std::vector<OpacityTrack> m_tracks;
        uint64_t m_nextHandle = 1u;
        float m_time = 0.0f;
        bool m_playing = false;
        bool m_loop = false;
        bool m_unscaledTime = true;
    };
}
