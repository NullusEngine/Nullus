#include "Rendering/SelectionOutlineMaskRenderer.h"

#include <algorithm>
#include <any>
#include <cstring>
#include <limits>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include <Debug/Assertion.h>
#include <Profiling/Profiler.h>
#include <Rendering/Core/CompositeRenderer.h>
#include <Rendering/Entities/Drawable.h>
#include <Rendering/EngineDrawableDescriptor.h>
#include <Rendering/FrameGraph/ExternalResourceBridge.h>
#include <Rendering/Geometry/Vertex.h>
#include <Rendering/RHI/Core/RHISubresourceRangeUtils.h>
#include <Rendering/Resources/IndexedObjectDataShaderSupport.h>
#include <Rendering/Resources/Mesh.h>
#include <Rendering/Resources/Shader.h>
#include <Rendering/Resources/Texture2D.h>
#include <Rendering/Settings/EComparaisonAlgorithm.h>

#include "Core/EditorActions.h"
#include "Core/EditorResources.h"
#include "Rendering/EditorDefaultResources.h"
#include "Rendering/EditorPipelineStatePresets.h"
// SelectionOutlineMaskChannels.def is the cross-language SSoT for mask swizzles.

namespace
{
    using NLS::Render::Context::RenderPassCommandInput;
    using NLS::Render::Context::ResourceAccessMode;
    using NLS::Render::Context::TextureResourceAccess;
    using NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata;
    using NLS::Render::FrameGraph::ThreadedRenderScenePassRole;
    using NLS::Render::RHI::AccessMask;
    using NLS::Render::RHI::PipelineStageMask;
    using NLS::Render::RHI::ResourceState;
    using NLS::Editor::Rendering::SelectionMaskCaptureGroup;

    constexpr size_t kSelectionOutlineMaskAttachmentIndex = 0u;
    constexpr size_t kSelectionOutlineDualDepthCaptureMaxItems = 8u;
    constexpr uint64_t kSelectionOutlineSignatureHashSeed = 14695981039346656037ull;

    enum SelectionOutlineShaderPassMode : int
    {
#define NLS_SELECTION_OUTLINE_MASK_PASS_MODE(name, value) SelectionOutlinePassMode##name = value,
#include "Rendering/SelectionOutlineMaskPassModes.def"
#undef NLS_SELECTION_OUTLINE_MASK_PASS_MODE
    };

    NLS::Render::Geometry::Vertex MakeFullscreenVertex(float x, float y, float u, float v)
    {
        NLS::Render::Geometry::Vertex vertex{};
        vertex.position[0] = x;
        vertex.position[1] = y;
        vertex.position[2] = 0.0f;
        vertex.texCoords[0] = u;
        vertex.texCoords[1] = v;
        vertex.normals[2] = 1.0f;
        vertex.tangent[0] = 1.0f;
        vertex.bitangent[1] = 1.0f;
        return vertex;
    }

    std::unique_ptr<NLS::Render::Resources::Mesh> CreateFullscreenQuad()
    {
        std::vector<NLS::Render::Geometry::Vertex> vertices{
            MakeFullscreenVertex(-1.0f, -1.0f, 0.0f, 1.0f),
            MakeFullscreenVertex(-1.0f,  1.0f, 0.0f, 0.0f),
            MakeFullscreenVertex( 1.0f,  1.0f, 1.0f, 0.0f),
            MakeFullscreenVertex( 1.0f, -1.0f, 1.0f, 1.0f)
        };
        std::vector<uint32_t> indices{ 0u, 1u, 2u, 0u, 2u, 3u };
        return std::make_unique<NLS::Render::Resources::Mesh>(vertices, indices, 0u);
    }

    NLS::Render::RHI::RHISubresourceRange ResolveTextureViewRange(
        const std::shared_ptr<NLS::Render::RHI::RHITextureView>& view)
    {
        NLS::Render::RHI::RHISubresourceRange range{};
        if (view == nullptr || view->GetTexture() == nullptr)
            return range;

        const auto normalizedRange = NLS::Render::RHI::NormalizeTextureSubresourceRange(
            view->GetTexture()->GetDesc(),
            view->GetDesc().subresourceRange);
        return normalizedRange.has_value() ? *normalizedRange : range;
    }

    void AddTextureViewAccess(
        RenderPassCommandInput& passInput,
        const std::shared_ptr<NLS::Render::RHI::RHITextureView>& view,
        ResourceAccessMode mode,
        ResourceState state,
        PipelineStageMask stages,
        AccessMask access)
    {
        if (view == nullptr || view->GetTexture() == nullptr)
            return;

        TextureResourceAccess resourceAccess;
        resourceAccess.texture = view->GetTexture();
        resourceAccess.subresourceRange = ResolveTextureViewRange(view);
        resourceAccess.mode = mode;
        resourceAccess.state = state;
        resourceAccess.stages = stages;
        resourceAccess.access = access;
        passInput.textureResourceAccesses.push_back(std::move(resourceAccess));
    }

    void ConfigureCommonPassInput(
        RenderPassCommandInput& passInput,
        const char* debugName,
        const NLS::Render::Data::FrameDescriptor& frameDescriptor)
    {
        passInput.kind = NLS::Render::Context::RenderPassCommandKind::Helper;
        passInput.debugName = debugName;
        passInput.queueType = NLS::Render::RHI::QueueType::Graphics;
        passInput.queueDependencyPolicy = NLS::Render::Context::QueueDependencyPolicy::Previous;
        passInput.targetsSwapchain = NLS::Render::FrameGraph::FrameTargetsSwapchain(frameDescriptor);
        passInput.renderWidth = frameDescriptor.renderWidth;
        passInput.renderHeight = frameDescriptor.renderHeight;
        passInput.usesColorAttachment = true;
        passInput.clearColor = true;
        passInput.clearDepth = false;
        passInput.clearStencil = false;
        passInput.clearColorValue = NLS::Maths::Vector4::Zero;
        passInput.requiresFrameData = true;
    }

    NLS::Render::Data::PipelineState CreateSelectionMaskVisiblePipelineState(
        NLS::Render::Data::PipelineState pso)
    {
        pso.blending = false;
        pso.depthTest = true;
        pso.depthFunc = NLS::Render::Settings::EComparaisonAlgorithm::LESS_EQUAL;
        pso.depthWriting = false;
        pso.stencilTest = false;
        pso.stencilWriteMask = 0u;
        pso.colorWriting.mask = 0x0F;
        pso.culling = false;
        return pso;
    }

    NLS::Render::Data::PipelineState CreateSelectionMaskOccludedPipelineState(
        NLS::Render::Data::PipelineState pso)
    {
        pso.blending = false;
        pso.depthTest = true;
        pso.depthFunc = NLS::Render::Settings::EComparaisonAlgorithm::ALWAYS;
        pso.depthWriting = false;
        pso.stencilTest = false;
        pso.stencilWriteMask = 0u;
        pso.colorWriting.mask = 0x0F;
        pso.culling = false;
        return pso;
    }

    NLS::Render::Data::PipelineState CreateSelectionFullscreenPipelineState(
        NLS::Render::Data::PipelineState pso)
    {
        pso.blending = false;
        pso.depthTest = false;
        pso.depthWriting = false;
        pso.stencilTest = false;
        pso.stencilWriteMask = 0u;
        pso.culling = false;
        pso.colorWriting.mask = 0x0F;
        return pso;
    }

    struct SelectionMaskCaptureGroupKey
    {
        NLS::Render::Resources::Mesh* mesh = nullptr;
        float selectionGroupId = 1.0f;
        float selectionClassification = NLS::Editor::Rendering::kSelectionOutlineParentClassification;

