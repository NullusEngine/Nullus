#pragma once

#include <UI/UIDef.h>

#include "ImGui/imgui.h"

namespace NLS::UI::Icons
{
enum class IconId
{
    Search,
    MoreHorizontal,
    Project,
    Install,
    Folder,
    Trash,
    Star
};

struct IconStyle
{
    ImU32 color = IM_COL32_WHITE;
    float strokeThickness = 1.5f;
};

NLS_UI_API void DrawIcon(ImDrawList* drawList, IconId iconId, const ImVec2& center, float size, const IconStyle& style = {});
} // namespace NLS::UI::Icons
