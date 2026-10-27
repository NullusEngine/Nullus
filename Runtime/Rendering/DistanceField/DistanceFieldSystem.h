#pragma once

#include <memory>
#include <string>
#include "Rendering/Context/RenderScenePackage.h"
#include "Rendering/DistanceField/DistanceFieldSceneData.h"

namespace NLS::Render::DistanceField
{
    enum class DistanceFieldVisualization : uint32_t { Off, Mesh, Global };
    enum class DistanceFieldDebugChannel : uint32_t { Surface, Steps, SignedSlice, Normals, Clipmaps, Pages, Residency };
    struct DistanceFieldDebugSettings
    {
        DistanceFieldVisualization visualization = DistanceFieldVisualization::Off;
        DistanceFieldDebugChannel channel = DistanceFieldDebugChannel::Surface;
        float slicePosition = 0;
        bool forceRebuild = false;
        bool mostlyStaticOnly = false;
    };
    struct DistanceFieldStats
    {
        uint64_t objects = 0, residentBricks = 0, meshBytes = 0, pageBytes = 0, metadataBytes = 0;
        uint64_t uploadedBytes = 0, dirtyPages = 0, residentPages = 0, allocationFailures = 0, allocationRetries = 0;
        uint64_t pendingLoads = 0, failedLoads = 0, revision = 0;
        double updateCpuMs = 0;
        std::optional<double> gpuUpdateMs;
        std::string diagnostic;
    };
    struct DistanceFieldFrameInput
    {
        uint64_t viewId = 0, frameSerial = 0;
        std::shared_ptr<const void> viewLifetime;
        std::vector<DistanceFieldObjectInput> objects;
        Maths::Vector3 cameraPosition;
        Maths::Matrix4 inverseViewProjection = Maths::Matrix4::Identity;
        std::shared_ptr<RHI::RHITexture> outputTexture;
        uint32_t width = 0, height = 0;
        float traceDistance = 2048;
        DistanceFieldDebugSettings debug;
        std::vector<Maths::Vector4> probePositions;
    };
    struct DistanceFieldGpuResources
    {
        std::shared_ptr<RHI::RHIBuffer> objects, meshIndirection, pageTable, samplingConstants;
        std::shared_ptr<RHI::RHITextureView> meshAtlas, globalAtlas;
        std::shared_ptr<RHI::RHISampler> sampler;
        uint32_t objectCount = 0;
        uint64_t revision = 0;
        std::shared_ptr<const void> lifetime;
    };
    struct DistanceFieldFrameResources
    {
        std::vector<Context::RenderPassCommandInput> passes;
        std::shared_ptr<RHI::RHIBuffer> probeResults;
        std::shared_ptr<const DistanceFieldGpuResources> gpuResources;
        std::optional<Context::PostSubmitBufferReadbackRequest> timingReadback;
        DistanceFieldStats stats;
    };
    class NLS_RENDER_API DistanceFieldSystem
    {
    public:
        static std::shared_ptr<DistanceFieldSystem> Get(const std::shared_ptr<RHI::RHIDevice>& device);
        explicit DistanceFieldSystem(std::shared_ptr<RHI::RHIDevice> device);
        ~DistanceFieldSystem();
        DistanceFieldFrameResources BuildFrame(DistanceFieldFrameInput input);
        [[nodiscard]] DistanceFieldStats GetStats() const;
        static uint64_t RuntimeRevision();
        static bool HasPendingWork();
        static uint64_t AllocateViewId();
        static std::shared_ptr<const uint64_t> CreateViewLifetime();
    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
