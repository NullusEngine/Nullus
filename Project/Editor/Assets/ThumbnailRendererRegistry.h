#pragma once

#include "Assets/EditorThumbnailPreviewRenderer.h"

#include <memory>
#include <unordered_map>

namespace NLS::Editor::Assets
{
class ThumbnailRendererRegistry final : public IEditorThumbnailPreviewRenderer
{
public:
    void Register(
        AssetThumbnailKind kind,
        std::shared_ptr<IEditorThumbnailPreviewRenderer> renderer);
    void Unregister(AssetThumbnailKind kind);
    void Clear();

    [[nodiscard]] bool Supports(const AssetThumbnailRequest& request) const override;
    EditorThumbnailPreviewResourcePumpResult PumpResources(
        const AssetThumbnailRequest& request) override;
    EditorThumbnailPreviewResult Render(const AssetThumbnailRequest& request) override;
    EditorThumbnailPreviewSubmitResult SubmitPreview(
        const AssetThumbnailRequest& request) override;
    EditorThumbnailPreviewSubmitResult SubmitPreparedPreview(
        const AssetThumbnailRequest& request) override;
    std::vector<EditorThumbnailPreviewCompletedReadback> PollCompletedReadbacks(
        size_t maxCount) override;
    bool OrphanReadback(
        const EditorThumbnailPreviewReadbackTicket& ticket) override;

private:
    [[nodiscard]] std::shared_ptr<IEditorThumbnailPreviewRenderer> Find(
        AssetThumbnailKind kind) const;

    std::unordered_map<AssetThumbnailKind, std::shared_ptr<IEditorThumbnailPreviewRenderer>> m_renderers;
};
}
