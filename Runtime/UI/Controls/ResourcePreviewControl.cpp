#include "UI/Controls/ResourcePreviewControl.h"
#include <algorithm>

namespace NLS::UI::Controls
{
bool AcceptPreviewResult(ResourcePreviewState& state, const PreviewRequest& request, PreviewImage image)
{
    if (request.lifetime.expired() || image.identity != request.identity || image.revision != request.revision)
        return false;
    if (state.identity != request.identity || state.revision != request.revision)
    {
        state.image.reset();
        state.identity = request.identity;
        state.revision = request.revision;
    }
    if (state.image && image.generation < state.image->generation)
        return false;
    state.image = std::move(image);
    return true;
}
void ResourcePreview(const char* id, ResourcePreviewState& state, const PreviewRequest& request,
                     const PreviewProvider& provider, const ControlOptions& options)
{
    if (request.lifetime.expired())
    {
        state = {};
        return;
    }
    if (state.identity != request.identity || state.revision != request.revision)
    {
        state = {};
        state.identity = request.identity;
        state.revision = request.revision;
    }
    ImGui::PushID(id);
    if (ImGui::TreeNode("Preview"))
    {
        if (provider)
            if (auto result = provider(request))
                AcceptPreviewResult(state, request, std::move(*result));
        if (state.image && state.image->texture)
        {
            const float width = (std::min)(static_cast<float>(request.size) * options.scale, ImGui::GetContentRegionAvail().x);
            const float aspect = state.image->dimensions.y > 0 ? state.image->dimensions.x / state.image->dimensions.y : 1;
            ImGui::Image(state.image->texture, {width, width / (std::max)(.01f, aspect)}, state.image->uv0, state.image->uv1);
        }
        else
            ImGui::TextDisabled("%s", state.image && !state.image->message.empty() ? state.image->message.c_str() : "Loading preview...");
        ImGui::TreePop();
    }
    ImGui::PopID();
}
} // namespace NLS::UI::Controls