        bool operator==(const SelectionMaskCaptureGroupKey& other) const
        {
            return mesh == other.mesh &&
                selectionGroupId == other.selectionGroupId &&
                selectionClassification == other.selectionClassification;
        }
    };

    struct SelectionMaskCaptureGroupKeyHasher
    {
        size_t operator()(const SelectionMaskCaptureGroupKey& key) const
        {
            size_t seed = std::hash<NLS::Render::Resources::Mesh*>{}(key.mesh);
            const auto combine = [&seed](const size_t value)
            {
                seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6u) + (seed >> 2u);
            };
            combine(std::hash<float>{}(key.selectionGroupId));
            combine(std::hash<float>{}(key.selectionClassification));
            return seed;
        }
    };

    using SelectionMaskCaptureGroupLookup =
        std::unordered_map<SelectionMaskCaptureGroupKey, size_t, SelectionMaskCaptureGroupKeyHasher>;

    void AddSelectionMaskCaptureGroup(
        std::vector<SelectionMaskCaptureGroup>& groups,
        SelectionMaskCaptureGroupLookup& groupLookup,
        NLS::Render::Resources::Mesh* mesh,
        const NLS::Maths::Matrix4& modelMatrix,
        const float selectionGroupId,
        const float selectionClassification)
    {
        if (mesh == nullptr)
            return;

        const SelectionMaskCaptureGroupKey key{ mesh, selectionGroupId, selectionClassification };
        const auto groupIt = groupLookup.find(key);
        if (groupIt == groupLookup.end())
        {
            SelectionMaskCaptureGroup newGroup;
            newGroup.mesh = mesh;
            newGroup.selectionGroupId = selectionGroupId;
            newGroup.selectionClassification = selectionClassification;
            groups.push_back(std::move(newGroup));
            const auto groupIndex = groups.size() - 1u;
            groupLookup.emplace(key, groupIndex);
            groups[groupIndex].instanceModelMatrices.push_back(modelMatrix);
            return;
        }

        groups[groupIt->second].instanceModelMatrices.push_back(modelMatrix);
    }

    std::vector<SelectionMaskCaptureGroup> BuildSelectionMaskCaptureGroups(
        const NLS::Editor::Rendering::DebugGameObjectDebugDrawItems& debugDrawItems,
        NLS::Render::Resources::Mesh* cameraMesh)
    {
        std::vector<SelectionMaskCaptureGroup> groups;
        groups.reserve(debugDrawItems.selectionMeshItems.size() + debugDrawItems.cameras.size());
        SelectionMaskCaptureGroupLookup groupLookup;
        groupLookup.reserve(groups.capacity());

        for (const auto& selectionItem : debugDrawItems.selectionMeshItems)
        {
            AddSelectionMaskCaptureGroup(
                groups,
                groupLookup,
                selectionItem.mesh,
                selectionItem.worldMatrix,
                selectionItem.selectionGroupId,
                selectionItem.selectionClassification);
        }

        if (cameraMesh == nullptr)
            return groups;

        for (const auto& cameraItem : debugDrawItems.cameras)
        {
            auto* cameraComponent = cameraItem.cameraComponent;
            const auto* actor = cameraComponent != nullptr ? cameraComponent->gameobject() : nullptr;
            if (actor == nullptr)
                continue;

            const auto translation = NLS::Maths::Matrix4::Translation(cameraItem.worldPosition);
            const auto rotation = NLS::Maths::Quaternion::ToMatrix4(cameraItem.worldRotation);
            AddSelectionMaskCaptureGroup(
                groups,
                groupLookup,
                cameraMesh,
                translation * rotation,
                cameraItem.selectionGroupId,
                cameraItem.selectionClassification);
        }

        return groups;
    }

    NLS::Render::Data::PipelineState CreateSelectionCompositePipelineState(
        NLS::Render::Data::PipelineState pso)
    {
        pso = CreateSelectionFullscreenPipelineState(pso);
        pso.blending = true;
        return pso;
    }

    bool SameViewDesc(
        const NLS::Render::RHI::RHITextureViewDesc& left,
        const NLS::Render::RHI::RHITextureViewDesc& right)
    {
        return left.viewType == right.viewType &&
            left.format == right.format &&
            left.subresourceRange.baseMipLevel == right.subresourceRange.baseMipLevel &&
            left.subresourceRange.mipLevelCount == right.subresourceRange.mipLevelCount &&
            left.subresourceRange.baseArrayLayer == right.subresourceRange.baseArrayLayer &&
            left.subresourceRange.arrayLayerCount == right.subresourceRange.arrayLayerCount;
    }

    bool HasCompleteScreenSpacePasses(const std::vector<RenderPassCommandInput>& passInputs)
    {
        return !passInputs.empty() &&
            std::all_of(
                passInputs.begin(),
                passInputs.end(),
                [](const RenderPassCommandInput& passInput)
                {
                    return passInput.drawCount > 0u && !passInput.recordedDrawCommands.empty();
                });
    }

    bool MaterialRequestsAlphaMask(const NLS::Render::Resources::Material& material)
    {
        const auto* alphaClip = material.GetParameterBlock().TryGet("u_AlphaClip");
        if (alphaClip != nullptr)
        {
            if (alphaClip->type() == typeid(float) && std::any_cast<float>(*alphaClip) > 0.5f)
                return true;
            if (alphaClip->type() == typeid(bool) && std::any_cast<bool>(*alphaClip))
                return true;
        }

        const auto* maskMap = material.GetParameterBlock().TryGet("u_MaskMap");
        if (maskMap == nullptr || maskMap->type() != typeid(NLS::Render::Resources::Texture2D*))
            return false;

        auto* texture = std::any_cast<NLS::Render::Resources::Texture2D*>(*maskMap);
        return texture != nullptr && texture->path != ":Generated/DefaultWhiteTexture";
    }

    NLS::Render::RHI::NativeBackendType ResolveSelectionOutlineBackend(
        const std::shared_ptr<NLS::Render::RHI::RHIDevice>& device)
    {
        if (device == nullptr)
            return NLS::Render::RHI::NativeBackendType::None;

        const auto nativeInfo = device->GetNativeDeviceInfo();
        if (nativeInfo.backend != NLS::Render::RHI::NativeBackendType::None)
            return nativeInfo.backend;

        const auto& adapter = device->GetAdapter();
        return adapter != nullptr
            ? adapter->GetBackendType()
            : NLS::Render::RHI::NativeBackendType::None;
    }

    bool HasUnsupportedMaterialMask(const NLS::Editor::Rendering::DebugGameObjectDebugDrawItems& debugDrawItems)
    {
        return std::any_of(
            debugDrawItems.selectionMeshItems.begin(),
            debugDrawItems.selectionMeshItems.end(),
            [](const NLS::Editor::Rendering::DebugGameObjectDebugDrawItems::SelectionMeshItem& item)
            {
                return item.sourceMaterial != nullptr && MaterialRequestsAlphaMask(*item.sourceMaterial);
            });
    }

    template<typename T>
    bool MaterialParameterEquals(const std::any* value, const T& expected)
    {
        if (value == nullptr || value->type() != typeid(T))
            return false;

        return std::any_cast<const T&>(*value) == expected;
    }

    template<typename T>
    void SetMaterialValueIfChanged(
        NLS::Render::Resources::Material& material,
        const std::string& name,
        const T& value)
    {
        if (MaterialParameterEquals(material.GetParameterBlock().TryGet(name), value))
            return;

        material.Set(name, value);
    }

    void SetSelectionMaskPassModeIfChanged(
        NLS::Render::Resources::Material& material,
        const std::string& name,
        const int value)
    {
        SetMaterialValueIfChanged(material, name, value);
    }

    void SetMaterialTextureIfChanged(
        NLS::Render::Resources::Material& material,
        const std::string& name,
        NLS::Render::Resources::Texture2D* texture)
    {
        if (MaterialParameterEquals(material.GetParameterBlock().TryGet(name), texture))
            return;

        material.Set<NLS::Render::Resources::Texture2D*>(name, texture);
    }

    bool WrapSelectionOutlineExternalTexture(
        std::unique_ptr<NLS::Render::Resources::Texture2D>& texture,
        const std::shared_ptr<NLS::Render::RHI::RHITextureView>& view,
        const uint32_t width,
        const uint32_t height,
        bool& textureHandleChanged)
    {
        if (view == nullptr || view->GetTexture() == nullptr)
            return false;

        const auto previousTextureHandle = texture != nullptr
            ? texture->GetTextureHandle()
            : nullptr;
        if (texture == nullptr)
        {
            texture = NLS::Render::Resources::Texture2D::WrapExternal(
                view->GetTexture(),
                width,
                height);
        }
        else
        {
            texture->WrapExternalInPlace(
                view->GetTexture(),
                width,
                height);
        }

        textureHandleChanged = textureHandleChanged || previousTextureHandle != texture->GetTextureHandle();
        return texture != nullptr;
    }

    bool SetShaderIfChanged(
        NLS::Render::Resources::Material& material,
        NLS::Render::Resources::Shader* shader)
    {
        if (material.GetShader() == shader)
            return false;

        material.SetShader(shader);
        return true;
    }

    void BindSelectionOutlineTextureFallbacks(
        NLS::Render::Resources::Material& material,
        NLS::Render::Resources::Texture2D* fallbackTexture)
    {
        SetMaterialTextureIfChanged(material, "u_SelectionOutlineMask", fallbackTexture);
        SetMaterialTextureIfChanged(material, "u_MainTexture", fallbackTexture);
    }

    void EnsureSelectionOutlineMaterial(
        NLS::Render::Resources::Material& material,
        NLS::Render::Resources::Shader* shader,
        NLS::Render::Resources::Texture2D* fallbackTexture)
    {
        if (SetShaderIfChanged(material, shader))
            BindSelectionOutlineTextureFallbacks(material, fallbackTexture);
    }

    void EnsureSelectionOutlineCompositeMaterial(
        NLS::Render::Resources::Material& material,
        NLS::Render::Resources::Shader* shader,
        NLS::Render::Resources::Texture2D* fallbackTexture)
    {
        if (SetShaderIfChanged(material, shader))
            BindSelectionOutlineTextureFallbacks(material, fallbackTexture);
    }

    void EnsureSelectionOutlineMaterials(
        NLS::Render::Resources::Material& maskVisibleMaterial,
        NLS::Render::Resources::Material& maskOccludedMaterial,
        NLS::Render::Resources::Material& compositeMaterial,
        NLS::Render::Resources::Shader* maskShader,
        NLS::Render::Resources::Shader* compositeShader)
    {
        auto* fallbackTexture = NLS::Editor::Rendering::GetEditorDefaultWhiteTexture();
        EnsureSelectionOutlineMaterial(maskVisibleMaterial, maskShader, fallbackTexture);
        EnsureSelectionOutlineMaterial(maskOccludedMaterial, maskShader, fallbackTexture);
        EnsureSelectionOutlineCompositeMaterial(compositeMaterial, compositeShader, fallbackTexture);
    }

    void HashCombine(uint64_t& seed, const uint64_t value)
    {
        seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6u) + (seed >> 2u);
    }

    void HashCombinePointer(uint64_t& seed, const void* value)
    {
        HashCombine(seed, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(value)));
    }

    void HashCombineFloat(uint64_t& seed, const float value)
    {
        uint32_t bits = 0u;
        std::memcpy(&bits, &value, sizeof(bits));
        HashCombine(seed, static_cast<uint64_t>(bits));
    }

    void HashCombineMatrix(uint64_t& seed, const NLS::Maths::Matrix4& matrix)
    {
        for (const float value : matrix.data)
            HashCombineFloat(seed, value);
    }

    void HashCombineVector3(uint64_t& seed, const NLS::Maths::Vector3& vector)
    {
        HashCombineFloat(seed, vector.x);
        HashCombineFloat(seed, vector.y);
        HashCombineFloat(seed, vector.z);
    }

    void HashCombineQuaternion(uint64_t& seed, const NLS::Maths::Quaternion& quaternion)
    {
        HashCombineFloat(seed, quaternion.x);
        HashCombineFloat(seed, quaternion.y);
        HashCombineFloat(seed, quaternion.z);
        HashCombineFloat(seed, quaternion.w);
    }

    bool SameMatrix(
        const NLS::Maths::Matrix4& left,
        const NLS::Maths::Matrix4& right)
    {
        for (size_t index = 0u; index < std::size(left.data); ++index)
        {
            if (left.data[index] != right.data[index])
                return false;
        }
        return true;
    }
}

