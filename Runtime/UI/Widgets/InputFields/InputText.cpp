#include "UI/Widgets/InputFields/InputText.h"

namespace NLS::UI::Widgets
{
InputText::InputText(const std::string& p_content, const std::string& p_label)
    : DataWidget<std::string>(content), content(p_content), label(p_label)
{
}

void InputText::_Draw_Impl()
{
    std::string previousContent = content;

    content.resize(256, '\0');
    bool enterPressed = ImGui::InputText((label + m_widgetID).c_str(), &content[0], 256, ImGuiInputTextFlags_EnterReturnsTrue | (selectAllOnClick ? ImGuiInputTextFlags_AutoSelectAll : 0));
    content = content.c_str();

    if (content != previousContent)
    {
        ContentChangedEvent.Invoke(content);
        this->NotifyChange();
    }

    if (enterPressed)
        EnterPressedEvent.Invoke(content);
}
} // namespace NLS
