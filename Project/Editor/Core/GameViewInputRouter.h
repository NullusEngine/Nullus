#pragma once
#include "GameViewPresentation.h"
#include <Scripting/GameInputFrame.h>
namespace NLS::Editor::Core
{
class GameViewInputRouter final
{
public:
    void Release() { m_focused = false; }
    bool IsFocused() const { return m_focused; }
    NLS::Scripting::GameInputFrame Route(NLS::Scripting::GameInputFrame raw, const GameViewPresentation& view,
        Maths::Vector2 pointer, bool ownsPointer, bool windowFocused, bool active, uint64_t sessionId)
    {
        const bool hit = ownsPointer && view.Contains(pointer);
        if (!active || !windowFocused || raw.pressed[256] || (raw.mousePressed[0] && !hit)) Release();
        else if (raw.mousePressed[0] && hit) m_focused = true;
        if (!m_focused)
        {
            NLS::Scripting::GameInputFrame neutral;
            neutral.width=view.width; neutral.height=view.height; neutral.sessionId=sessionId;
            return neutral;
        }
        raw.sessionId=sessionId; raw.width=view.width; raw.height=view.height; raw.focused=true;
        raw.position=view.ToOutput(pointer);
        raw.pointerOrigin = view.imageMin;
        raw.pointerScale = {view.width/(view.imageMax.x-view.imageMin.x), view.height/(view.imageMax.y-view.imageMin.y)};
        raw.pointerAvailable=hit;
        raw.delta={raw.delta.x*view.width/(view.imageMax.x-view.imageMin.x),raw.delta.y*view.height/(view.imageMax.y-view.imageMin.y)};
        if (!hit)
        {
            raw.mouse.fill(false); raw.mousePressed.fill(false); raw.mouseReleased.fill(false); raw.scroll={};
        }
        return raw;
    }
private:
    bool m_focused = false;
};
}
