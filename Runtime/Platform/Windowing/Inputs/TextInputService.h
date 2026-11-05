#pragma once
#include "PlatformDef.h"
#include <Input/InputTypes.h>
#include <span>
#include <string>
#include <vector>

namespace NLS::Windowing::Inputs
{
struct TextInputEvent
{
    uint64_t sequence = 0;
    double timestamp = 0;
    Input::WindowId window;
    char32_t codepoint = 0;
};
struct TextComposition
{
    Input::WindowId window;
    std::u32string text;
    size_t caret = 0;
    uint64_t sequence = 0;
    double timestamp = 0;
    bool active = false;
};

// Committed text is deliberately separate from physical keyboard controls.
class NLS_PLATFORM_API TextInputService final
{
public:
    bool Commit(char32_t codepoint, Input::WindowId window, uint64_t sequence, double timestamp);
    std::span<const TextInputEvent> GetEvents() const { return m_events; }
    const std::string& GetCommittedText() const { return m_text; }
    const TextComposition& GetComposition() const { return m_composition; }
    const std::string& GetCompositionText() const { return m_compositionText; }
    bool UpdateComposition(std::u32string text, size_t caret, Input::WindowId window, uint64_t sequence, double timestamp);
    void CancelComposition(Input::WindowId window, uint64_t sequence, double timestamp);
    void EndFrame() { m_events.clear(); m_text.clear(); }
private:
    std::vector<TextInputEvent> m_events;
    std::string m_text;
    TextComposition m_composition;
    std::string m_compositionText;
};
}
