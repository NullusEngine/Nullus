#include "Panels/EditorStatusBar.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>
#include <imgui.h>
#include <Reflection/ReflectionDiagnostics.h>
#include <ServiceLocator.h>
#include <UI/UIManager.h>
#include "Core/EditorTaskProgressService.h"
#include "Panels/Console.h"

namespace NLS::Editor::Panels
{
namespace
{
float Scaled(float value)
{
    return NLS::Core::ServiceLocator::Contains<UI::UIManager>() ? NLS_SERVICE(UI::UIManager).Scale(value) : value;
}
ImVec4 LogColor(Debug::ELogLevel level)
{
    if (level == Debug::ELogLevel::LOG_ERROR) return {1.0f, 0.35f, 0.35f, 1.0f};
    if (level == Debug::ELogLevel::LOG_WARNING) return {1.0f, 0.78f, 0.2f, 1.0f};
    return ImGui::GetStyleColorVec4(ImGuiCol_Text);
}
bool HitRegion(const char* id, float width, const char* tooltip)
{
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(std::max(width, 1.0f), ImGui::GetTextLineHeight()));
    if (ImGui::IsItemHovered())
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(Scaled(560));
        ImGui::TextUnformatted(tooltip);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    return clicked;
}
}

EditorStatusBar::EditorStatusBar()
    : PanelViewportBar("EditorStatusBar", UI::PanelViewportBar::EAnchor::BOTTOM, 24.0f, false) {}

void EditorStatusBar::SetSources(Console& console, Core::EditorTaskProgressService& tasks,
    std::function<void()> openConsole, std::function<void()> openTasks, std::function<float()> frameRate)
{
    m_console = &console; m_tasks = &tasks;
    m_openConsole = std::move(openConsole); m_openTasks = std::move(openTasks); m_frameRate = std::move(frameRate);
}

