#pragma once

#include <array>
#include <memory>
#include <span>
#include <Math/Matrix4.h>
#include <Math/Vector3.h>
#include <Rendering/Context/RenderScenePackage.h>
#include <Rendering/RHI/Core/RHIDevice.h>
#include <Rendering/Debug/DebugDrawTypes.h>

namespace NLS::Render::Resources { class Shader; }
namespace NLS::Editor::Rendering
{
struct EditorGridShaders
{
    Render::RHI::RHIShaderModuleDesc vertex, fragment;
    uint64_t generation = 0;
};
std::shared_ptr<const EditorGridShaders> CaptureEditorGridShaders(const Render::Resources::Shader& shader,
    Render::RHI::NativeBackendType backend);
std::array<Render::Debug::DebugDrawPrimitive,3> BuildEditorOriginAxes(const Maths::Vector3& eye,float extent = 5000.0f);

// Shared by Scene/Asset View grid passes and embedded asset previews.
class EditorGridRenderer final
{
public:
    EditorGridRenderer();
    ~EditorGridRenderer();
    std::optional<Render::Context::RecordedDrawCommandInput> Prepare(
        std::shared_ptr<Render::RHI::RHIDevice> device,
        const std::shared_ptr<const EditorGridShaders>& shaders,
        const Render::RHI::RHIRenderTargetLayoutDesc& targets,
        const Maths::Matrix4& viewProjection,const Maths::Vector3& eye,
        const Maths::Vector3& color,std::string& error);
    std::optional<std::array<Render::Context::RecordedDrawCommandInput,3>> PrepareAxes(
        std::shared_ptr<Render::RHI::RHIDevice> device,
        const std::shared_ptr<const EditorGridShaders>& shaders,
        const Render::RHI::RHIRenderTargetLayoutDesc& targets,
        const Maths::Matrix4& viewProjection,const Maths::Vector3& eye,std::string& error);
    static void Record(Render::RHI::RHICommandBuffer& commands,
        const Render::Context::RecordedDrawCommandInput& draw,
        const Render::RHI::RHIRenderPassDesc& pass);
private:
    struct State;
    std::optional<Render::Context::RecordedDrawCommandInput> PrepareHelper(
        std::unique_ptr<State>& state,bool axes,std::shared_ptr<Render::RHI::RHIDevice> device,
        const std::shared_ptr<const EditorGridShaders>& shaders,const Render::RHI::RHIRenderTargetLayoutDesc& targets,
        const Maths::Matrix4& viewProjection,const Maths::Vector3& eye,std::span<const std::byte> material,std::string& error);
    std::unique_ptr<State> m_state,m_axesState;
};
}