namespace NLS::Editor::Rendering
{
    bool SelectionOutlineMaskRenderer::ResourceIdentity::Matches(
        const ResourceIdentity& other) const
    {
        return width == other.width &&
            height == other.height &&
            sceneDepthExtentWidth == other.sceneDepthExtentWidth &&
            sceneDepthExtentHeight == other.sceneDepthExtentHeight &&
            outputColorExtentWidth == other.outputColorExtentWidth &&
            outputColorExtentHeight == other.outputColorExtentHeight &&
            maskFormat == other.maskFormat &&
            sampleCount == other.sampleCount &&
            depthSampleCount == other.depthSampleCount &&
            sceneDepthTexture == other.sceneDepthTexture &&
            outputColorTexture == other.outputColorTexture &&
            SameViewDesc(sceneDepthViewDesc, other.sceneDepthViewDesc) &&
            SameViewDesc(outputColorViewDesc, other.outputColorViewDesc);
    }

    bool SelectionOutlineMaskRenderer::MaskReuseSignature::Matches(
        const MaskReuseSignature& other) const
    {
        return resourceIdentity.Matches(other.resourceIdentity) &&
            selectedItemCount == other.selectedItemCount &&
            groupCount == other.groupCount &&
            instanceCount == other.instanceCount &&
            selectedTreeHash == other.selectedTreeHash &&
            meshContentRevisionHash == other.meshContentRevisionHash &&
            camera == other.camera &&
            SameMatrix(viewMatrix, other.viewMatrix) &&
            SameMatrix(projectionMatrix, other.projectionMatrix);
    }

    bool SelectionOutlineMaskRenderer::SelectionSourceSignature::Matches(
        const SelectionSourceSignature& other) const
    {
        return selectedItemCount == other.selectedItemCount &&
            selectionMeshItemCount == other.selectionMeshItemCount &&
            cameraItemCount == other.cameraItemCount &&
            hasCameraMesh == other.hasCameraMesh &&
            cameraMeshContentRevision == other.cameraMeshContentRevision &&
            selectedTreeHash == other.selectedTreeHash &&
            meshContentRevisionHash == other.meshContentRevisionHash;
    }

    SelectionOutlineMaskRenderer::SelectionOutlineMaskRenderer(
        NLS::Render::Core::CompositeRenderer& renderer)
        : m_renderer(renderer)
    {
        auto* shader = EDITOR_CONTEXT(editorResources)->GetShader("SelectionOutlineMask");
        auto* compositeShader = EDITOR_CONTEXT(editorResources)->GetShader("SelectionOutlineComposite");
        EnsureSelectionOutlineMaterials(
            m_maskVisibleMaterial,
            m_maskOccludedMaterial,
            m_compositeMaterial,
            shader,
            compositeShader);
        m_compositeMaterial.SetBlendable(true);
        SetSelectionMaskPassModeIfChanged(m_maskVisibleMaterial, "u_SelectionOutlinePassMode", SelectionOutlinePassModeCaptureVisible);
        SetSelectionMaskPassModeIfChanged(m_maskOccludedMaterial, "u_SelectionOutlinePassMode", SelectionOutlinePassModeCaptureOccluded);
        SetMaterialValueIfChanged(m_maskVisibleMaterial, "u_AlphaCutoff", 0.5f);
        SetMaterialValueIfChanged(m_maskOccludedMaterial, "u_AlphaCutoff", 0.5f);
    }

