#include "UI/Widgets/Visual/Image.h"
#include "Core/ServiceLocator.h"
#include "UI/Internal/Converter.h"
#include "UI/UIManager.h"

namespace NLS::UI::Widgets
{
Image::Image(uint32_t p_textureID, const Maths::Vector2& p_size)
    : textureID{p_textureID}, size(p_size)
{
}

void Image::_Draw_Impl()
{
    void* resolvedTextureId = textureID.raw;
    if (NLS::Core::ServiceLocator::Contains<NLS::UI::UIManager>())
        resolvedTextureId = NLS_SERVICE(NLS::UI::UIManager).ResolveTextureID(textureID.id);

    const ImVec2 uv0 = flipVertically ? ImVec2(0.f, 1.f) : ImVec2(0.f, 0.f);
    const ImVec2 uv1 = flipVertically ? ImVec2(1.f, 0.f) : ImVec2(1.f, 1.f);
    ImGui::Image(resolvedTextureId, Internal::Converter::ToImVec2(size), uv0, uv1);
}
} // namespace NLS
