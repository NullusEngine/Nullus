#include "UI/Widgets/InputFields/InputText.h"
#include "ImGui/imgui.h"
#include "UI/Controls/TextControl.h"

namespace NLS::UI::Widgets
{
InputText::InputText(const std::string& p_content, const std::string& p_label)
    : DataWidget<std::string>(content), content(p_content), label(p_label)
{
}

void InputText::_Draw_Impl()
{
    std::string previousContent = content;

    const auto result=Controls::Text((label+m_widgetID).c_str(),content,false,{}, {},
        ImGuiInputTextFlags_EnterReturnsTrue | (selectAllOnClick?ImGuiInputTextFlags_AutoSelectAll:0));
    const bool enterPressed=result.completed && ImGui::IsKeyPressed(ImGuiKey_Enter);

    if (content != previousContent)
    {
        ContentChangedEvent.Invoke(content);
        this->NotifyChange();
    }

    if (enterPressed)
        EnterPressedEvent.Invoke(content);
}
} // namespace NLS