    std::vector<ThreadedRenderScenePassMetadata> SelectionOutlineMaskRenderer::BuildMetadata(
        const std::vector<RenderPassCommandInput>& passInputs,
        const std::vector<SelectionOutlineMaskPassKind>& passKinds)
    {
        std::vector<ThreadedRenderScenePassMetadata> metadata;
        NLS_ASSERT(
            passInputs.size() == passKinds.size(),
            "Selection outline pass inputs and pass kinds must stay paired.");
        if (passInputs.size() != passKinds.size())
            return metadata;

        metadata.reserve(passInputs.size());
        for (size_t passIndex = 0u; passIndex < passInputs.size(); ++passIndex)
        {
            const auto& passInput = passInputs[passIndex];
            const auto passKind = passKinds[passIndex];
            ThreadedRenderScenePassMetadata entry;
            entry.commandKind = NLS::Render::Context::RenderPassCommandKind::Helper;
            entry.role = ThreadedRenderScenePassRole::Helper;
            entry.queueType = NLS::Render::RHI::QueueType::Graphics;
            entry.queueDependencyPolicy = NLS::Render::Context::QueueDependencyPolicy::Previous;
            entry.visibleDrawCountContribution = passInput.drawCount;
            entry.propagatesColorOutput = SelectionOutlineMaskPassPropagatesColorOutput(passKind);
            entry.propagatesDepthOutput = false;
            NLS::Render::FrameGraph::SetThreadedRenderScenePassGraphPassName(
                entry,
                passInput.debugName.c_str());
            metadata.push_back(entry);
        }
        return metadata;
    }

    SelectionOutlinePreparedOutput SelectionOutlineMaskRenderer::BuildPreparedOutput(
        const DebugGameObjectDebugDrawItems& debugDrawItems,
        const Maths::Vector4& parentColor,
        const Maths::Vector4& childColor,
        NLS::Render::Data::PipelineState basePipelineState,
        std::shared_ptr<NLS::Render::RHI::RHITextureView> sceneDepthViewOverride)
    {
        NLS_PROFILE_NAMED_SCOPE("SelectionOutlineMask::BuildPreparedOutput");
        SelectionOutlinePreparedOutput output;
        output.passInputs.reserve(kPassNames.size());
        output.passKinds.reserve(kPassNames.size());
        output.selectedItemCount =
            static_cast<uint64_t>(debugDrawItems.selectionMeshItems.size() + debugDrawItems.cameras.size());
        if (output.selectedItemCount == 0u)
        {
            InvalidateCachedMask();
            return output;
        }

        auto* shader = EDITOR_CONTEXT(editorResources)->GetShader("SelectionOutlineMask");
        auto* compositeShader = EDITOR_CONTEXT(editorResources)->GetShader("SelectionOutlineComposite");
        if (shader == nullptr || compositeShader == nullptr)
        {
            InvalidateCachedMask();
            SetFallbackReason(SelectionOutlineFallbackReason::MissingShader);
            output.fallbackDecision = { m_lastFallbackReason, output.selectedItemCount };
            return output;
        }
        if (NLS::Render::Resources::ShaderSupportsIndexedObjectData(*shader))
        {
            const auto device = NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(m_renderer.GetDriver());
            const auto backend = ResolveSelectionOutlineBackend(device);
            if (!NLS::Render::Resources::BackendSupportsIndexedObjectDataPushConstants(backend))
            {
                InvalidateCachedMask();
                SetFallbackReason(SelectionOutlineFallbackReason::UnsupportedBackend);
                output.fallbackDecision = { m_lastFallbackReason, output.selectedItemCount };
                return output;
            }
        }
        if (HasUnsupportedMaterialMask(debugDrawItems))
        {
            InvalidateCachedMask();
            SetFallbackReason(SelectionOutlineFallbackReason::UnsupportedMaterialMask);
            output.fallbackDecision = { m_lastFallbackReason, output.selectedItemCount };
            return output;
        }

        auto selectionFrameDescriptor = NLS::Render::FrameGraph::CaptureExternalSceneOutputSnapshot(
            m_renderer.GetFrameDescriptor());
        if (sceneDepthViewOverride != nullptr)
        {
            selectionFrameDescriptor.outputDepthStencilView = sceneDepthViewOverride;
            selectionFrameDescriptor.outputDepthStencilTexture = sceneDepthViewOverride->GetTexture();
        }
        PreparedResources resources;
        if (!PrepareResources(selectionFrameDescriptor, resources))
        {
            output.fallbackDecision = { m_lastFallbackReason, output.selectedItemCount };
            return output;
        }

        EnsureSelectionOutlineMaterials(
            m_maskVisibleMaterial,
            m_maskOccludedMaterial,
            m_compositeMaterial,
            shader,
            compositeShader);
        SetSelectionMaskPassModeIfChanged(m_maskVisibleMaterial, "u_SelectionOutlinePassMode", SelectionOutlinePassModeCaptureVisible);
        SetSelectionMaskPassModeIfChanged(m_maskOccludedMaterial, "u_SelectionOutlinePassMode", SelectionOutlinePassModeCaptureOccluded);
        SetMaterialValueIfChanged(m_maskVisibleMaterial, "u_AlphaCutoff", 0.5f);
        SetMaterialValueIfChanged(m_maskOccludedMaterial, "u_AlphaCutoff", 0.5f);
        SetMaterialValueIfChanged(m_compositeMaterial, "u_OutlineColor", parentColor);
        SetMaterialValueIfChanged(m_compositeMaterial, "u_ChildOutlineColor", childColor);

        const auto texelSize = Maths::Vector4(
            selectionFrameDescriptor.renderWidth > 0u ? 1.0f / static_cast<float>(selectionFrameDescriptor.renderWidth) : 0.0f,
            selectionFrameDescriptor.renderHeight > 0u ? 1.0f / static_cast<float>(selectionFrameDescriptor.renderHeight) : 0.0f,
            static_cast<float>(selectionFrameDescriptor.renderWidth),
            static_cast<float>(selectionFrameDescriptor.renderHeight));
        SetMaterialValueIfChanged(m_maskVisibleMaterial, "u_TexelSize", texelSize);
        SetMaterialValueIfChanged(m_maskOccludedMaterial, "u_TexelSize", texelSize);
        SetMaterialValueIfChanged(m_compositeMaterial, "u_TexelSize", texelSize);

        if (!BindScreenSpaceMaterialTextures(selectionFrameDescriptor, resources))
        {
            InvalidateCachedMask();
            output.fallbackDecision = { m_lastFallbackReason, output.selectedItemCount };
            return output;
        }

        auto* cameraMesh = EDITOR_CONTEXT(editorResources)->GetMesh("Camera");
        const auto selectionSourceSignature = BuildSelectionSourceSignature(
            debugDrawItems,
            selectionFrameDescriptor,
            cameraMesh);
        std::optional<MaskReuseSignature> maskReuseSignature;
        const auto& preparedSelection = ResolveSelectionCaptureGroups(
            debugDrawItems,
            selectionSourceSignature,
            selectionFrameDescriptor,
            cameraMesh);
        maskReuseSignature = preparedSelection.maskReuseSignature;
        const bool reuseCachedMask = CanReuseCachedMask(maskReuseSignature);
        const bool capturedNewMask = !reuseCachedMask;

        if (capturedNewMask)
        {
            std::vector<NLS::Render::Context::RecordedDrawCommandInput> maskDrawCommands;
            CaptureMaskDrawCommands(
                preparedSelection.groups,
                output.selectedItemCount,
                basePipelineState,
                maskDrawCommands);
            if (maskDrawCommands.empty())
            {
                InvalidateCachedMask();
                SetFallbackReason(SelectionOutlineFallbackReason::ScreenSpaceCommandCaptureFailed);
                output.fallbackDecision = { m_lastFallbackReason, output.selectedItemCount };
                return output;
            }

            output.passKinds.push_back(SelectionOutlineMaskPassKind::CaptureMask);
            output.passInputs.push_back(BuildPassInput(
                SelectionOutlineMaskPassKind::CaptureMask,
                selectionFrameDescriptor,
                resources,
                std::move(maskDrawCommands)));
        }

        output.passKinds.push_back(SelectionOutlineMaskPassKind::Composite);
        output.passInputs.push_back(BuildPassInput(
            SelectionOutlineMaskPassKind::Composite,
            selectionFrameDescriptor,
            resources,
            {}));
        if (!HasCompleteScreenSpacePasses(output.passInputs))
        {
            output.passInputs.clear();
            output.passKinds.clear();
            InvalidateCachedMask();
            SetFallbackReason(SelectionOutlineFallbackReason::ScreenSpaceCommandCaptureFailed);
            output.fallbackDecision = { m_lastFallbackReason, output.selectedItemCount };
            return output;
        }

        output.metadata = BuildMetadata(output.passInputs, output.passKinds);
        if (output.metadata.size() != output.passInputs.size())
        {
            output.passInputs.clear();
            output.passKinds.clear();
            output.metadata.clear();
            DiscardPendingCachedMask();
            SetFallbackReason(SelectionOutlineFallbackReason::ScreenSpaceCommandCaptureFailed);
            output.fallbackDecision = { m_lastFallbackReason, output.selectedItemCount };
            return output;
        }
        if (capturedNewMask)
            MarkCachedMaskPending(maskReuseSignature);
        SetFallbackReason(SelectionOutlineFallbackReason::None);
        return output;
    }