std::string EditorStatusBar::SingleLine(const std::string& text)
{
    auto result = text;
    for (auto& ch : result) if (ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
    return result;
}

std::string EditorStatusBar::Elide(const std::string& text, float width)
{
    if (width <= 0) return {};
    if (ImGui::CalcTextSize(text.c_str()).x <= width) return text;
    constexpr const char* ellipsis = "...";
    if (ImGui::CalcTextSize(ellipsis).x > width) return {};
    size_t low = 0, high = text.size();
    const float available = width - ImGui::CalcTextSize(ellipsis).x;
    while (low < high)
    {
        const size_t mid = low + (high - low + 1) / 2;
        if (ImGui::CalcTextSize(text.data(), text.data() + mid).x <= available) low = mid;
        else high = mid - 1;
    }
    // Retreat to a UTF-8 boundary before appending the ellipsis.
    while (low > 0 && low < text.size() && (static_cast<unsigned char>(text[low]) & 0xc0) == 0x80) --low;
    auto result = text.substr(0, low) + ellipsis;
    // ImGui rounds the final glyph advance; measuring prefix and suffix
    // separately can differ by a pixel from the composed label.
    while (low > 0 && ImGui::CalcTextSize(result.c_str()).x > width)
    {
        --low;
        while (low > 0 && (static_cast<unsigned char>(text[low]) & 0xc0) == 0x80) --low;
        result = text.substr(0, low) + ellipsis;
    }
    return result;
}

void EditorStatusBar::DrawBarContent()
{
    const auto snapshot = m_console ? m_console->GetStatusSnapshot() : ConsoleStatusSnapshot{};
    const auto tasks = m_tasks ? m_tasks->GetSnapshot() : std::vector<Core::EditorTaskSnapshot>{};
    const auto running = std::count_if(tasks.begin(), tasks.end(), [](const auto& task) { return task.state == Core::EditorTaskState::Running; });
    const auto diagnostics = NLS::meta::ReflectionDiagnostics::Count();
    char fps[32] = "FPS: --";
    const float rate = m_frameRate ? m_frameRate() : 0.0f;
    if (rate > 0) std::snprintf(fps, sizeof(fps), "FPS: %.1f", rate);
    const std::string reflection = "Reflection: " + std::to_string(diagnostics.errors) + " E / " + std::to_string(diagnostics.warnings) + " W";

    const float width = GetContentWidth();
    const float gap = Scaled(12);
    const float fpsWidth = width >= Scaled(400) ? ImGui::CalcTextSize(fps).x : 0;
    const float reflectionWidth = (diagnostics.errors || diagnostics.warnings) && width >= Scaled(640)
        ? ImGui::CalcTextSize(reflection.c_str()).x : 0;
    const float taskWidth = std::min(Scaled(running ? 220.0f : 128.0f), std::max(Scaled(20), width * 0.4f));
    const float messageWidth = std::max(1.0f, width - taskWidth - fpsWidth - reflectionWidth -
        gap * (1 + (fpsWidth > 0) + (reflectionWidth > 0)));
    const auto start = ImGui::GetCursorScreenPos();
    auto* draw = ImGui::GetWindowDrawList();
    const float lineHeight = ImGui::GetTextLineHeight();
    const auto color = ImGui::ColorConvertFloat4ToU32(LogColor(snapshot.level));
    const auto message = snapshot.hasMessage ? SingleLine(snapshot.message) : "Ready";
    if (HitRegion("##latestLog", messageWidth, snapshot.hasMessage ? snapshot.message.c_str() : "Open Console") && m_openConsole) m_openConsole();
    draw->PushClipRect(start, ImVec2(start.x + messageWidth, start.y + lineHeight), true);
    const float radius = Scaled(4);
    const ImVec2 icon(start.x + Scaled(5), start.y + lineHeight * 0.5f);
    if (snapshot.level == Debug::ELogLevel::LOG_WARNING)
        draw->AddTriangleFilled(ImVec2(icon.x, icon.y - radius), ImVec2(icon.x - radius, icon.y + radius), ImVec2(icon.x + radius, icon.y + radius), color);
    else if (snapshot.level == Debug::ELogLevel::LOG_ERROR)
    {
        draw->AddCircleFilled(icon, radius, color);
        draw->AddLine(ImVec2(icon.x - radius * .45f, icon.y), ImVec2(icon.x + radius * .45f, icon.y), IM_COL32(40,40,40,255), Scaled(1.5f));
    }
    else draw->AddCircle(icon, radius, color, 12, Scaled(1.5f));
    const auto label = Elide(message, messageWidth - Scaled(18));
    draw->AddText(ImVec2(start.x + Scaled(16), start.y), ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
    draw->PopClipRect();

    float x = start.x + messageWidth + gap;
    ImGui::SetCursorScreenPos(ImVec2(x, start.y));
    if (HitRegion("##backgroundTasks", taskWidth, "Open Background Tasks") && m_openTasks) m_openTasks();
    draw->PushClipRect(ImVec2(x, start.y), ImVec2(x + taskWidth, start.y + lineHeight), true);
    if (running)
    {
        const ImVec2 center(x + Scaled(6), start.y + lineHeight * .5f);
        draw->PathArcTo(center, Scaled(5), float(ImGui::GetTime() * 6), float(ImGui::GetTime() * 6 + 4.6), 16);
        draw->PathStroke(ImGui::GetColorU32(ImGuiCol_PlotHistogram), 0, Scaled(1.5f));
    }
    std::string taskText = "Background Tasks";
    if (running == 1)
    {
        taskText = tasks.front().cancellationRequested ? "Cancelling..." : tasks.front().title;
        if (!tasks.front().progress) taskText = "1 task: " + taskText;
        if (tasks.front().progress) taskText += " " + std::to_string(int(std::round(*tasks.front().progress * 100))) + "%";
        if (tasks.front().progress)
            draw->AddRectFilled(ImVec2(x, start.y + lineHeight - Scaled(2)),
                ImVec2(x + taskWidth * *tasks.front().progress, start.y + lineHeight), ImGui::GetColorU32(ImGuiCol_PlotHistogram));
    }
    else if (running > 1) taskText = std::to_string(running) + " tasks running";
    const auto taskLabel = Elide(taskText, taskWidth - (running ? Scaled(18) : 0));
    draw->AddText(ImVec2(x + (running ? Scaled(18) : 0), start.y), ImGui::GetColorU32(ImGuiCol_Text), taskLabel.c_str());
    draw->PopClipRect();
    x += taskWidth + gap;
    if (reflectionWidth)
    {
        ImGui::SetCursorScreenPos(ImVec2(x, start.y));
        if (HitRegion("##reflectionDiagnostics", reflectionWidth, "Open Console") && m_openConsole) m_openConsole();
        draw->AddText(ImVec2(x, start.y), ImGui::ColorConvertFloat4ToU32(LogColor(diagnostics.errors ? Debug::ELogLevel::LOG_ERROR : Debug::ELogLevel::LOG_WARNING)), reflection.c_str());
        x += reflectionWidth + gap;
    }
    if (fpsWidth) draw->AddText(ImVec2(x, start.y), ImGui::GetColorU32(ImGuiCol_TextDisabled), fps);
}
}
