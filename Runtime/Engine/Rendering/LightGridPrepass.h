#pragma once

#include <memory>
#include <optional>
#include <vector>

#include <fg/FrameGraph.hpp>

#include <Rendering/Context/ThreadedRenderingLifecycle.h>
#include <Rendering/Data/FrameDescriptor.h>
#include <Rendering/Data/LightingDescriptor.h>
#include <Rendering/FrameGraph/FrameGraphExecutionTypes.h>
#include <Rendering/Resources/Shader.h>
#include <Rendering/Resources/ShaderParameterStruct.h>
#include <Rendering/RHI/Utils/PipelineCache/PipelineCache.h>

#include "Rendering/ClusteredShading.h"
#include "EngineDef.h"

namespace NLS::Render::Context
{
    class Driver;
}

namespace NLS::Render::RHI
{
    class RHIBindingLayout;
    class RHIBindingSet;
    class RHIBuffer;
    class RHIComputePipeline;
    class RHIPipelineLayout;
}

namespace NLS::Render::Settings
{
    struct EngineDiagnosticsSettings;
}

namespace NLS::Engine::Rendering
{
    class NLS_ENGINE_API LightGridPrepass
    {
    public:
        struct CapturedLight
        {
            NLS::Maths::Vector3 position{ 0.0f, 0.0f, 0.0f };
            NLS::Maths::Vector3 forward{ 0.0f, 0.0f, -1.0f };
            NLS::Maths::Vector3 color{ 1.0f, 1.0f, 1.0f };
            float effectRange = 0.0f;
            float intensity = 1.0f;
            float constant = 0.0f;
            float linear = 0.0f;
            float quadratic = 1.0f;
            float outerCutoff = 15.0f;
            NLS::Render::Settings::ELightType type = NLS::Render::Settings::ELightType::POINT;
        };

        struct PreparedFrameInputs
        {
            std::vector<CapturedLight> lights;
            bool hasSkyboxTexture = false;
        };

        struct PreparedComputeRequest
        {
            NLS::Render::Data::FrameDescriptor frameDescriptor{};
            std::shared_ptr<LightGridPrepass> lightGridPrepass;
            std::optional<PreparedFrameInputs> preparedFrameInputs;
        };

        explicit LightGridPrepass(NLS::Render::Context::Driver& driver);
        ~LightGridPrepass();

        static PreparedFrameInputs CaptureFrameInputs(
            const NLS::Render::Data::LightingDescriptor& lightingDescriptor,
            bool hasSkyboxTexture);

        bool Prepare(
            const NLS::Render::Data::FrameDescriptor& frameDescriptor,
            const NLS::Render::Data::LightingDescriptor& lightingDescriptor,
            bool hasSkyboxTexture = false);
        bool Prepare(
            const NLS::Render::Data::FrameDescriptor& frameDescriptor,
            const PreparedFrameInputs& preparedFrameInputs);
        static PreparedComputeRequest BuildPreparedComputeRequest(
            const NLS::Render::Data::FrameDescriptor& frameDescriptor,
            const std::shared_ptr<LightGridPrepass>& lightGridPrepass,
            const std::optional<PreparedFrameInputs>& preparedFrameInputs = {});
        static NLS::Render::FrameGraph::PreparedComputeDispatchSource BuildPreparedComputeDispatchSource(
            const PreparedComputeRequest& preparedComputeRequest);
        static bool ShouldLogHotPathFailureDiagnostics(
            const NLS::Render::Settings::EngineDiagnosticsSettings& diagnostics);

        bool EnsureFallbackGraphicsPassBindingSet(
            const NLS::Render::Data::FrameDescriptor& frameDescriptor,
            bool hasSkyboxTexture);
        const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& GetGraphicsPassBindingSet() const;
        NLS::Render::FrameGraph::PreparedComputeDispatchSource GetPreparedComputeDispatchSource() const;
        std::vector<NLS::Render::Context::RecordedComputeDispatchInput> GetPreparedComputeDispatchInputs() const;

    private:
        struct ForwardLightData
        {
            NLS::Maths::Matrix4 viewMatrix;
            NLS::Maths::Matrix4 projectionMatrix;
            NLS::Maths::Matrix4 inverseViewProjection;
            NLS::Maths::Matrix4 clipToView;
            NLS::Maths::Vector4 cameraWorldPositionNearPlane;
            NLS::Maths::Vector4 renderSizeFarPlane;
            NLS::Maths::Vector4 gridParams;
            NLS::Maths::Vector4 lightingParams;
            NLS::Maths::Vector4 zParams;
            NLS::Maths::Vector4 pixelParams;
        };