    void SelectionOutlineMaskRenderer::CaptureMaskDrawCommands(
        const std::vector<SelectionMaskCaptureGroup>& groups,
        const uint64_t selectedItemCount,
        NLS::Render::Data::PipelineState pso,
        std::vector<NLS::Render::Context::RecordedDrawCommandInput>& outDrawCommands)
    {
        NLS_PROFILE_NAMED_SCOPE("SelectionOutlineMask::CaptureMask");
        const bool enableVisibleDepthRefinement = selectedItemCount <= kSelectionOutlineDualDepthCaptureMaxItems;
        outDrawCommands.reserve(groups.size() * (enableVisibleDepthRefinement ? 2u : 1u));

        if (enableVisibleDepthRefinement)
        {
            // Unity uses max blending plus ColorMask GBA for this contribution. Nullus
            // recorded helper commands do not expose that RHI override yet, so capture
            // selected coverage first and let a bounded visible pass overwrite front pixels.
            CaptureMaskDrawCommandsForGroups(
                groups,
                m_maskOccludedMaterial,
                SelectionOutlinePassModeCaptureOccluded,
                CreateSelectionMaskOccludedPipelineState(pso),
                outDrawCommands);
            CaptureMaskDrawCommandsForGroups(
                groups,
                m_maskVisibleMaterial,
                SelectionOutlinePassModeCaptureVisible,
                CreateSelectionMaskVisiblePipelineState(pso),
                outDrawCommands);
        }
        else
        {
            // Large selections skip the extra visible-depth refinement. Mark coverage
            // as visible-unknown so the composite does not fade every edge as occluded.
            CaptureMaskDrawCommandsForGroups(
                groups,
                m_maskVisibleMaterial,
                SelectionOutlinePassModeCaptureVisible,
                CreateSelectionMaskOccludedPipelineState(pso),
                outDrawCommands);
        }
    }

    void ApplySelectionMaskGroupParameters(
        NLS::Render::Resources::Material& material,
        const SelectionMaskCaptureGroup& group,
        const int passMode)
    {
        SetSelectionMaskPassModeIfChanged(material, "u_SelectionOutlinePassMode", passMode);
        SetMaterialValueIfChanged(material, "u_ObjectId", group.selectionGroupId);
        SetMaterialValueIfChanged(material, "u_SelectionClassification", group.selectionClassification);
    }

    void SelectionOutlineMaskRenderer::CaptureMaskDrawCommandsForGroups(
        const std::vector<SelectionMaskCaptureGroup>& groups,
        NLS::Render::Resources::Material& material,
        const int passMode,
        NLS::Render::Data::PipelineState pso,
        std::vector<NLS::Render::Context::RecordedDrawCommandInput>& outDrawCommands)
    {
        for (const auto& group : groups)
        {
            if (group.mesh == nullptr ||
                group.instanceModelMatrices.empty() ||
                group.instanceModelMatrices.size() > (std::numeric_limits<uint32_t>::max)())
            {
                continue;
            }

            NLS::Render::Entities::Drawable drawable;
            drawable.mesh = group.mesh;
            drawable.material = &material;
            drawable.instanceCount = static_cast<uint32_t>(group.instanceModelMatrices.size());
            NLS::Engine::Rendering::EngineDrawableDescriptor descriptor{
                group.instanceModelMatrices.front(),
                Maths::Matrix4::Identity
            };
            descriptor.objectCount = drawable.instanceCount;
            descriptor.instanceModelMatrices = group.instanceModelMatrices;
            drawable.AddDescriptor(NLS::Engine::Rendering::EngineDrawableDescriptor{
                descriptor
            });
            ApplySelectionMaskGroupParameters(material, group, passMode);
            drawable.stateMask = material.GenerateStateMask();

            NLS::Render::Context::RecordedDrawCommandInput drawCommand;
            if (m_renderer.CaptureRecordedDrawCommand(pso, drawable, drawCommand))
                outDrawCommands.push_back(std::move(drawCommand));
        }
    }

    void SelectionOutlineMaskRenderer::EnsureFullscreenResources()
    {
        if (m_fullscreenQuad == nullptr)
            m_fullscreenQuad = CreateFullscreenQuad();
    }

    void SelectionOutlineMaskRenderer::RecordFullscreenCommand(
        NLS::Render::Resources::Material& material,
        const NLS::Render::Data::FrameDescriptor& frameDescriptor,
        std::vector<NLS::Render::Context::RecordedDrawCommandInput>& outDrawCommands) const
    {
        NLS_PROFILE_NAMED_SCOPE("SelectionOutlineMask::RecordComposite");
        if (m_fullscreenQuad == nullptr)
            return;

        NLS::Render::Entities::Drawable drawable;
        drawable.mesh = m_fullscreenQuad.get();
        drawable.material = &material;
        drawable.primitiveMode = NLS::Render::Settings::EPrimitiveMode::TRIANGLES;
        drawable.stateMask = material.GenerateStateMask();
        drawable.AddDescriptor(NLS::Engine::Rendering::EngineDrawableDescriptor{
            Maths::Matrix4::Identity,
            Maths::Matrix4::Identity
        });

        NLS::Render::Resources::MaterialPipelineStateOverrides compositeOverrides;
        compositeOverrides.blending = true;
        compositeOverrides.depthTest = false;
        compositeOverrides.depthWrite = false;
        compositeOverrides.hasDepthAttachment = false;
        compositeOverrides.culling = false;
        compositeOverrides.colorWrite = true;
        if (frameDescriptor.outputColorView != nullptr)
        {
            const std::array<NLS::Render::RHI::TextureFormat, 1u> outputColorFormats {
                frameDescriptor.outputColorView->GetDesc().format
            };
            compositeOverrides.SetColorFormats(outputColorFormats);
        }

        NLS::Render::Context::RecordedDrawCommandInput drawCommand;
        if (m_renderer.CaptureRecordedDrawCommand(
                drawable,
                compositeOverrides,
                NLS::Render::Settings::EComparaisonAlgorithm::LESS,
                drawCommand))
        {
            outDrawCommands.push_back(std::move(drawCommand));
        }
    }

