#pragma once

#include <any>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Rendering/Data/StateMask.h"
#include "Rendering/Data/PipelineState.h"
#include "Rendering/Resources/MaterialLayout.h"
#include "Rendering/Resources/MaterialParameterBlock.h"
#include "Rendering/Resources/ResourceBinding.h"
#include "Rendering/Settings/EComparaisonAlgorithm.h"
#include "Rendering/Settings/ECullFace.h"
#include "Rendering/Settings/EPrimitiveMode.h"
#include "RenderDef.h"

namespace NLS::Render::Resources
{
    class MaterialResourceSet;
    struct MaterialPipelineStateOverrides;
    class Material;
    class Shader;
    struct ShaderPropertyDesc;
    class Texture2D;
}

namespace NLS::Render::RHI
{
    class RHIDevice;
    class RHIBindingLayout;
    class RHIBindingSet;
    class RHIPipelineLayout;
    class RHIGraphicsPipeline;
}

namespace NLS::Render::Context
{
class Driver;
}

namespace NLS::Render::Core
{
class ABaseRenderer;
}

namespace NLS::Render::Resources
{
struct MaterialPipelineStateOverrides
{
    std::optional<bool> depthWrite;
    std::optional<bool> colorWrite;
    std::optional<bool> depthTest;
    std::optional<bool> culling;
    std::optional<Settings::ECullFace> cullFace;
};

/**
 * A material is a combination of a shader and some settings (Material settings and shader settings)
 */
class NLS_RENDER_API Material
{
public:
    using ShaderType = Shader;
    using Texture2DType = Texture2D;

    /**
     * Creates a material
     * @param p_shader
     */
    Material(Shader* p_shader = nullptr);
    ~Material();

    /**
     * Defines the shader to attach to this material instance
     * @param p_shader
     */
    void SetShader(Shader* p_shader);

    /**
     * Fill uniform with default uniform values
     */
    void FillUniform();

    void SyncParameterLayout();

    void RebuildBindingLayout();
    void RebuildBindingSet() const;

    /**
     * Set a shader uniform value
     * @param p_key
     * @param p_value
     */
    template<typename T>
    void Set(const std::string& p_key, const T& p_value);

    /**
     * Set a shader uniform value
     * @param p_key
     */
    template<typename T>
    const T& Get(const std::string p_key) const;

    /**
     * Returns the attached shader
     */
    Shader*& GetShader();
    const Shader* GetShader() const;

    /**
     * Returns true if the material has a shader attached
     */
    bool HasShader() const;

    /**
     * Returns true if the material is valid
     */
    bool IsValid() const;

    /**
     * Defines if the material is blendable
     * @param p_blendable
     */
    void SetBlendable(bool p_blendable);

    /**
     * Defines if the material has backface culling
     * @param p_backfaceCulling
     */
    void SetBackfaceCulling(bool p_backfaceCulling);

    /**
     * Defines if the material has frontface culling
     * @param p_frontfaceCulling
     */
    void SetFrontfaceCulling(bool p_frontfaceCulling);

    /**
     * Defines if the material has depth test
     * @param p_depthTest
     */
    void SetDepthTest(bool p_depthTest);

    /**
     * Defines if the material has depth writting
     * @param p_depthWriting
     */
    void SetDepthWriting(bool p_depthWriting);

    /**
     * Defines if the material has color writting
     * @param p_colorWriting
     */
    void SetColorWriting(bool p_colorWriting);

    /**
     * Defines the number of instances
     * @param p_instances
     */
    void SetGPUInstances(int p_instances);

    /**
     * Returns true if the material is blendable
     */
    bool IsBlendable() const;

    /**
     * Returns true if the material has backface culling
     */
    bool HasBackfaceCulling() const;

    /**
     * Returns true if the material has frontface culling
     */
    bool HasFrontfaceCulling() const;

    /**
     * Returns true if the material has depth test
     */
    bool HasDepthTest() const;

    /**
     * Returns true if the material has depth writing
     */
    bool HasDepthWriting() const;

    /**
     * Returns true if the material has color writing
     */
    bool HasColorWriting() const;

    /**
     * Returns the number of instances
     */
    int GetGPUInstances() const;

    /**
     * Generate a state mask with the current material settings
     */
    const Data::StateMask GenerateStateMask() const;

    MaterialParameterBlock& GetParameterBlock();
    const MaterialParameterBlock& GetParameterBlock() const;

    /**
     * Returns the uniforms data of the material
     */
    std::map<std::string, std::any>& GetUniformsData();

    struct MaterialRuntimeState;  // Forward declaration for public methods

    // Formal RHI methods - exposed for renderer direct access
    std::shared_ptr<RHI::RHIGraphicsPipeline> BuildRecordedGraphicsPipeline(
        const std::shared_ptr<RHI::RHIDevice>& device,
        Settings::EPrimitiveMode primitiveMode,
        const Data::PipelineState& pipelineState,
        MaterialPipelineStateOverrides overrides = {},
        bool* hasPipelineLayout = nullptr,
        bool* hasVertexShader = nullptr,
        bool* hasFragmentShader = nullptr) const;
    MaterialRuntimeState& GetRuntimeState() const;
    void ResetRuntimeState() const;
    void InvalidateExplicitBindingSetCache() const;
    std::shared_ptr<RHI::RHIBindingSet> GetRecordedBindingSet(const std::shared_ptr<RHI::RHIDevice>& device) const;
    const MaterialResourceSet& GetBindingSet() const;
    const std::shared_ptr<RHI::RHIBindingLayout>& GetExplicitBindingLayout(const std::shared_ptr<RHI::RHIDevice>& device) const;
    const std::shared_ptr<RHI::RHIBindingSet>& GetExplicitBindingSet(const std::shared_ptr<RHI::RHIDevice>& device) const;
    const std::shared_ptr<RHI::RHIPipelineLayout>& GetExplicitPipelineLayout(const std::shared_ptr<RHI::RHIDevice>& device) const;

    const std::string path;

protected:
    const ShaderPropertyDesc* FindMaterialProperty(const std::string& key) const;
    bool EnsureMaterialParameterExists(const std::string& key);

    Shader* m_shader = nullptr;
    MaterialParameterBlock m_parameterBlock;
    MaterialLayout m_materialLayout;
    ResourceBindingLayout m_bindingLayout;
    mutable std::unique_ptr<MaterialRuntimeState> m_runtimeState;

    bool m_blendable = false;
    bool m_backfaceCulling = true;
    bool m_frontfaceCulling = false;
    bool m_depthTest = true;
    bool m_depthWriting = true;
    bool m_colorWriting = true;
    int m_gpuInstances = 1;

private:
    friend class NLS::Render::Context::Driver;
    friend class NLS::Render::Core::ABaseRenderer;
};
} // namespace NLS::Render::Data

#include "Rendering/Resources/Material.inl"
