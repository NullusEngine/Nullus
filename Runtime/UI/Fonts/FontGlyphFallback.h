#pragma once

#include <ImGui/imgui.h>
#include <ImGui/imgui_internal.h>

#include <string>
#include <string_view>
#include <utility>

namespace NLS::UI
{
// The atlas retains pointers to the range data until it is cleared. Keep the
// active ranges separate from requests made while a frame is being rendered.
class FontGlyphFallback
{
public:
    FontGlyphFallback(ImFontAtlas& atlas, std::string path) : m_path(std::move(path))
    {
        m_glyphs.AddRanges(atlas.GetGlyphRangesChineseSimplifiedCommon());
        PrepareRanges();
    }

    void RequestText(std::string_view text, const ImFont* currentFont = nullptr)
    {
        const char* cursor = text.data();
        const char* end = cursor + text.size();
        while (cursor < end)
        {
            unsigned int codepoint = 0;
            const int length = ImTextCharFromUtf8(&codepoint, cursor, end);
            if (length <= 0) break;
            cursor += length;
            if (codepoint < 0x80 || codepoint > IM_UNICODE_CODEPOINT_MAX ||
                m_glyphs.GetBit(codepoint) ||
                (currentFont && currentFont->FindGlyphNoFallback(static_cast<ImWchar>(codepoint))))
                continue;
            m_glyphs.SetBit(codepoint);
            m_dirty = true;
        }
    }

    bool IsDirty() const { return m_dirty; }

    // Call only after the old atlas configuration has been cleared.
    void PrepareRanges()
    {
        m_ranges.clear();
        m_glyphs.BuildRanges(&m_ranges);
        m_dirty = false;
    }

    bool Merge(ImFontAtlas& atlas, ImFont* target, float size) const
    {
        if (!target || m_path.empty()) return false;
        ImFontConfig config;
        config.MergeMode = true;
        config.DstFont = target;
        config.FontNo = 0;
        config.OversampleH = 1;
        config.OversampleV = 1;
        return atlas.AddFontFromFileTTF(m_path.c_str(), size, &config, m_ranges.Data) != nullptr;
    }

private:
    std::string m_path;
    ImFontGlyphRangesBuilder m_glyphs;
    ImVector<ImWchar> m_ranges;
    bool m_dirty = false;
};
}
