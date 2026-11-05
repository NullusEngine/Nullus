#pragma once

#include <cstdint>

namespace NLS::Editor::Panels
{
enum class SceneViewSelectionCaptureState { Held, Released, Lost };

// Mouse ownership for a Scene selection gesture. Call only while drawing the
// Scene window; Reset also works outside a frame or after the context is gone.
class SceneViewSelectionCapture
{
public:
    [[nodiscard]] bool CanBegin() const;
    void Begin();
    [[nodiscard]] SceneViewSelectionCaptureState Poll() const;
    void Reset();

private:
    uint32_t m_id = 0;
};
}