        struct PackedFrameData
        {
            ForwardLightData forwardLightData{};
            std::vector<uint32_t> forwardLocalLightData;
            std::vector<uint32_t> startOffsetGrid;
            std::vector<uint32_t> culledLightLinks;
            std::vector<uint32_t> linkCounter;
            std::vector<uint32_t> compactCounter;
            std::vector<uint32_t> numCulledLightsGrid;
            std::vector<uint32_t> culledLightDataGrid;
        };

        struct PreparedResourceCacheKey
        {
            uint32_t renderWidth = 0u;
            uint32_t renderHeight = 0u;
            float nearPlane = 0.0f;
            float farPlane = 0.0f;
            ClusteredShadingSettings settings{};
            bool hasSkyboxTexture = false;
            std::vector<CapturedLight> lights;
        };

        struct PreparedResourceCache
        {
            bool valid = false;
            PreparedResourceCacheKey key{};
            ForwardLightData forwardLightData{};
            std::shared_ptr<NLS::Render::RHI::RHIBuffer> forwardLightDataBuffer;
            std::shared_ptr<NLS::Render::RHI::RHIBindingSet> graphicsPassBindingSet;
            std::vector<NLS::Render::Context::RecordedComputeDispatchInput> computeDispatchInputs;
        };

        struct PreparedBufferCache
        {
            std::shared_ptr<NLS::Render::RHI::RHIBuffer> startOffsetGrid;
            size_t startOffsetGridSize = 0u;
            std::shared_ptr<NLS::Render::RHI::RHIBuffer> culledLightLinks;
            size_t culledLightLinksSize = 0u;
            std::shared_ptr<NLS::Render::RHI::RHIBuffer> linkCounter;
            size_t linkCounterSize = 0u;
            std::shared_ptr<NLS::Render::RHI::RHIBuffer> compactCounter;
            size_t compactCounterSize = 0u;
            std::shared_ptr<NLS::Render::RHI::RHIBuffer> numCulledLightsGrid;
            size_t numCulledLightsGridSize = 0u;
            std::shared_ptr<NLS::Render::RHI::RHIBuffer> culledLightDataGrid;
            size_t culledLightDataGridSize = 0u;
        };

        struct LightGridResetParameters
        {
            static NLS::Render::Resources::ShaderParameterStruct Build();
            std::shared_ptr<NLS::Render::RHI::RHIBuffer> Forward;
            std::shared_ptr<NLS::Render::RHI::RHIBuffer> RWStartOffsetGrid;
            std::shared_ptr<NLS::Render::RHI::RHIBuffer> RWCulledLightLinks;
            std::shared_ptr<NLS::Render::RHI::RHIBuffer> RWLinkCounter;
            std::shared_ptr<NLS::Render::RHI::RHIBuffer> RWCompactCounter;
            std::shared_ptr<NLS::Render::RHI::RHIBuffer> RWNumCulledLightsGrid;
            std::shared_ptr<NLS::Render::RHI::RHIBuffer> RWCulledLightDataGrid;
        };

        struct LightGridInjectionParameters
        {
            static NLS::Render::Resources::ShaderParameterStruct Build();
            std::shared_ptr<NLS::Render::RHI::RHIBuffer> Forward;
            std::shared_ptr<NLS::Render::RHI::RHIBuffer> ForwardLocalLightBuffer;
            std::shared_ptr<NLS::Render::RHI::RHIBuffer> RWStartOffsetGrid;
            std::shared_ptr<NLS::Render::RHI::RHIBuffer> RWCulledLightLinks;
            std::shared_ptr<NLS::Render::RHI::RHIBuffer> RWLinkCounter;
        };

        struct LightGridCompactParameters
        {
            static NLS::Render::Resources::ShaderParameterStruct Build();
            std::shared_ptr<NLS::Render::RHI::RHIBuffer> Forward;
            std::shared_ptr<NLS::Render::RHI::RHIBuffer> StartOffsetGrid;
            std::shared_ptr<NLS::Render::RHI::RHIBuffer> CulledLightLinks;
            std::shared_ptr<NLS::Render::RHI::RHIBuffer> RWCompactCounter;
            std::shared_ptr<NLS::Render::RHI::RHIBuffer> RWNumCulledLightsGrid;
            std::shared_ptr<NLS::Render::RHI::RHIBuffer> RWCulledLightDataGrid;
        };

