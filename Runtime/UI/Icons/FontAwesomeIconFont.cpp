#include "UI/Icons/FontAwesomeIconFont.h"

#include <algorithm>
#include <map>
#include <mutex>

#include "ImGui/imgui.h"
#include "UI/ImGuiExtensions/TimelineProfiler/IconsFontAwesome4_data.h"

namespace NLS::UI::Icons
{
namespace
{
    std::vector<ImWchar> BuildRanges(const std::vector<ImWchar>& p_codepoints)
    {
        std::vector<ImWchar> ranges;
        if (p_codepoints.empty())
            return ranges;

        std::size_t index = 0;
        while (index < p_codepoints.size())
        {
            const ImWchar start = p_codepoints[index];
            ImWchar end = start;
            while (index + 1 < p_codepoints.size() && p_codepoints[index + 1] == static_cast<ImWchar>(end + 1))
            {
                ++index;
                end = p_codepoints[index];
            }
            ranges.push_back(start);
            ranges.push_back(end);
            ++index;
        }
        ranges.push_back(0);
        return ranges;
    }
}

std::vector<ImWchar> BuildFontIconRanges(const std::vector<ImWchar>& p_codepoints)
{
    return BuildRanges(p_codepoints);
}

bool EnsureFontAwesomeIconFontLoaded(const float p_pixelSize, ImFont* p_targetFont)
{
    if (!p_targetFont)
        return EnsureFontAwesomeIconFontLoaded(p_pixelSize, nullptr, {});
    return EnsureFontAwesomeIconFontLoaded(p_pixelSize, p_targetFont, {});
}

bool EnsureFontAwesomeIconFontLoaded(
    const float p_pixelSize,
    ImFont* p_targetFont,
    const std::vector<ImWchar>& p_codepoints)
{
    if (ImGui::GetCurrentContext() == nullptr)
        return false;

    ImGuiIO& io = ImGui::GetIO();
    if (p_targetFont == nullptr && !io.Fonts->Fonts.empty())
        p_targetFont = io.Fonts->Fonts[0];

    const ImWchar probe = p_codepoints.empty() ? kFontAwesomeSearchGlyph : p_codepoints.front();
    if (p_targetFont != nullptr &&
        p_targetFont->FindGlyphNoFallback(probe) != nullptr)
    {
        return true;
    }

    if (io.Fonts->Locked)
        return false;

    if (io.Fonts->Fonts.empty())
        io.Fonts->AddFontDefault();
    if (p_targetFont == nullptr)
        p_targetFont = io.Fonts->Fonts[0];

    static const ImWchar fullRanges[] = { ICON_MIN_FA, ICON_MAX_FA, 0 };
    const ImWchar* ranges = fullRanges;
    if (!p_codepoints.empty())
    {
        // ImFontAtlas keeps GlyphRanges pointers for subsequent builds. A local
        // vector would dangle when another font is loaded or the atlas rebuilt.
        static std::mutex rangeMutex;
        static std::map<std::vector<ImWchar>, std::vector<ImWchar>> rangeCache;
        std::lock_guard lock(rangeMutex);
        auto [entry, inserted] = rangeCache.try_emplace(p_codepoints);
        if (inserted) entry->second = BuildRanges(p_codepoints);
        ranges = entry->second.data();
    }

    ImFontConfig iconConfig;
    iconConfig.MergeMode = true;
    iconConfig.PixelSnapH = true;
    iconConfig.GlyphMinAdvanceX = p_pixelSize;
    iconConfig.DstFont = p_targetFont;
    io.Fonts->AddFontFromMemoryCompressedTTF(
        font_awesome_compressed_data,
        static_cast<int>(font_awesome_compressed_size),
        p_pixelSize,
        &iconConfig,
        ranges);
    io.Fonts->Build();

    return p_targetFont->FindGlyphNoFallback(probe) != nullptr;
}
}
