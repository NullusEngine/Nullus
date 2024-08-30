#pragma once

#include <Eventing/Event.h>

#include "UI/Widgets/DataWidget.h"

namespace NLS::UI::Widgets
{
/**
 * Checkbox widget that can be checked or not
 */
class NLS_UI_API CheckBox : public DataWidget<bool>
{
public:
    /**
     * Constructor
     * @param p_value
     * @param p_label
     */
    CheckBox(bool p_value = false, const std::string& p_label = "");
    ~CheckBox() noexcept = default;

protected:
    void _Draw_Impl() override;

public:
    bool value;
    std::string label;
    NLS::Event<bool> ValueChangedEvent;
};
} // namespace NLS::UI::Widgets