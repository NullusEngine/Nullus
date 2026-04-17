#include "UI/Icons/VectorIcons.h"

#include <algorithm>
#include <cmath>

namespace NLS::UI::Icons
{
namespace
{
void DrawSearch(ImDrawList* drawList, const ImVec2& center, float size, const IconStyle& style)
{
    const float radius = size * 0.24f;
    drawList->AddCircle(center, radius, style.color, 24, style.strokeThickness);
    drawList->AddLine(
        ImVec2(center.x + radius * 0.78f, center.y + radius * 0.78f),
        ImVec2(center.x + radius * 1.78f, center.y + radius * 1.78f),
        style.color,
        style.strokeThickness);
}

void DrawMoreHorizontal(ImDrawList* drawList, const ImVec2& center, float size, const IconStyle& style)
{
    const float spacing = size * 0.18f;
    const float radius = (std::max)(1.4f, size * 0.08f);
    drawList->AddCircleFilled(ImVec2(center.x - spacing, center.y), radius, style.color, 12);
    drawList->AddCircleFilled(center, radius, style.color, 12);
    drawList->AddCircleFilled(ImVec2(center.x + spacing, center.y), radius, style.color, 12);
}

void DrawProject(ImDrawList* drawList, const ImVec2& center, float size, const IconStyle& style)
{
    ImVec2 points[6] = {
        ImVec2(center.x, center.y - size * 0.32f),
        ImVec2(center.x + size * 0.28f, center.y - size * 0.16f),
        ImVec2(center.x + size * 0.28f, center.y + size * 0.18f),
        ImVec2(center.x, center.y + size * 0.34f),
        ImVec2(center.x - size * 0.28f, center.y + size * 0.18f),
        ImVec2(center.x - size * 0.28f, center.y - size * 0.16f)
    };

    drawList->AddPolyline(points, 6, style.color, ImDrawFlags_Closed, style.strokeThickness);
    const ImVec2& topLeft = points[5];
    const ImVec2& topRight = points[1];
    const ImVec2& bottomLeft = points[4];
    drawList->AddLine(topLeft, center, style.color, style.strokeThickness);
    drawList->AddLine(center, bottomLeft, style.color, style.strokeThickness);
    drawList->AddLine(center, topRight, style.color, style.strokeThickness);
}

void DrawInstall(ImDrawList* drawList, const ImVec2& center, float size, const IconStyle& style)
{
    const ImVec2 min(center.x - size * 0.30f, center.y - size * 0.20f);
    const ImVec2 max(center.x + size * 0.30f, center.y + size * 0.22f);
    drawList->AddRect(min, max, style.color, 2.0f, 0, style.strokeThickness);
    drawList->AddLine(
        ImVec2(min.x, center.y - size * 0.02f),
        ImVec2(max.x, center.y - size * 0.02f),
        style.color,
        style.strokeThickness);
    drawList->AddLine(
        ImVec2(center.x - size * 0.10f, min.y - size * 0.02f),
        ImVec2(center.x + size * 0.10f, min.y - size * 0.02f),
        style.color,
        style.strokeThickness);
    drawList->AddLine(
        ImVec2(center.x - size * 0.06f, min.y - size * 0.08f),
        ImVec2(center.x + size * 0.06f, min.y - size * 0.08f),
        style.color,
        style.strokeThickness);
}

void DrawFolder(ImDrawList* drawList, const ImVec2& center, float size, const IconStyle& style)
{
    const ImVec2 min(center.x - size * 0.34f, center.y - size * 0.18f);
    const ImVec2 max(center.x + size * 0.34f, center.y + size * 0.22f);
    drawList->AddRect(min, max, style.color, 3.0f, 0, style.strokeThickness);
    drawList->AddLine(
        ImVec2(min.x + size * 0.10f, min.y),
        ImVec2(min.x + size * 0.24f, min.y - size * 0.12f),
        style.color,
        style.strokeThickness);
    drawList->AddLine(
        ImVec2(min.x + size * 0.24f, min.y - size * 0.12f),
        ImVec2(min.x + size * 0.48f, min.y - size * 0.12f),
        style.color,
        style.strokeThickness);
}

void DrawTrash(ImDrawList* drawList, const ImVec2& center, float size, const IconStyle& style)
{
    const ImVec2 bodyMin(center.x - size * 0.20f, center.y - size * 0.10f);
    const ImVec2 bodyMax(center.x + size * 0.20f, center.y + size * 0.26f);
    drawList->AddRect(bodyMin, bodyMax, style.color, 2.0f, 0, style.strokeThickness);
    drawList->AddLine(
        ImVec2(center.x - size * 0.26f, center.y - size * 0.16f),
        ImVec2(center.x + size * 0.26f, center.y - size * 0.16f),
        style.color,
        style.strokeThickness);
    drawList->AddLine(
        ImVec2(center.x - size * 0.10f, center.y - size * 0.22f),
        ImVec2(center.x + size * 0.10f, center.y - size * 0.22f),
        style.color,
        style.strokeThickness);
    drawList->AddLine(ImVec2(center.x - size * 0.08f, bodyMin.y + size * 0.04f), ImVec2(center.x - size * 0.08f, bodyMax.y - size * 0.04f), style.color, style.strokeThickness);
    drawList->AddLine(ImVec2(center.x + size * 0.08f, bodyMin.y + size * 0.04f), ImVec2(center.x + size * 0.08f, bodyMax.y - size * 0.04f), style.color, style.strokeThickness);
}

void DrawStar(ImDrawList* drawList, const ImVec2& center, float size, const IconStyle& style)
{
    constexpr float kPi = 3.14159265358979323846f;
    ImVec2 points[10];
    for (int index = 0; index < 10; ++index)
    {
        const float angle = -kPi * 0.5f + index * kPi * 0.2f;
        const float radius = (index % 2 == 0) ? size * 0.34f : size * 0.15f;
        points[index] = ImVec2(center.x + std::cos(angle) * radius, center.y + std::sin(angle) * radius);
    }

    drawList->AddPolyline(points, 10, style.color, ImDrawFlags_Closed, style.strokeThickness);
}
}

void DrawIcon(ImDrawList* drawList, IconId iconId, const ImVec2& center, float size, const IconStyle& style)
{
    if (drawList == nullptr || size <= 0.0f)
        return;

    switch (iconId)
    {
    case IconId::Search:
        DrawSearch(drawList, center, size, style);
        break;
    case IconId::MoreHorizontal:
        DrawMoreHorizontal(drawList, center, size, style);
        break;
    case IconId::Project:
        DrawProject(drawList, center, size, style);
        break;
    case IconId::Install:
        DrawInstall(drawList, center, size, style);
        break;
    case IconId::Folder:
        DrawFolder(drawList, center, size, style);
        break;
    case IconId::Trash:
        DrawTrash(drawList, center, size, style);
        break;
    case IconId::Star:
        DrawStar(drawList, center, size, style);
        break;
    }
}
} // namespace NLS::UI::Icons
