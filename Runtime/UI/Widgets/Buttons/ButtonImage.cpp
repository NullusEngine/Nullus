#include "UI/Widgets/Buttons/ButtonImage.h"
#include "Core/ServiceLocator.h"
#include "UI/Internal/Converter.h"
#include "UI/UIManager.h"
#include "ImGui/imgui_internal.h"

namespace NLS::UI::Widgets
{
ButtonImage::ButtonImage(std::shared_ptr<NLS::Render::RHI::RHITextureView> p_textureView, const Maths::Vector2& p_size)
    : textureView(p_textureView), size(p_size)
{
}

void ButtonImage::_Draw_Impl()
{
    ImVec4 bg = Internal::Converter::ToImVec4(background);
    ImVec4 tn = Internal::Converter::ToImVec4(tint);
    void* resolvedTextureId = nullptr;
    if (NLS::Core::ServiceLocator::Contains<NLS::UI::UIManager>())
        resolvedTextureId = NLS_SERVICE(NLS::UI::UIManager).ResolveTextureId(textureView);

    const ImVec2 imageSize = Internal::Converter::ToImVec2(size);
    const bool wasDisabled = disabled;
    if (wasDisabled)
        ImGui::BeginDisabled();

    const bool clicked = resolvedTextureId != nullptr
        ? ImGui::ImageButton(resolvedTextureId, imageSize, ImVec2(0.f, 1.f), ImVec2(1.f, 0.f), -1, bg, tn)
        : ImGui::InvisibleButton(m_widgetID.c_str(), imageSize);

    if (wasDisabled)
        ImGui::EndDisabled();

    if (clicked)
        ClickedEvent.Invoke();
}
} // namespace NLS
