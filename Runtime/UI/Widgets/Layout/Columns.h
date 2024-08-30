#pragma once

#include <vector>

#include "UI/Internal/WidgetContainer.h"

namespace NLS::UI::Widgets
{
/**
 * Widget that allow columnification
 */
class NLS_UI_API Columns : public AWidget, public Internal::WidgetContainer
{
public:
    /**
     * Constructor
     */
    Columns(size_t size);

protected:
    virtual void _Draw_Impl() override;

public:
    std::vector<float> widths;
    size_t m_size;
};
} // namespace NLS::UI::Widgets::Layout