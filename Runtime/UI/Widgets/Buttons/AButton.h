#pragma once

#include <string>

#include <Eventing/Event.h>

#include "UI/Widgets/AWidget.h"

namespace NLS::UI::Widgets
{
/**
 * Base class for any button widget
 */
class NLS_UI_API AButton : public AWidget
{
protected:
    void _Draw_Impl() override = 0;
    ~AButton() noexcept = default;

public:
    Event<> ClickedEvent;
};
} // namespace NLS::UI::Widgets