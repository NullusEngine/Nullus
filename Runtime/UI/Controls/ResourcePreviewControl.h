#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include "UI/Controls/Control.h"

namespace NLS::UI::Controls
{
struct PreviewRequest
{
    std::string identity;
    uint64_t revision = 0;
    uint32_t size = 128;
    std::weak_ptr<const void> lifetime;
};
struct PreviewImage
{
    std::string identity, message;
    uint64_t revision = 0, generation = 0;
    ImTextureID texture = nullptr;
    ImVec2 dimensions{}, uv0{0, 0}, uv1{1, 1};
    std::shared_ptr<const void> owner;
};
struct ResourcePreviewState
{
    std::optional<PreviewImage> image;
    std::string identity;
    uint64_t revision = 0;
};
using PreviewProvider = std::function<std::optional<PreviewImage>(const PreviewRequest&)>;
NLS_UI_API bool AcceptPreviewResult(ResourcePreviewState& state, const PreviewRequest& request, PreviewImage image);
NLS_UI_API void ResourcePreview(const char* id, ResourcePreviewState& state, const PreviewRequest& request,
                                const PreviewProvider& provider, const ControlOptions& options = {});
} // namespace NLS::UI::Controls
