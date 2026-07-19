#include "Assets/ThumbnailRendererRegistry.h"

#include <utility>

namespace NLS::Editor::Assets
{
void ThumbnailRendererRegistry::Register(
    const AssetThumbnailKind kind,
    std::shared_ptr<IEditorThumbnailPreviewRenderer> renderer)
{
    if (renderer == nullptr)
    {
        Unregister(kind);
        return;
    }
    m_renderers[kind] = std::move(renderer);
}

void ThumbnailRendererRegistry::Unregister(const AssetThumbnailKind kind)
{
    m_renderers.erase(kind);
}

void ThumbnailRendererRegistry::Clear()
{
    m_renderers.clear();
}

bool ThumbnailRendererRegistry::Supports(const AssetThumbnailRequest& request) const
{
    const auto renderer = Find(request.kind);
    return renderer != nullptr && renderer->Supports(request);
}

EditorThumbnailPreviewResourcePumpResult ThumbnailRendererRegistry::PumpResources(
    const AssetThumbnailRequest& request)
{
    const auto renderer = Find(request.kind);
    if (renderer == nullptr)
        return { false, false, "thumbnail-renderer-unregistered" };
    return renderer->PumpResources(request);
}

EditorThumbnailPreviewResult ThumbnailRendererRegistry::Render(
    const AssetThumbnailRequest& request)
{
    const auto renderer = Find(request.kind);
    if (renderer == nullptr)
    {
        EditorThumbnailPreviewResult result;
        result.width = request.requestedSize;
        result.height = request.requestedSize;
        result.status = ThumbnailRenderStatus::Unsupported;
        result.diagnostic = "thumbnail-renderer-unregistered";
        return result;
    }
    return renderer->Render(request);
}

std::shared_ptr<IEditorThumbnailPreviewRenderer> ThumbnailRendererRegistry::Find(
    const AssetThumbnailKind kind) const
{
    const auto found = m_renderers.find(kind);
    return found != m_renderers.end() ? found->second : nullptr;
}
}
