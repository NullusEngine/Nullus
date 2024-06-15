#pragma once

#include "UI/Widgets/Texts/Text.h"

namespace NLS::UI::Widgets::Texts
{
/**
 * Simple widget to display a long text with word-wrap on a panel
 */
class TextWrapped : public Text
{
public:
    /**
     * Constructor
     * @param p_content
     */
    TextWrapped(const std::string& p_content = "");

protected:
    virtual void _Draw_Impl() override;
};
} // namespace NLS::UI::Widgets::Texts