    std::optional<SelectionOutlineMaskRenderer::ResourceIdentity>
    SelectionOutlineMaskRenderer::BuildResourceIdentity(
        const NLS::Render::Data::FrameDescriptor& frameDescriptor) const
    {
        if (frameDescriptor.renderWidth == 0u || frameDescriptor.renderHeight == 0u)
            return std::nullopt;
        if (frameDescriptor.outputDepthStencilView == nullptr ||
            frameDescriptor.outputDepthStencilView->GetTexture() == nullptr)
        {
            return std::nullopt;
        }
        if (frameDescriptor.outputColorView == nullptr ||
            frameDescriptor.outputColorView->GetTexture() == nullptr)
        {
            return std::nullopt;
        }
        if (!SelectionOutlineFrameAttachmentsMatchRenderExtent(frameDescriptor))
            return std::nullopt;

        ResourceIdentity identity;
        identity.width = frameDescriptor.renderWidth;
        identity.height = frameDescriptor.renderHeight;
        identity.maskFormat = NLS::Render::RHI::TextureFormat::RGBA8;
        const auto& outputTextureDesc = frameDescriptor.outputColorView->GetTexture()->GetDesc();
        const auto& depthTextureDesc = frameDescriptor.outputDepthStencilView->GetTexture()->GetDesc();
        const uint32_t outputMipLevel = frameDescriptor.outputColorView->GetDesc().subresourceRange.baseMipLevel;
        const uint32_t depthMipLevel = frameDescriptor.outputDepthStencilView->GetDesc().subresourceRange.baseMipLevel;
        const auto outputSampleCount = outputTextureDesc.sampleCount;
        const auto depthSampleCount = depthTextureDesc.sampleCount;
        identity.outputColorExtentWidth = SelectionOutlineMipExtent(outputTextureDesc.extent.width, outputMipLevel);
        identity.outputColorExtentHeight = SelectionOutlineMipExtent(outputTextureDesc.extent.height, outputMipLevel);
        identity.sceneDepthExtentWidth = SelectionOutlineMipExtent(depthTextureDesc.extent.width, depthMipLevel);
        identity.sceneDepthExtentHeight = SelectionOutlineMipExtent(depthTextureDesc.extent.height, depthMipLevel);
        identity.sampleCount = outputSampleCount;
        identity.depthSampleCount = depthSampleCount;
        identity.sceneDepthTexture = frameDescriptor.outputDepthStencilView->GetTexture();
        identity.outputColorTexture = frameDescriptor.outputColorView->GetTexture();
        identity.sceneDepthViewDesc = frameDescriptor.outputDepthStencilView->GetDesc();
        identity.outputColorViewDesc = frameDescriptor.outputColorView->GetDesc();
        return identity;
    }

    bool SelectionOutlineMaskRenderer::PrepareResources(
        const NLS::Render::Data::FrameDescriptor& frameDescriptor,
        PreparedResources& outResources)
    {
        NLS_PROFILE_NAMED_SCOPE("SelectionOutlineMask::PrepareResources");
        if (frameDescriptor.renderWidth == 0u || frameDescriptor.renderHeight == 0u)
        {
            SetFallbackReason(SelectionOutlineFallbackReason::ZeroSizeTarget);
            ResetResources();
            return false;
        }
        if (frameDescriptor.outputDepthStencilView == nullptr ||
            frameDescriptor.outputDepthStencilView->GetTexture() == nullptr)
        {
            SetFallbackReason(SelectionOutlineFallbackReason::MissingSceneDepth);
            ResetResources();
            return false;
        }
        if (frameDescriptor.outputColorView == nullptr ||
            frameDescriptor.outputColorView->GetTexture() == nullptr)
        {
            SetFallbackReason(SelectionOutlineFallbackReason::MissingOutputColor);
            ResetResources();
            return false;
        }
        if (!SelectionOutlineFrameAttachmentsMatchRenderExtent(frameDescriptor))
        {
            SetFallbackReason(SelectionOutlineFallbackReason::StaleFrameAttachment);
            ResetResources();
            return false;
        }

        const auto nextIdentity = BuildResourceIdentity(frameDescriptor);
        if (!nextIdentity.has_value())
        {
            SetFallbackReason(SelectionOutlineFallbackReason::AllocationFailure);
            ResetResources();
            return false;
        }
        if (!SelectionOutlineMaskSampleCountsAreSupported(
            nextIdentity->sampleCount,
            nextIdentity->depthSampleCount))
        {
            SetFallbackReason(SelectionOutlineFallbackReason::UnsupportedSampleCount);
            ResetResources();
            return false;
        }

        const bool needsResize =
            !m_resourceIdentity.has_value() ||
            !m_resourceIdentity->Matches(*nextIdentity) ||
            !m_intermediateFramebuffer.IsInitialized();
        if (needsResize)
        {
            const std::vector<NLS::Render::Buffers::MultiFramebuffer::AttachmentDesc> colorAttachments(1u);
            m_intermediateFramebuffer.Init(
                static_cast<uint16_t>(frameDescriptor.renderWidth),
                static_cast<uint16_t>(frameDescriptor.renderHeight),
                colorAttachments,
                false);
        }

        outResources.maskView = m_intermediateFramebuffer.GetOrCreateExplicitColorView(kSelectionOutlineMaskAttachmentIndex, "SelectionOutlineMask.Mask");
        if (outResources.maskView == nullptr)
        {
            SetFallbackReason(SelectionOutlineFallbackReason::AllocationFailure);
            return false;
        }
        m_resourceIdentity = nextIdentity;

        EnsureFullscreenResources();
        if (m_fullscreenQuad == nullptr)
        {
            SetFallbackReason(SelectionOutlineFallbackReason::AllocationFailure);
            return false;
        }

        SetFallbackReason(SelectionOutlineFallbackReason::None);
        return true;
    }

    void SelectionOutlineMaskRenderer::InvalidateSelectionOutlineTextureBindings()
    {
        m_compositeMaterial.InvalidateExplicitBindingSetCache();
    }

    bool SelectionOutlineMaskRenderer::BindScreenSpaceMaterialTextures(
        const NLS::Render::Data::FrameDescriptor& frameDescriptor,
        const PreparedResources& resources)
    {
        if (resources.maskView == nullptr)
        {
            SetFallbackReason(SelectionOutlineFallbackReason::AllocationFailure);
            return false;
        }

        const uint32_t width = frameDescriptor.renderWidth;
        const uint32_t height = frameDescriptor.renderHeight;
        bool textureHandleChanged = false;
        const bool wrappedTextures =
            WrapSelectionOutlineExternalTexture(m_maskTexture, resources.maskView, width, height, textureHandleChanged);
        if (!wrappedTextures)
        {
            SetFallbackReason(SelectionOutlineFallbackReason::AllocationFailure);
            return false;
        }
        if (m_maskTexture == nullptr)
        {
            SetFallbackReason(SelectionOutlineFallbackReason::AllocationFailure);
            return false;
        }

        if (textureHandleChanged)
            InvalidateSelectionOutlineTextureBindings();

        SetMaterialTextureIfChanged(m_compositeMaterial, "u_SelectionOutlineMask", m_maskTexture.get());
        SetFallbackReason(SelectionOutlineFallbackReason::None);
        return true;
    }

