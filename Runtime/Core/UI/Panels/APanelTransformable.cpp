#include "UI/Panels/APanelTransformable.h"
#include "UI/Internal/Converter.h"

namespace NLS
{
UI::Panels::APanelTransformable::APanelTransformable(
    const Maths::Vector2& p_defaultPosition,
    const Maths::Vector2& p_defaultSize,
    Settings::EHorizontalAlignment p_defaultHorizontalAlignment,
    Settings::EVerticalAlignment p_defaultVerticalAlignment,
    bool p_ignoreConfigFile)
    : m_defaultPosition(p_defaultPosition), m_defaultSize(p_defaultSize), m_defaultHorizontalAlignment(p_defaultHorizontalAlignment), m_defaultVerticalAlignment(p_defaultVerticalAlignment), m_ignoreConfigFile(p_ignoreConfigFile)
{
}

void UI::Panels::APanelTransformable::SetPosition(const Maths::Vector2& p_position)
{
    m_position = p_position;
    m_positionChanged = true;
}

void UI::Panels::APanelTransformable::SetSize(const Maths::Vector2& p_size)
{
    m_size = p_size;
    m_sizeChanged = true;
}

void UI::Panels::APanelTransformable::SetAlignment(Settings::EHorizontalAlignment p_horizontalAlignment, Settings::EVerticalAlignment p_verticalAligment)
{
    m_horizontalAlignment = p_horizontalAlignment;
    m_verticalAlignment = p_verticalAligment;
    m_alignmentChanged = true;
}

const Maths::Vector2& UI::Panels::APanelTransformable::GetPosition() const
{
    return m_position;
}

const Maths::Vector2& UI::Panels::APanelTransformable::GetSize() const
{
    return m_size;
}

UI::Settings::EHorizontalAlignment UI::Panels::APanelTransformable::GetHorizontalAlignment() const
{
    return m_horizontalAlignment;
}

UI::Settings::EVerticalAlignment UI::Panels::APanelTransformable::GetVerticalAlignment() const
{
    return m_verticalAlignment;
}

void UI::Panels::APanelTransformable::UpdatePosition()
{
    if (m_defaultPosition.x != -1.f && m_defaultPosition.y != 1.f)
    {
        Maths::Vector2 offsettedDefaultPos = m_defaultPosition + CalculatePositionAlignmentOffset(true);
        ImGui::SetWindowPos(Internal::Converter::ToImVec2(offsettedDefaultPos), m_ignoreConfigFile ? ImGuiCond_Once : ImGuiCond_FirstUseEver);
    }

    if (m_positionChanged || m_alignmentChanged)
    {
        Maths::Vector2 offset = CalculatePositionAlignmentOffset(false);
        Maths::Vector2 offsettedPos(m_position.x + offset.x, m_position.y + offset.y);
        ImGui::SetWindowPos(Internal::Converter::ToImVec2(offsettedPos), ImGuiCond_Always);
        m_positionChanged = false;
        m_alignmentChanged = false;
    }
}

void UI::Panels::APanelTransformable::UpdateSize()
{
    /*
    if (m_defaultSize.x != -1.f && m_defaultSize.y != 1.f)
        ImGui::SetWindowSize(Internal::Converter::ToImVec2(m_defaultSize), m_ignoreConfigFile ? ImGuiCond_Once : ImGuiCond_FirstUseEver);
    */
    if (m_sizeChanged)
    {
        ImGui::SetWindowSize(Internal::Converter::ToImVec2(m_size), ImGuiCond_Always);
        m_sizeChanged = false;
    }
}

void UI::Panels::APanelTransformable::CopyImGuiPosition()
{
    m_position = Internal::Converter::ToFVector2(ImGui::GetWindowPos());
}

void UI::Panels::APanelTransformable::CopyImGuiSize()
{
    m_size = Internal::Converter::ToFVector2(ImGui::GetWindowSize());
}

void UI::Panels::APanelTransformable::Update()
{
    if (!m_firstFrame)
    {
        if (!autoSize)
            UpdateSize();
        CopyImGuiSize();

        UpdatePosition();
        CopyImGuiPosition();
    }

    m_firstFrame = false;
}

Maths::Vector2 UI::Panels::APanelTransformable::CalculatePositionAlignmentOffset(bool p_default)
{
    Maths::Vector2 result(0.0f, 0.0f);

    switch (p_default ? m_defaultHorizontalAlignment : m_horizontalAlignment)
    {
        case UI::Settings::EHorizontalAlignment::CENTER:
            result.x -= m_size.x / 2.0f;
            break;
        case UI::Settings::EHorizontalAlignment::RIGHT:
            result.x -= m_size.x;
            break;
    }

    switch (p_default ? m_defaultVerticalAlignment : m_verticalAlignment)
    {
        case UI::Settings::EVerticalAlignment::MIDDLE:
            result.y -= m_size.y / 2.0f;
            break;
        case UI::Settings::EVerticalAlignment::BOTTOM:
            result.y -= m_size.y;
            break;
    }

    return result;
}

} // namespace NLS
