#include "UI/Widgets/Visual/Image.h"
#include "ImGui/imgui.h"
#include "Core/ServiceLocator.h"
#include "UI/Internal/Converter.h"
#include "UI/UIManager.h"

namespace NLS::UI::Widgets
{
Image::Image(std::shared_ptr<NLS::Render::RHI::RHITextureView> p_textureView, const Maths::Vector2& p_size)
    : textureView(p_textureView), size(p_size)
{
}

void Image::_Draw_Impl()
{
    void* resolvedTextureId = nullptr;
    if (NLS::Core::ServiceLocator::Contains<NLS::UI::UIManager>())
        resolvedTextureId = NLS_SERVICE(NLS::UI::UIManager).ResolveTextureId(textureView);

    const ImVec2 uv0 = flipVertically ? ImVec2(0.f, 1.f) : ImVec2(0.f, 0.f);
    const ImVec2 uv1 = flipVertically ? ImVec2(1.f, 0.f) : ImVec2(1.f, 1.f);
    const ImVec2 imageSize = Internal::Converter::ToImVec2(size);
    if (resolvedTextureId != nullptr)
        ImGui::Image(resolvedTextureId, imageSize, uv0, uv1);
    else
        ImGui::Dummy(imageSize);
    m_lastDrawMin = Internal::Converter::ToFVector2(ImGui::GetItemRectMin());
    m_lastDrawMax = Internal::Converter::ToFVector2(ImGui::GetItemRectMax());
    m_lastDrawFrame = ImGui::GetFrameCount();
    m_hasLastDrawBounds = true;
    m_hoveredLastDraw = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
}

bool Image::WasDrawnThisFrame() const
{
    return m_hasLastDrawBounds &&
        ImGui::GetCurrentContext() != nullptr &&
        m_lastDrawFrame == ImGui::GetFrameCount();
}
} // namespace NLS