    void SelectionOutlineMaskRenderer::ResetResources()
    {
        BindSelectionOutlineTextureFallbacks(m_compositeMaterial, NLS::Editor::Rendering::GetEditorDefaultWhiteTexture());
        m_intermediateFramebuffer.Resize(0u, 0u);
        m_maskTexture.reset();
        m_resourceIdentity.reset();
        InvalidateCachedMask();
    }

    std::optional<SelectionOutlineMaskRenderer::MaskReuseSignature>
    SelectionOutlineMaskRenderer::BuildMaskReuseSignature(
        const NLS::Render::Data::FrameDescriptor& frameDescriptor,
        const std::vector<SelectionMaskCaptureGroup>& groups,
        const uint64_t selectedItemCount) const
    {
        if (frameDescriptor.camera == nullptr)
            return std::nullopt;

        const auto resourceIdentity = BuildResourceIdentity(frameDescriptor);
        if (!resourceIdentity.has_value())
            return std::nullopt;

        MaskReuseSignature signature;
        signature.resourceIdentity = *resourceIdentity;
        signature.selectedItemCount = selectedItemCount;
        signature.groupCount = static_cast<uint64_t>(groups.size());
        signature.camera = frameDescriptor.camera;
        signature.viewMatrix = frameDescriptor.camera->GetViewMatrix();
        signature.projectionMatrix = frameDescriptor.camera->GetProjectionMatrix();

        uint64_t selectedTreeHash = kSelectionOutlineSignatureHashSeed;
        uint64_t meshContentRevisionHash = kSelectionOutlineSignatureHashSeed;
        for (const auto& group : groups)
        {
            HashCombinePointer(selectedTreeHash, group.mesh);
            HashCombine(selectedTreeHash, group.mesh->GetContentRevision());
            HashCombine(meshContentRevisionHash, group.mesh->GetContentRevision());
            HashCombineFloat(selectedTreeHash, group.selectionGroupId);
            HashCombineFloat(selectedTreeHash, group.selectionClassification);
            HashCombine(selectedTreeHash, static_cast<uint64_t>(group.instanceModelMatrices.size()));
            signature.instanceCount += static_cast<uint64_t>(group.instanceModelMatrices.size());
            for (const auto& matrix : group.instanceModelMatrices)
                HashCombineMatrix(selectedTreeHash, matrix);
        }
        signature.selectedTreeHash = selectedTreeHash;
        signature.meshContentRevisionHash = meshContentRevisionHash;
        return signature;
    }

    std::optional<SelectionOutlineMaskRenderer::SelectionSourceSignature>
    SelectionOutlineMaskRenderer::BuildSelectionSourceSignature(
        const DebugGameObjectDebugDrawItems& debugDrawItems,
        const NLS::Render::Data::FrameDescriptor& frameDescriptor,
        NLS::Render::Resources::Mesh* cameraMesh) const
    {
        if (frameDescriptor.camera == nullptr)
            return std::nullopt;

        SelectionSourceSignature signature;
        signature.selectionMeshItemCount = static_cast<uint64_t>(debugDrawItems.selectionMeshItems.size());
        signature.cameraItemCount = static_cast<uint64_t>(debugDrawItems.cameras.size());
        signature.selectedItemCount = signature.selectionMeshItemCount + signature.cameraItemCount;

        uint64_t selectedTreeHash = kSelectionOutlineSignatureHashSeed;
        uint64_t meshContentRevisionHash = kSelectionOutlineSignatureHashSeed;
        for (const auto& selectionItem : debugDrawItems.selectionMeshItems)
        {
            HashCombinePointer(selectedTreeHash, selectionItem.meshRenderer);
            HashCombinePointer(selectedTreeHash, selectionItem.sourceMaterial);
            HashCombinePointer(selectedTreeHash, selectionItem.mesh);
            if (selectionItem.mesh != nullptr)
            {
                HashCombine(selectedTreeHash, selectionItem.mesh->GetContentRevision());
                HashCombine(meshContentRevisionHash, selectionItem.mesh->GetContentRevision());
            }
            HashCombineMatrix(selectedTreeHash, selectionItem.worldMatrix);
            HashCombineFloat(selectedTreeHash, selectionItem.selectionGroupId);
            HashCombineFloat(selectedTreeHash, selectionItem.selectionClassification);
        }

        if (cameraMesh != nullptr)
        {
            signature.hasCameraMesh = true;
            signature.cameraMeshContentRevision = cameraMesh->GetContentRevision();
            HashCombinePointer(selectedTreeHash, cameraMesh);
            HashCombine(selectedTreeHash, cameraMesh->GetContentRevision());
            HashCombine(meshContentRevisionHash, cameraMesh->GetContentRevision());
        }
        for (const auto& cameraItem : debugDrawItems.cameras)
        {
            HashCombinePointer(selectedTreeHash, cameraItem.cameraComponent);
            HashCombineVector3(selectedTreeHash, cameraItem.worldPosition);
            HashCombineQuaternion(selectedTreeHash, cameraItem.worldRotation);
            HashCombineFloat(selectedTreeHash, cameraItem.selectionGroupId);
            HashCombineFloat(selectedTreeHash, cameraItem.selectionClassification);
        }

        signature.selectedTreeHash = selectedTreeHash;
        signature.meshContentRevisionHash = meshContentRevisionHash;
        return signature;
    }

    SelectionOutlineMaskRenderer::PreparedSelectionCaptureGroups
    SelectionOutlineMaskRenderer::BuildPreparedSelectionCaptureGroups(
        const DebugGameObjectDebugDrawItems& debugDrawItems,
        NLS::Render::Resources::Mesh* cameraMesh) const
    {
        PreparedSelectionCaptureGroups prepared;
        prepared.groups = BuildSelectionMaskCaptureGroups(debugDrawItems, cameraMesh);
        for (const auto& group : prepared.groups)
            prepared.instanceCount += static_cast<uint64_t>(group.instanceModelMatrices.size());
        return prepared;
    }

    const SelectionOutlineMaskRenderer::PreparedSelectionCaptureGroups&
    SelectionOutlineMaskRenderer::ResolveSelectionCaptureGroups(
        const DebugGameObjectDebugDrawItems& debugDrawItems,
        const std::optional<SelectionSourceSignature>& sourceSignature,
        const NLS::Render::Data::FrameDescriptor& frameDescriptor,
        NLS::Render::Resources::Mesh* cameraMesh)
    {
        NLS_PROFILE_NAMED_SCOPE("SelectionOutlineMask::ResolveSelectionCaptureGroups");
        if (sourceSignature.has_value() &&
            m_cachedSelectionSourceSignature.has_value() &&
            m_cachedSelectionMaskReuseSignature.has_value() &&
            sourceSignature->Matches(*m_cachedSelectionSourceSignature))
        {
            auto maskReuseSignature = *m_cachedSelectionMaskReuseSignature;
            if (auto resourceIdentity = BuildResourceIdentity(frameDescriptor); resourceIdentity.has_value())
            {
                maskReuseSignature.resourceIdentity = *resourceIdentity;
                maskReuseSignature.camera = frameDescriptor.camera;
                if (frameDescriptor.camera != nullptr)
                {
                    maskReuseSignature.viewMatrix = frameDescriptor.camera->GetViewMatrix();
                    maskReuseSignature.projectionMatrix = frameDescriptor.camera->GetProjectionMatrix();
                }
                m_cachedSelectionCaptureGroups.maskReuseSignature = std::move(maskReuseSignature);
            }
            else
            {
                m_cachedSelectionCaptureGroups.maskReuseSignature.reset();
            }
            return m_cachedSelectionCaptureGroups;
        }

        m_cachedSelectionCaptureGroups = BuildPreparedSelectionCaptureGroups(debugDrawItems, cameraMesh);
        m_cachedSelectionSourceSignature = sourceSignature;
        m_cachedSelectionCaptureGroups.maskReuseSignature = BuildMaskReuseSignature(
            frameDescriptor,
            m_cachedSelectionCaptureGroups.groups,
            sourceSignature.has_value()
                ? sourceSignature->selectedItemCount
                : static_cast<uint64_t>(debugDrawItems.selectionMeshItems.size() + debugDrawItems.cameras.size()));
        m_cachedSelectionMaskReuseSignature = m_cachedSelectionCaptureGroups.maskReuseSignature;
        if (!sourceSignature.has_value())
            m_cachedSelectionSourceSignature.reset();
        return m_cachedSelectionCaptureGroups;
    }

