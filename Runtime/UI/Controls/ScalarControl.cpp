#include "UI/Controls/ScalarControl.h"
#include "UI/Controls/TextControl.h"
#include <algorithm>
#include <string>
#include <unordered_map>
#include <array>
#include <cstring>

namespace NLS::UI::Controls
{
namespace
{
struct ExpressionState
{
    std::string text, error;
};
// Expression state is stored by caller scope and ImGui context. Empty entries
// are erased on popup close; no document or value pointers survive a draw.
thread_local std::unordered_map<ImGuiContext*, std::unordered_map<ImGuiID, ExpressionState>> expressions;
bool ApplyExpression(ImGuiDataType type, void* value, const std::string& text, std::string& error)
{
    switch (type)
    {
#define NLS_EXPRESSION_CASE(kind, T) \
    case kind:                       \
        return EvaluateExpressionAs(text, *static_cast<T*>(value), error)
        NLS_EXPRESSION_CASE(ImGuiDataType_S8, int8_t);
        NLS_EXPRESSION_CASE(ImGuiDataType_U8, uint8_t);
        NLS_EXPRESSION_CASE(ImGuiDataType_S16, int16_t);
        NLS_EXPRESSION_CASE(ImGuiDataType_U16, uint16_t);
        NLS_EXPRESSION_CASE(ImGuiDataType_S32, int32_t);
        NLS_EXPRESSION_CASE(ImGuiDataType_U32, uint32_t);
        NLS_EXPRESSION_CASE(ImGuiDataType_S64, int64_t);
        NLS_EXPRESSION_CASE(ImGuiDataType_U64, uint64_t);
        NLS_EXPRESSION_CASE(ImGuiDataType_Float, float);
        NLS_EXPRESSION_CASE(ImGuiDataType_Double, double);
#undef NLS_EXPRESSION_CASE
        default:
            return false;
    }
}
template<class T>
void Constrain(void* value, const ScalarOptions& options)
{
    auto& number = *static_cast<T*>(value);
    if (options.minimum)
        number = (std::max)(number, *static_cast<const T*>(options.minimum));
    if (options.maximum)
        number = (std::min)(number, *static_cast<const T*>(options.maximum));
}
} // namespace
ControlResult Scalar(const char* id, ImGuiDataType type, void* value, const ScalarOptions& scalar,
                     const ControlOptions& options)
{
    ImGui::BeginDisabled(options.readOnly);
    MixedLabel(options);
    if (type < 0 || type >= ImGuiDataType_COUNT)
    {
        ImGui::EndDisabled();
        return {};
    }
    constexpr size_t widths[]{1, 1, 2, 2, 4, 4, 8, 8, 4, 8};
    std::array<unsigned char, 8> before{};
    std::memcpy(before.data(), value, widths[type]);
    if (scalar.vertical && scalar.minimum && scalar.maximum)
        ImGui::VSliderScalar(id, scalar.size, type, value, scalar.minimum, scalar.maximum, scalar.format, scalar.flags);
    else if (scalar.slider && scalar.minimum && scalar.maximum)
        ImGui::SliderScalar(id, type, value, scalar.minimum, scalar.maximum, scalar.format, scalar.flags);
    else if (scalar.drag)
        ImGui::DragScalar(id, type, value, scalar.speed, scalar.minimum, scalar.maximum, scalar.format, scalar.flags);
    else
        ImGui::InputScalar(id, type, value, scalar.step, scalar.fastStep, scalar.format, scalar.textFlags);
    const bool changed = std::memcmp(before.data(), value, widths[type]) != 0;
    auto result = ItemResult(changed);
    result.completed |= ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Enter);
    const auto key = ImGui::GetID(id);
    ImGui::PushID(id);
    if (scalar.expression && ImGui::IsItemFocused() && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Enter))
        ImGui::OpenPopup("Expression");
    if (scalar.expression && ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
        ImGui::OpenPopup("Expression");
    if (ImGui::BeginPopup("Expression"))
    {
        auto& state = expressions[ImGui::GetCurrentContext()][key];
        Text("##Formula", state.text);
        if (ImGui::Button("Evaluate") || (ImGui::IsKeyPressed(ImGuiKey_Enter) && !state.text.empty()))
        {
            if (ApplyExpression(type, value, state.text, state.error))
            {
                result.changed = result.completed = true;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            result.canceled = true;
            ImGui::CloseCurrentPopup();
        }
        if (!state.error.empty())
            ImGui::TextWrapped("%s", state.error.c_str());
        result.active = !result.completed && !result.canceled;
        ImGui::EndPopup();
    }
    else if (!ImGui::IsPopupOpen("Numeric menu"))
        expressions[ImGui::GetCurrentContext()].erase(key);
    if (result.changed)
    {
        switch (type)
        {
#define NLS_CLAMP_CASE(kind, T)      \
    case kind:                       \
        Constrain<T>(value, scalar); \
        break
            NLS_CLAMP_CASE(ImGuiDataType_S8, int8_t);
            NLS_CLAMP_CASE(ImGuiDataType_U8, uint8_t);
            NLS_CLAMP_CASE(ImGuiDataType_S16, int16_t);
            NLS_CLAMP_CASE(ImGuiDataType_U16, uint16_t);
            NLS_CLAMP_CASE(ImGuiDataType_S32, int32_t);
            NLS_CLAMP_CASE(ImGuiDataType_U32, uint32_t);
            NLS_CLAMP_CASE(ImGuiDataType_S64, int64_t);
            NLS_CLAMP_CASE(ImGuiDataType_U64, uint64_t);
            NLS_CLAMP_CASE(ImGuiDataType_Float, float);
            NLS_CLAMP_CASE(ImGuiDataType_Double, double);
#undef NLS_CLAMP_CASE
            default:
                break;
        }
    }
    if (result.changed)
        result.components = 1;
    ImGui::PopID();
    ImGui::EndDisabled();
    return result;
}
} // namespace NLS::UI::Controls
