#pragma once

#include "EngineDef.h"
#include <Rendering/Context/RenderScenePackage.h>
#include <Rendering/FrameGraph/ExternalResourceBridge.h>
#include <Rendering/Sprites/SpriteBatcher.h>
#include <Core/ResourceManagement/ResourceHandle.h>
#include <Math/Matrix4.h>
#include <Math/Vector4.h>
#include <Math/Vector2.h>
#include <Assets/SpriteArtifact.h>

namespace NLS::Engine::Components { class SpriteRendererComponent; }
namespace NLS::Render::Resources { class Texture2D; class Material; }

namespace NLS::Engine::Rendering
{
struct SpriteShaderSnapshot
{
    uint64_t identity = 0;
    std::vector<Render::RHI::RHIShaderModuleDesc> shaders;
    Render::RHI::RHIBindingLayoutDesc materialLayout;
    struct Constant { uint32_t binding = 0; std::vector<uint8_t> bytes; };
    std::vector<Constant> constants;
};
// Game-thread capture. Only value data, immutable shaders and strong resource
// owners cross to the render thread; no Component/GameObject pointer escapes.
struct SpriteRenderProxy
{
    std::shared_ptr<const Core::Assets::SpriteArtifact> sprite;
    std::shared_ptr<const SpriteShaderSnapshot> program;
    std::shared_ptr<Core::ResourceManagement::ResourceHandle<Render::Resources::Texture2D>> textureOwner;
    std::shared_ptr<Core::ResourceManagement::ResourceHandle<Render::Resources::Material>> materialOwner;
    std::shared_ptr<Render::RHI::RHITextureView> texture;
    Render::RHI::SamplerDesc sampler;
    Maths::Matrix4 world = Maths::Matrix4::Identity;
    Maths::Vector4 color {1,1,1,1};
    std::string sortingLayer = "Default";
    int32_t layerOrder = 0, orderInLayer = 0;
    uint32_t visibilityLayer = 0;
    uint64_t ownerId = 0;
    bool flipX = false, flipY = false;
    bool picking = false;
    bool sliced = false;
    bool tiled = false;
    Maths::Vector2 size{1,1};
};

NLS_ENGINE_API std::shared_ptr<const SpriteRenderProxy> CaptureSpriteRenderProxy(
    Components::SpriteRendererComponent& component, std::string& error);
// One game-thread extraction shares resource resolution and a single sorting
// table revision. Results retain input order; unavailable components yield null.
NLS_ENGINE_API std::vector<std::shared_ptr<const SpriteRenderProxy>> CaptureSpriteRenderProxies(
    const std::vector<Components::SpriteRendererComponent*>& components);

class NLS_ENGINE_API SpriteSceneRenderer final
{
public:
    struct Statistics
    {
        uint32_t submitted=0, visible=0, culled=0, batches=0, drawCalls=0;
        uint32_t materialBreaks=0, textureBreaks=0, stateBreaks=0, capacityBreaks=0, orderingBreaks=0;
        bool cpuExpanded=false, prepared=false;
    };
    SpriteSceneRenderer();
    ~SpriteSceneRenderer();
    bool AppendPass(std::shared_ptr<Render::RHI::RHIDevice> device,
        Render::Context::RenderScenePackage& package,
        const Render::FrameGraph::FrozenFrameDescriptor& view,
        const std::vector<std::shared_ptr<const SpriteRenderProxy>>& sprites,
        std::string& error, bool cpuExpanded = false);
    Statistics GetStatistics() const;
private:
    struct State;
    std::unique_ptr<State> m_state;
};
}