    bool SelectionOutlineMaskRenderer::CanReuseCachedMask(
        const std::optional<MaskReuseSignature>& signature) const
    {
        return signature.has_value() &&
            m_cachedMaskSignature.has_value() &&
            signature->selectedItemCount > kSelectionOutlineDualDepthCaptureMaxItems &&
            GetLatestRetiredThreadedFrameId() >= m_cachedMaskRetirementTarget &&
            GetLatestFailedRetiredThreadedFrameId() < m_cachedMaskRetirementTarget &&
            signature->Matches(*m_cachedMaskSignature);
    }

    bool SelectionOutlineMaskRenderer::ShouldKeepCachedMaskRetirementTarget(
        const std::optional<MaskReuseSignature>& signature) const
    {
        return signature.has_value() &&
            m_cachedMaskSignature.has_value() &&
            m_cachedMaskRetirementTarget != 0u &&
            GetLatestRetiredThreadedFrameId() < m_cachedMaskRetirementTarget &&
            GetLatestFailedRetiredThreadedFrameId() < m_cachedMaskRetirementTarget &&
            signature->Matches(*m_cachedMaskSignature);
    }

    void SelectionOutlineMaskRenderer::MarkCachedMaskPending(
        const std::optional<MaskReuseSignature>& signature)
    {
        if (signature.has_value())
        {
            m_keepCachedMaskRetirementTargetForPending =
                ShouldKeepCachedMaskRetirementTarget(signature);
            m_pendingCachedMaskSignature = signature;
        }
        else
        {
            InvalidateCachedMask();
        }
    }

    void SelectionOutlineMaskRenderer::CommitPendingCachedMask(const uint64_t publishedFrameId)
    {
        const bool keepRetirementTarget = m_keepCachedMaskRetirementTargetForPending;
        m_keepCachedMaskRetirementTargetForPending = false;
        if (!m_pendingCachedMaskSignature.has_value() || publishedFrameId == 0u)
        {
            DiscardPendingCachedMask();
            return;
        }

        m_cachedMaskSignature = m_pendingCachedMaskSignature;
        if (!keepRetirementTarget)
            m_cachedMaskRetirementTarget = publishedFrameId;
        m_pendingCachedMaskSignature.reset();
    }

    void SelectionOutlineMaskRenderer::DiscardPendingCachedMask()
    {
        m_pendingCachedMaskSignature.reset();
        m_keepCachedMaskRetirementTargetForPending = false;
    }

    void SelectionOutlineMaskRenderer::InvalidateCachedMask()
    {
        m_cachedMaskSignature.reset();
        m_pendingCachedMaskSignature.reset();
        m_cachedSelectionSourceSignature.reset();
        m_cachedSelectionCaptureGroups = {};
        m_cachedSelectionMaskReuseSignature.reset();
        m_cachedMaskRetirementTarget = 0u;
        m_keepCachedMaskRetirementTargetForPending = false;
    }

    uint64_t SelectionOutlineMaskRenderer::GetLatestRetiredThreadedFrameId() const
    {
        const auto telemetry =
            NLS::Render::Context::DriverRendererAccess::TryGetThreadedFrameTelemetry(m_renderer.GetDriver());
        return telemetry.has_value() ? telemetry->latestRetiredFrameId : 0u;
    }

    uint64_t SelectionOutlineMaskRenderer::GetLatestFailedRetiredThreadedFrameId() const
    {
        const auto telemetry =
            NLS::Render::Context::DriverRendererAccess::TryGetThreadedFrameTelemetry(m_renderer.GetDriver());
        return telemetry.has_value() ? telemetry->latestFailedRetiredFrameId : 0u;
    }

    RenderPassCommandInput SelectionOutlineMaskRenderer::BuildPassInput(
        const SelectionOutlineMaskPassKind kind,
        const NLS::Render::Data::FrameDescriptor& frameDescriptor,
        const PreparedResources& resources,
        std::vector<NLS::Render::Context::RecordedDrawCommandInput>&& maskDrawCommands) const
    {
        RenderPassCommandInput passInput;
        if (!SelectionOutlineMaskPassKindIsValid(kind))
        {
            NLS_ASSERT(false, "Invalid selection outline mask pass kind.");
            return passInput;
        }

        ConfigureCommonPassInput(
            passInput,
            SelectionOutlineMaskPassName(kind),
            frameDescriptor);

        switch (kind)
        {
        case SelectionOutlineMaskPassKind::CaptureMask:
            passInput.requiresObjectData = true;
            passInput.usesDepthStencilAttachment = true;
            passInput.writesDepthStencilAttachment = false;
            passInput.depthStencilAttachmentView = frameDescriptor.outputDepthStencilView;
            passInput.colorAttachmentViews.push_back(resources.maskView);
            passInput.recordedDrawCommands = std::move(maskDrawCommands);
            AddTextureViewAccess(
                passInput,
                frameDescriptor.outputDepthStencilView,
                ResourceAccessMode::Read,
                ResourceState::DepthRead,
                PipelineStageMask::DepthStencil,
                AccessMask::DepthStencilRead);
            AddTextureViewAccess(
                passInput,
                resources.maskView,
                ResourceAccessMode::Write,
                ResourceState::RenderTarget,
                PipelineStageMask::RenderTarget,
                AccessMask::ColorAttachmentRead | AccessMask::ColorAttachmentWrite);
            break;
        case SelectionOutlineMaskPassKind::Composite:
            passInput.clearColor = false;
            passInput.colorAttachmentViews.push_back(frameDescriptor.outputColorView);
            AddTextureViewAccess(
                passInput,
                resources.maskView,
                ResourceAccessMode::Read,
                ResourceState::ShaderRead,
                PipelineStageMask::FragmentShader,
                AccessMask::ShaderRead);
            AddTextureViewAccess(
                passInput,
                frameDescriptor.outputColorView,
                ResourceAccessMode::Write,
                ResourceState::RenderTarget,
                PipelineStageMask::RenderTarget,
                AccessMask::ColorAttachmentRead | AccessMask::ColorAttachmentWrite);
            RecordFullscreenCommand(
                const_cast<NLS::Render::Resources::Material&>(m_compositeMaterial),
                frameDescriptor,
                passInput.recordedDrawCommands);
            break;
        case SelectionOutlineMaskPassKind::Count:
        default:
            NLS_ASSERT(false, "Invalid selection outline mask pass kind.");
            break;
        }

        passInput.drawCount = static_cast<uint64_t>(passInput.recordedDrawCommands.size());
        return passInput;
    }

    void SelectionOutlineMaskRenderer::SetFallbackReason(
        const SelectionOutlineFallbackReason reason)
    {
        m_lastFallbackReason = reason;
    }
}
