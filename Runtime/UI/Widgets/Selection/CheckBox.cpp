#include "UI/Widgets/Selection/CheckBox.h"

namespace NLS::UI::Widgets
{
CheckBox::CheckBox(bool p_value, const std::string& p_label)
    : DataWidget<bool>(value), value(p_value), label(p_label)
{
}

void CheckBox::_Draw_Impl()
{
    bool previousValue = value;

    ImGui::Checkbox((label + m_widgetID).c_str(), &value);

    if (value != previousValue)
    {
        ValueChangedEvent.Invoke(value);
        this->NotifyChange();
    }
}

} // namespace NLS::UI::Widgets
