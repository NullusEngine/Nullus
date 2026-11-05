#pragma once
#include <Math/Vector2.h>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace NLS::Editor::Core
{
enum class GameViewSizeMode : uint8_t { FreeAspect, AspectRatio, FixedResolution };
struct GameViewPresentation
{
    uint32_t width = 0, height = 0;
    Maths::Vector2 imageMin, imageMax, clipMin, clipMax;
    uint64_t generation = 0;
    uint64_t sessionId = 0;
    uint32_t display = 0;
    bool Contains(Maths::Vector2 position) const
    {
        return width && height && position.x >= std::max(imageMin.x, clipMin.x) && position.y >= std::max(imageMin.y, clipMin.y)
            && position.x < std::min(imageMax.x, clipMax.x) && position.y < std::min(imageMax.y, clipMax.y);
    }
    Maths::Vector2 ToOutput(Maths::Vector2 position) const
    {
        if (imageMax.x <= imageMin.x || imageMax.y <= imageMin.y) return {};
        return {(position.x-imageMin.x)*width/(imageMax.x-imageMin.x), (position.y-imageMin.y)*height/(imageMax.y-imageMin.y)};
    }
    static GameViewPresentation Resolve(Maths::Vector2 origin, Maths::Vector2 available,
        GameViewSizeMode mode, uint32_t requestedWidth, uint32_t requestedHeight, float framebufferScale,
        bool lowResolution, bool fit, float scale, Maths::Vector2 pan)
    {
        GameViewPresentation view;
        view.clipMin = origin;
        view.clipMax = origin + available;
        if (!(available.x > 0 && available.y > 0)) return view;
        const float dpi = lowResolution ? 1.0f : std::clamp(framebufferScale, 1.0f, 4.0f);
        float w = available.x * dpi, h = available.y * dpi;
        if (mode == GameViewSizeMode::FixedResolution) { w = float(requestedWidth); h = float(requestedHeight); }
        else if (mode == GameViewSizeMode::AspectRatio && requestedWidth && requestedHeight)
        {
            const float aspect = float(requestedWidth)/requestedHeight;
            if (w/h > aspect) w = h*aspect; else h = w/aspect;
        }
        view.width = static_cast<uint32_t>(std::clamp(std::round(w), 1.0f, 8192.0f));
        view.height = static_cast<uint32_t>(std::clamp(std::round(h), 1.0f, 8192.0f));
        const float zoom = fit ? std::min(available.x/view.width, available.y/view.height) : std::clamp(scale, 0.1f, 8.0f)/std::clamp(framebufferScale, 1.0f, 4.0f);
        const Maths::Vector2 size { view.width*zoom, view.height*zoom };
        view.imageMin = origin + (available-size)*0.5f + (fit ? Maths::Vector2{} : pan);
        view.imageMax = view.imageMin+size;
        return view;
    }
};
}
