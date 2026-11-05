#include "TextInputService.h"

namespace NLS::Windowing::Inputs
{
namespace
{
void AppendUtf8(std::string& text, uint32_t value)
{
    if (value < 0x80) text.push_back(static_cast<char>(value));
    else if (value < 0x800)
    { text.push_back(static_cast<char>(0xc0 | (value >> 6))); text.push_back(static_cast<char>(0x80 | (value & 63))); }
    else if (value < 0x10000)
    { text.push_back(static_cast<char>(0xe0 | (value >> 12))); text.push_back(static_cast<char>(0x80 | ((value >> 6) & 63))); text.push_back(static_cast<char>(0x80 | (value & 63))); }
    else
    { text.push_back(static_cast<char>(0xf0 | (value >> 18))); text.push_back(static_cast<char>(0x80 | ((value >> 12) & 63))); text.push_back(static_cast<char>(0x80 | ((value >> 6) & 63))); text.push_back(static_cast<char>(0x80 | (value & 63))); }
}
}
bool TextInputService::UpdateComposition(std::u32string text, size_t caret, Input::WindowId window, uint64_t sequence, double timestamp)
{
    if (caret > text.size()) return false;
    for (char32_t codepoint : text) if (codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff)) return false;
    m_compositionText.clear();
    for (char32_t codepoint : text) AppendUtf8(m_compositionText, codepoint);
    m_composition = {window, std::move(text), caret, sequence, timestamp, true}; return true;
}
void TextInputService::CancelComposition(Input::WindowId window, uint64_t sequence, double timestamp)
{
    if (m_composition.active && m_composition.window == window)
    { m_composition = {window, {}, 0, sequence, timestamp, false}; m_compositionText.clear(); }
}
bool TextInputService::Commit(char32_t codepoint, Input::WindowId window, uint64_t sequence, double timestamp)
{
    const auto value = static_cast<uint32_t>(codepoint);
    if (value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
    m_events.push_back({sequence, timestamp, window, codepoint});
    AppendUtf8(m_text, value);
    return true;
}
}
