#pragma once
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <VFX/SystemInstance.h>
#include <VFX/SpriteMaterial.h>
#include "Rendering/EditorGridRenderer.h"

namespace NLS::Editor::VFXTools
{
struct VfxSystemPreviewInput
{
    uint32_t width = 640, height = 480;
    NLS::VFX::SpriteView camera;
    std::string isolatedEmitter;
    NLS::VFX::Value background{0.025f,0.028f,0.032f,1};
    std::vector<std::vector<NLS::VFX::SpriteMaterial>> materials;
    std::vector<std::vector<std::shared_ptr<const NLS::VFX::MeshRenderGeometry>>> meshes;
    std::shared_ptr<const Rendering::EditorGridShaders> gridShaders;
    std::shared_ptr<const Rendering::EditorGridShaders> axesShaders;
    Maths::Vector3 gridEye{}, gridColor{0.176f,0.176f,0.176f};
    bool showGrid = false;
    bool showOriginAxis = false;
};
// Uses SystemInstance publications and SystemRenderProxy exactly as scene
// playback does. Every GPU simulation frame is consumed, including seeks.
class VfxSystemPreview final
{
public:
    VfxSystemPreview();
    ~VfxSystemPreview();
    void Update(std::shared_ptr<NLS::Render::RHI::RHIDevice> device,
        std::shared_ptr<const NLS::VFX::CompiledSystem> system,
        std::vector<std::shared_ptr<const NLS::VFX::SystemFrame>> frames,
        const std::filesystem::path& cacheRoot, VfxSystemPreviewInput input);
    bool CanAdvance() const;
    void Clear();
    std::shared_ptr<NLS::Render::RHI::RHITextureView> View();
    std::string Status() const;
private:
    struct State;
    std::shared_ptr<State> m_state;
    std::shared_ptr<NLS::Render::RHI::RHITextureView> m_display;
};
}
