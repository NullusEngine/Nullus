#include "Assets/ThumbnailRendererRegistry.h"

#include <iterator>
#include <unordered_set>
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

EditorThumbnailPreviewSubmitResult ThumbnailRendererRegistry::SubmitPreview(
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
        return {std::move(result), std::nullopt};
    }
    return renderer->SubmitPreview(request);
}

EditorThumbnailPreviewSubmitResult ThumbnailRendererRegistry::SubmitPreparedPreview(
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
        return {std::move(result), std::nullopt};
    }
    return renderer->SubmitPreparedPreview(request);
}

std::vector<EditorThumbnailPreviewCompletedReadback>
ThumbnailRendererRegistry::PollCompletedReadbacks(const size_t maxCount)
{
    std::vector<EditorThumbnailPreviewCompletedReadback> completed;
    if (maxCount == 0u)
        return completed;

    // A readback ticket does not carry a registry-owned renderer pointer. Poll
    // each distinct registered renderer and merge the bounded results instead
    // of losing completions behind the base-class no-op implementation.
    std::unordered_set<IEditorThumbnailPreviewRenderer*> visited;
    for (const auto& [kind, renderer] : m_renderers)
    {
        (void)kind;
        if (renderer == nullptr || !visited.insert(renderer.get()).second)
            continue;

        const auto remaining = maxCount - completed.size();
        auto rendererCompleted = renderer->PollCompletedReadbacks(remaining);
        if (rendererCompleted.size() > remaining)
            rendererCompleted.resize(remaining);
        completed.insert(
            completed.end(),
            std::make_move_iterator(rendererCompleted.begin()),
            std::make_move_iterator(rendererCompleted.end()));
        if (completed.size() >= maxCount)
            break;
    }
    return completed;
}

bool ThumbnailRendererRegistry::OrphanReadback(
    const EditorThumbnailPreviewReadbackTicket& ticket)
{
    if (!ticket.IsValid())
        return false;

    std::unordered_set<IEditorThumbnailPreviewRenderer*> visited;
    for (const auto& [kind, renderer] : m_renderers)
    {
        (void)kind;
        if (renderer == nullptr || !visited.insert(renderer.get()).second)
            continue;
        if (renderer->OrphanReadback(ticket))
            return true;
    }
    return false;
}

std::shared_ptr<IEditorThumbnailPreviewRenderer> ThumbnailRendererRegistry::Find(
    const AssetThumbnailKind kind) const
{
    const auto found = m_renderers.find(kind);
    return found != m_renderers.end() ? found->second : nullptr;
}
}
