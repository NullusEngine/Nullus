#pragma once

#include <memory>
#include <optional>
#include <vector>

#include <fg/FrameGraph.hpp>

#include <Rendering/Context/ThreadedRenderingLifecycle.h>
#include <Rendering/Data/FrameDescriptor.h>
#include <Rendering/Data/LightingDescriptor.h>
#include <Rendering/FrameGraph/FrameGraphExecutionPlan.h>
#include <Rendering/Resources/Shader.h>
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

        const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& GetGraphicsPassBindingSet() const;
        NLS::Render::FrameGraph::PreparedComputeDispatchSource GetPreparedComputeDispatchSource() const;
        std::vector<NLS::Render::Context::RecordedComputeDispatchInput> GetPreparedComputeDispatchInputs() const;

    private:
        struct LightGridPassConstants
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
            LightGridPassConstants constants{};
            std::vector<uint32_t> packedLights;
            std::vector<uint32_t> clusterLightCounts;
            std::vector<uint32_t> clusterScratchIndices;
            std::vector<uint32_t> linkCounter;
            std::vector<uint32_t> compactCounter;
            std::vector<uint32_t> clusterRecords;
            std::vector<uint32_t> compactLightIndices;
        };

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
        NLS::Render::Resources::Shader* m_injectionShader = nullptr;
        NLS::Render::Resources::Shader* m_compactShader = nullptr;
        std::shared_ptr<NLS::Render::RHI::RHIPipelineLayout> m_injectionPipelineLayout;
        std::shared_ptr<NLS::Render::RHI::RHIPipelineLayout> m_compactPipelineLayout;
        std::shared_ptr<NLS::Render::RHI::RHIBindingLayout> m_injectionBindingLayout;
        std::shared_ptr<NLS::Render::RHI::RHIBindingLayout> m_compactBindingLayout;
        std::shared_ptr<NLS::Render::RHI::RHIBindingLayout> m_graphicsBindingLayout;
        std::shared_ptr<NLS::Render::RHI::RHIComputePipeline> m_injectionPipeline;
        std::shared_ptr<NLS::Render::RHI::RHIComputePipeline> m_compactPipeline;
        NLS::Render::RHI::PipelineCacheKey m_injectionPipelineKey;
        NLS::Render::RHI::PipelineCacheKey m_compactPipelineKey;
        std::shared_ptr<NLS::Render::RHI::RHIBindingSet> m_graphicsPassBindingSet;
        std::vector<NLS::Render::Context::RecordedComputeDispatchInput> m_computeDispatchInputs;
        mutable PackedFrameData m_frameScratch;
    };

    template<typename TMetadataRange, typename TMutatePackageFn, typename TBuildPassInputsFn>
    inline NLS::Render::FrameGraph::CompiledThreadedRenderSceneExecution CompileAndApplyPreparedLightGridThreadedExecution(
        NLS::Render::Context::RenderScenePackage& package,
        const LightGridPrepass::PreparedComputeRequest& preparedComputeRequest,
        TMutatePackageFn&& mutatePackageForPreparedCompute,
        const TMetadataRange& scenePassMetadataRange,
        TBuildPassInputsFn&& buildPassInputs)
    {
        return NLS::Render::FrameGraph::CompileAndApplyThreadedRenderSceneExecution(
            package,
            preparedComputeRequest.frameDescriptor,
            -1,
            -1,
            [&]()
            {
                return LightGridPrepass::BuildPreparedComputeDispatchSource(preparedComputeRequest);
            },
            std::forward<TMutatePackageFn>(mutatePackageForPreparedCompute),
            scenePassMetadataRange,
            std::forward<TBuildPassInputsFn>(buildPassInputs));
    }

    template<typename TMetadataRange, typename TMutatePackageFn>
    inline NLS::Render::FrameGraph::CompiledThreadedRenderSceneExecution CompileAndApplyPreparedLightGridThreadedExecution(
        NLS::Render::Context::RenderScenePackage& package,
        const LightGridPrepass::PreparedComputeRequest& preparedComputeRequest,
        TMutatePackageFn&& mutatePackageForPreparedCompute,
        const TMetadataRange& scenePassMetadataRange)
    {
        return NLS::Render::FrameGraph::CompileAndApplyThreadedRenderSceneExecution(
            package,
            preparedComputeRequest.frameDescriptor,
            -1,
            -1,
            [&]()
            {
                return LightGridPrepass::BuildPreparedComputeDispatchSource(preparedComputeRequest);
            },
            std::forward<TMutatePackageFn>(mutatePackageForPreparedCompute),
            scenePassMetadataRange);
    }
}