        struct LightGridGraphicsParameters
        {
            static NLS::Render::Resources::ShaderParameterStruct Build();
            std::shared_ptr<NLS::Render::RHI::RHIBuffer> Forward;
            std::shared_ptr<NLS::Render::RHI::RHIBuffer> ForwardLocalLightBuffer;
            std::shared_ptr<NLS::Render::RHI::RHIBuffer> NumCulledLightsGrid;
            std::shared_ptr<NLS::Render::RHI::RHIBuffer> CulledLightDataGrid;
        };

        std::optional<PreparedResourceCacheKey> BuildPreparedResourceCacheKey(
            const NLS::Render::Data::FrameDescriptor& frameDescriptor,
            const PreparedFrameInputs& preparedFrameInputs) const;
        bool TryReusePreparedResources(
            const PreparedResourceCacheKey& key,
            const ForwardLightData& forwardLightData);
        void StorePreparedResourceCache(const PreparedResourceCacheKey& key);
        static bool AreSamePreparedResourceCacheKeys(
            const PreparedResourceCacheKey& lhs,
            const PreparedResourceCacheKey& rhs);
        static bool AreSameForwardLightData(
            const ForwardLightData& lhs,
            const ForwardLightData& rhs);
        bool EnsureGraphicsBindingLayout();

        bool EnsureShadersLoaded();
        bool EnsurePipelines();
        bool BuildFrameData(
            const NLS::Render::Data::FrameDescriptor& frameDescriptor,
            const NLS::Render::Data::LightingDescriptor& lightingDescriptor,
            bool hasSkyboxTexture,
            PackedFrameData& outFrameData) const;
        bool BuildFrameData(
            const NLS::Render::Data::FrameDescriptor& frameDescriptor,
            const PreparedFrameInputs& preparedFrameInputs,
            bool hasSkyboxTexture,
            PackedFrameData& outFrameData) const;

    private:
        NLS::Render::Context::Driver& m_driver;
        ClusteredShadingSettings m_settings;
        NLS::Render::Resources::Shader* m_resetShader = nullptr;
        NLS::Render::Resources::Shader* m_injectionShader = nullptr;
        NLS::Render::Resources::Shader* m_compactShader = nullptr;
        NLS::Render::Resources::GlobalShader m_resetGlobalShader;
        NLS::Render::Resources::GlobalShader m_injectionGlobalShader;
        NLS::Render::Resources::GlobalShader m_compactGlobalShader;
        std::shared_ptr<NLS::Render::RHI::RHIPipelineLayout> m_resetPipelineLayout;
        std::shared_ptr<NLS::Render::RHI::RHIPipelineLayout> m_injectionPipelineLayout;
        std::shared_ptr<NLS::Render::RHI::RHIPipelineLayout> m_compactPipelineLayout;
        std::shared_ptr<NLS::Render::RHI::RHIBindingLayout> m_resetBindingLayout;
        std::shared_ptr<NLS::Render::RHI::RHIBindingLayout> m_injectionBindingLayout;
        std::shared_ptr<NLS::Render::RHI::RHIBindingLayout> m_compactBindingLayout;
        std::shared_ptr<NLS::Render::RHI::RHIBindingLayout> m_graphicsBindingLayout;
        std::shared_ptr<NLS::Render::RHI::RHIComputePipeline> m_resetPipeline;
        std::shared_ptr<NLS::Render::RHI::RHIComputePipeline> m_injectionPipeline;
        std::shared_ptr<NLS::Render::RHI::RHIComputePipeline> m_compactPipeline;
        NLS::Render::RHI::PipelineCacheKey m_resetPipelineKey;
        NLS::Render::RHI::PipelineCacheKey m_injectionPipelineKey;
        NLS::Render::RHI::PipelineCacheKey m_compactPipelineKey;
        std::shared_ptr<NLS::Render::RHI::RHIBindingSet> m_graphicsPassBindingSet;
        std::shared_ptr<NLS::Render::RHI::RHIBindingSet> m_fallbackGraphicsPassBindingSet;
        std::vector<NLS::Render::Context::RecordedComputeDispatchInput> m_computeDispatchInputs;
        mutable PackedFrameData m_frameScratch;
        PreparedResourceCache m_preparedResourceCache;
        PreparedBufferCache m_preparedBufferCache;
    };

}
