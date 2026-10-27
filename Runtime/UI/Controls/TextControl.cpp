#include "UI/Controls/TextControl.h"

#include <imgui_internal.h>

namespace NLS::UI::Controls
{
namespace
{
struct TextCallbackState
{
    std::string& text;
    size_t maximum;
};
int ResizeText(ImGuiInputTextCallbackData* data)
{
    auto& state = *static_cast<TextCallbackState*>(data->UserData);
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
    {
        auto& text = state.text;
        text.resize(static_cast<size_t>(data->BufTextLen));
        data->Buf = text.data();
    }
    else if (data->EventFlag == ImGuiInputTextFlags_CallbackEdit && static_cast<size_t>(data->BufTextLen) > state.maximum)
    {
        auto keep = state.maximum;
        while (keep > 0 && (static_cast<unsigned char>(data->Buf[keep]) & 0xc0) == 0x80)
            --keep;
        data->DeleteChars(static_cast<int>(keep), data->BufTextLen - static_cast<int>(keep));
    }
    return 0;
}
} // namespace
ControlResult Text(const char* id, std::string& value, bool multiline, const ControlOptions& options, ImVec2 size,
                   ImGuiInputTextFlags flags, size_t maximumBytes)
{
    ImGui::BeginDisabled(options.readOnly);
    MixedLabel(options);
    const auto before = value;
    value.reserve(value.size() + 64);
    flags |= ImGuiInputTextFlags_CallbackResize;
    if (maximumBytes != (std::numeric_limits<size_t>::max)())
        flags |= ImGuiInputTextFlags_CallbackEdit;
    TextCallbackState state{value, maximumBytes};
    bool submitted = multiline ? ImGui::InputTextMultiline(id, value.data(), value.capacity() + 1, size, flags, ResizeText, &state) : ImGui::InputText(id, value.data(), value.capacity() + 1, flags, ResizeText, &state);
    auto result = ItemResult(value != before);
    result.completed |= submitted && (flags & ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::EndDisabled();
    return result;
}
ControlResult Toggle(const char* id, bool& value, const ControlOptions& options)
{
    ImGui::BeginDisabled(options.readOnly);
    if (options.mixed)
        ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
    auto result = ItemResult(ImGui::Checkbox(id, &value));
    if (options.mixed)
        ImGui::PopItemFlag();
    result.completed |= result.changed;
    ImGui::EndDisabled();
    return result;
}
} // namespace NLS::UI::Controls
