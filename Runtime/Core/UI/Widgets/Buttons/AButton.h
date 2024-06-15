#pragma once

#include <string>

#include <Eventing/Event.h>

#include "UI/Widgets/AWidget.h"

namespace NLS::UI::Widgets::Buttons
{
/**
 * Base class for any button widget
 */
class AButton : public AWidget
{
protected:
    void _Draw_Impl() override = 0;
    ~AButton() noexcept = default;

public:
    Event<> ClickedEvent;
};
} // namespace NLS::UI::Widgets::Buttons