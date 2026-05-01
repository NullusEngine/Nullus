#include "Rendering/LightGridPrepass.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <string_view>

#include <Core/ResourceManagement/ShaderManager.h>
#include <Core/ServiceLocator.h>
#include <Debug/Logger.h>
#include <Math/Matrix4.h>
#include <Math/Vector3.h>
#include <Math/Vector4.h>
#include <Rendering/Context/DriverAccess.h>
#include <Rendering/RHI/BindingPointMap.h>
#include <Rendering/RHI/Core/RHIDevice.h>
#include <Rendering/RHI/Utils/PipelineCache/PipelineCache.h>

namespace
{
    struct LightGridPassConstants
    {
        NLS::Maths::Matrix4 viewMatrix;
        NLS::Maths::Matrix4 projectionMatrix;
        NLS::Maths::Matrix4 inverseViewProjection;
        NLS::Maths::Vector4 cameraWorldPositionNearPlane;
        NLS::Maths::Vector4 renderSizeFarPlane;
        NLS::Maths::Vector4 gridParams;
        NLS::Maths::Vector4 lightingParams;
    };

    constexpr uint32_t kLightWordStride = 16u;
    constexpr uint32_t kRecordWordStride = 2u;
    constexpr float kDefaultAmbientFloor = 0.05f;

    template<typename TValue>
    void AppendValueBytes(std::vector<uint8_t>& bytes, const TValue& value)
    {
        const auto* begin = reinterpret_cast<const uint8_t*>(&value);
        bytes.insert(bytes.end(), begin, begin + sizeof(TValue));
    }

    uint32_t PackFloat(float value)
    {
        uint32_t bits = 0u;
        std::memcpy(&bits, &value, sizeof(bits));
        return bits;
    }

    void PackLight(
        const NLS::Render::Entities::Light& light,
        std::vector<uint32_t>& packedWords)
    {
        packedWords.push_back(PackFloat(light.transform->GetWorldPosition().x));
        packedWords.push_back(PackFloat(light.transform->GetWorldPosition().y));
        packedWords.push_back(PackFloat(light.transform->GetWorldPosition().z));
        packedWords.push_back(PackFloat(light.GetEffectRange()));

        packedWords.push_back(PackFloat(light.transform->GetWorldForward().x));
        packedWords.push_back(PackFloat(light.transform->GetWorldForward().y));
        packedWords.push_back(PackFloat(light.transform->GetWorldForward().z));
        packedWords.push_back(static_cast<uint32_t>(light.type));

        packedWords.push_back(PackFloat(light.color.x));
        packedWords.push_back(PackFloat(light.color.y));
        packedWords.push_back(PackFloat(light.color.z));
        packedWords.push_back(PackFloat(light.intensity));

        packedWords.push_back(PackFloat(light.constant));
        packedWords.push_back(PackFloat(light.linear));
        packedWords.push_back(PackFloat(light.quadratic));
        packedWords.push_back(PackFloat(light.outerCutoff));
    }

    void PackCapturedLight(
        const NLS::Engine::Rendering::LightGridPrepass::CapturedLight& light,
        std::vector<uint32_t>& packedWords)
    {
        packedWords.push_back(PackFloat(light.position.x));
        packedWords.push_back(PackFloat(light.position.y));
        packedWords.push_back(PackFloat(light.position.z));
        packedWords.push_back(PackFloat(light.effectRange));

        packedWords.push_back(PackFloat(light.forward.x));
        packedWords.push_back(PackFloat(light.forward.y));
        packedWords.push_back(PackFloat(light.forward.z));
        packedWords.push_back(static_cast<uint32_t>(light.type));

        packedWords.push_back(PackFloat(light.color.x));
        packedWords.push_back(PackFloat(light.color.y));
        packedWords.push_back(PackFloat(light.color.z));
        packedWords.push_back(PackFloat(light.intensity));

        packedWords.push_back(PackFloat(light.constant));
        packedWords.push_back(PackFloat(light.linear));
        packedWords.push_back(PackFloat(light.quadratic));
        packedWords.push_back(PackFloat(light.outerCutoff));
    }

    uint64_t HashComputePipelineKey(
        std::string_view label,
        const std::shared_ptr<NLS::Render::RHI::RHIPipelineLayout>& layout,
        const std::shared_ptr<NLS::Render::RHI::RHIShaderModule>& shaderModule)
    {
        uint64_t hash = static_cast<uint64_t>(std::hash<std::string_view>{}(label));
        if (layout != nullptr)
        {
            hash ^= static_cast<uint64_t>(std::hash<std::string>{}(layout->GetDesc().debugName)) + 0x9e3779b97f4a7c15ull;
        }
        if (shaderModule != nullptr)
        {
            hash ^= static_cast<uint64_t>(std::hash<std::string>{}(shaderModule->GetDesc().debugName)) + 0x517cc1b727220a95ull;
            hash ^= static_cast<uint64_t>(std::hash<std::string>{}(shaderModule->GetDesc().entryPoint)) + 0x94d049bb133111ebull;
            hash ^= static_cast<uint64_t>(static_cast<uint32_t>(shaderModule->GetDesc().targetBackend)) << 32u;
        }
        return hash;
    }

    NLS::Render::RHI::PipelineCacheKey BuildComputePipelineCacheKey(
        std::string_view label,
        const std::shared_ptr<NLS::Render::RHI::RHIPipelineLayout>& layout,
        const std::shared_ptr<NLS::Render::RHI::RHIShaderModule>& shaderModule)
    {
        NLS::Render::RHI::PipelineCacheKey key;
        key.hash = HashComputePipelineKey(label, layout, shaderModule);
        key.backend =
            shaderModule != nullptr
                ? shaderModule->GetDesc().targetBackend
                : NLS::Render::RHI::NativeBackendType::None;
        key.stableDebugName = std::string(label);
        return key;
    }

}

namespace NLS::Engine::Rendering
{
    struct LightGridPrepass::PackedFrameData
    {
        LightGridPassConstants constants{};
        std::vector<uint32_t> packedLights;
        std::vector<uint32_t> clusterLightCounts;
        std::vector<uint32_t> clusterScratchIndices;
        std::vector<uint32_t> compactCounter;
        std::vector<uint32_t> clusterRecords;
        std::vector<uint32_t> compactLightIndices;
    };

    LightGridPrepass::LightGridPrepass(NLS::Render::Context::Driver& driver)
        : m_driver(driver)
    {
    }

    LightGridPrepass::~LightGridPrepass() = default;

    LightGridPrepass::PreparedFrameInputs LightGridPrepass::CaptureFrameInputs(
        const NLS::Render::Data::LightingDescriptor& lightingDescriptor,
        const bool hasSkyboxTexture)
    {
        PreparedFrameInputs preparedInputs;
        preparedInputs.hasSkyboxTexture = hasSkyboxTexture;
        preparedInputs.lights.reserve(lightingDescriptor.lights.size());

        for (const auto& lightRef : lightingDescriptor.lights)
        {
            const auto& light = lightRef.get();
            CapturedLight capturedLight;
            capturedLight.position = light.transform->GetWorldPosition();
            capturedLight.forward = light.transform->GetWorldForward();
            capturedLight.color = light.color;
            capturedLight.effectRange = light.GetEffectRange();
            capturedLight.intensity = light.intensity;
            capturedLight.constant = light.constant;
            capturedLight.linear = light.linear;
            capturedLight.quadratic = light.quadratic;
            capturedLight.outerCutoff = light.outerCutoff;
            capturedLight.type = light.type;
            preparedInputs.lights.push_back(capturedLight);
        }

        return preparedInputs;
    }

    bool LightGridPrepass::Prepare(
        const NLS::Render::Data::FrameDescriptor& frameDescriptor,
        const NLS::Render::Data::LightingDescriptor& lightingDescriptor,
        const bool hasSkyboxTexture)
    {
        return Prepare(
            frameDescriptor,
            CaptureFrameInputs(lightingDescriptor, hasSkyboxTexture));
    }

    bool LightGridPrepass::Prepare(
        const NLS::Render::Data::FrameDescriptor& frameDescriptor,
        const PreparedFrameInputs& preparedFrameInputs)
    {
        m_computeDispatchInputs.clear();
        m_graphicsPassBindingSet.reset();

        auto device = NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(m_driver);
        if (device == nullptr)
        {
            NLS_LOG_ERROR("LightGridPrepass::Prepare failed: explicit RHI device is unavailable.");
            return false;
        }

        if (!EnsureShadersLoaded())
        {
            NLS_LOG_ERROR("LightGridPrepass::Prepare failed: clustered lighting shaders are not loaded.");
            return false;
        }

        if (!EnsurePipelines())
        {
            NLS_LOG_ERROR("LightGridPrepass::Prepare failed: clustered lighting compute pipelines are unavailable.");
            return false;
        }

        PackedFrameData frameData;
        if (!BuildFrameData(frameDescriptor, preparedFrameInputs, preparedFrameInputs.hasSkyboxTexture, frameData))
        {
            NLS_LOG_ERROR("LightGridPrepass::Prepare failed: frame data could not be built.");
            return false;
        }

        NLS::Render::RHI::RHIBufferDesc constantsDesc;
        constantsDesc.size = sizeof(LightGridPassConstants);
        constantsDesc.usage = NLS::Render::RHI::BufferUsageFlags::Uniform;
        constantsDesc.memoryUsage = NLS::Render::RHI::MemoryUsage::CPUToGPU;
        constantsDesc.debugName = "LightGridPassConstants";
        auto constantsBuffer = device->CreateBuffer(constantsDesc, &frameData.constants);

        auto createStorageBuffer = [&](std::string_view debugName, const std::vector<uint32_t>& data)
        {
            NLS::Render::RHI::RHIBufferDesc desc;
            desc.size = std::max<size_t>(sizeof(uint32_t), data.size() * sizeof(uint32_t));
            desc.usage = NLS::Render::RHI::BufferUsageFlags::Storage;
            desc.memoryUsage = NLS::Render::RHI::MemoryUsage::GPUOnly;
            desc.debugName = std::string(debugName);
            return device->CreateBuffer(desc, data.empty() ? nullptr : data.data());
        };

        auto packedLightsBuffer = createStorageBuffer("LightGridPackedLights", frameData.packedLights);
        auto clusterCountsBuffer = createStorageBuffer("LightGridClusterCounts", frameData.clusterLightCounts);
        auto clusterScratchBuffer = createStorageBuffer("LightGridClusterScratchIndices", frameData.clusterScratchIndices);
        auto compactCounterBuffer = createStorageBuffer("LightGridCompactCounter", frameData.compactCounter);
        auto clusterRecordsBuffer = createStorageBuffer("LightGridClusterRecords", frameData.clusterRecords);
        auto compactLightIndicesBuffer = createStorageBuffer("LightGridCompactLightIndices", frameData.compactLightIndices);

        if (constantsBuffer == nullptr ||
            packedLightsBuffer == nullptr ||
            clusterCountsBuffer == nullptr ||
            clusterScratchBuffer == nullptr ||
            compactCounterBuffer == nullptr ||
            clusterRecordsBuffer == nullptr ||
            compactLightIndicesBuffer == nullptr)
        {
            NLS_LOG_ERROR("LightGridPrepass::Prepare failed: one or more clustered lighting buffers could not be created.");
            return false;
        }

        const auto descriptorLifetime = NLS::Render::Context::DriverRendererAccess::IsThreadedRenderingEnabled(m_driver)
            ? NLS::Render::RHI::DescriptorAllocationLifetime::Persistent
            : NLS::Render::RHI::DescriptorAllocationLifetime::TransientFrame;

        NLS::Render::RHI::RHIBindingSetDesc injectionSetDesc;
        injectionSetDesc.layout = m_injectionBindingLayout;
        injectionSetDesc.debugName = "LightGridInjectionBindingSet";
        injectionSetDesc.entries = {
            { 0u, NLS::Render::RHI::BindingType::UniformBuffer, constantsBuffer, 0u, sizeof(LightGridPassConstants), nullptr, nullptr },
            { 0u, NLS::Render::RHI::BindingType::StructuredBuffer, packedLightsBuffer, 0u, frameData.packedLights.size() * sizeof(uint32_t), nullptr, nullptr },
            { 1u, NLS::Render::RHI::BindingType::StorageBuffer, clusterCountsBuffer, 0u, frameData.clusterLightCounts.size() * sizeof(uint32_t), nullptr, nullptr },
            { 2u, NLS::Render::RHI::BindingType::StorageBuffer, clusterScratchBuffer, 0u, frameData.clusterScratchIndices.size() * sizeof(uint32_t), nullptr, nullptr },
            { 3u, NLS::Render::RHI::BindingType::StorageBuffer, compactCounterBuffer, 0u, frameData.compactCounter.size() * sizeof(uint32_t), nullptr, nullptr },
            { 4u, NLS::Render::RHI::BindingType::StorageBuffer, clusterRecordsBuffer, 0u, frameData.clusterRecords.size() * sizeof(uint32_t), nullptr, nullptr },
            { 5u, NLS::Render::RHI::BindingType::StorageBuffer, compactLightIndicesBuffer, 0u, frameData.compactLightIndices.size() * sizeof(uint32_t), nullptr, nullptr }
        };
        auto injectionBindingSet = NLS::Render::Context::DriverRendererAccess::CreateExplicitBindingSet(
            m_driver,
            injectionSetDesc,
            descriptorLifetime);

        NLS::Render::RHI::RHIBindingSetDesc compactSetDesc;
        compactSetDesc.layout = m_compactBindingLayout;
        compactSetDesc.debugName = "LightGridCompactBindingSet";
        compactSetDesc.entries = {
            { 0u, NLS::Render::RHI::BindingType::UniformBuffer, constantsBuffer, 0u, sizeof(LightGridPassConstants), nullptr, nullptr },
            { 1u, NLS::Render::RHI::BindingType::StructuredBuffer, clusterCountsBuffer, 0u, frameData.clusterLightCounts.size() * sizeof(uint32_t), nullptr, nullptr },
            { 2u, NLS::Render::RHI::BindingType::StructuredBuffer, clusterScratchBuffer, 0u, frameData.clusterScratchIndices.size() * sizeof(uint32_t), nullptr, nullptr },
            { 3u, NLS::Render::RHI::BindingType::StorageBuffer, compactCounterBuffer, 0u, frameData.compactCounter.size() * sizeof(uint32_t), nullptr, nullptr },
            { 4u, NLS::Render::RHI::BindingType::StorageBuffer, clusterRecordsBuffer, 0u, frameData.clusterRecords.size() * sizeof(uint32_t), nullptr, nullptr },
            { 5u, NLS::Render::RHI::BindingType::StorageBuffer, compactLightIndicesBuffer, 0u, frameData.compactLightIndices.size() * sizeof(uint32_t), nullptr, nullptr }
        };
        auto compactBindingSet = NLS::Render::Context::DriverRendererAccess::CreateExplicitBindingSet(
            m_driver,
            compactSetDesc,
            descriptorLifetime);

        NLS::Render::RHI::RHIBindingSetDesc graphicsSetDesc;
        graphicsSetDesc.layout = m_graphicsBindingLayout;
        graphicsSetDesc.debugName = "LightGridGraphicsBindingSet";
        graphicsSetDesc.entries = {
            { 0u, NLS::Render::RHI::BindingType::UniformBuffer, constantsBuffer, 0u, sizeof(LightGridPassConstants), nullptr, nullptr },
            { 0u, NLS::Render::RHI::BindingType::StructuredBuffer, packedLightsBuffer, 0u, frameData.packedLights.size() * sizeof(uint32_t), nullptr, nullptr },
            { 1u, NLS::Render::RHI::BindingType::StructuredBuffer, clusterRecordsBuffer, 0u, frameData.clusterRecords.size() * sizeof(uint32_t), nullptr, nullptr },
            { 2u, NLS::Render::RHI::BindingType::StructuredBuffer, compactLightIndicesBuffer, 0u, frameData.compactLightIndices.size() * sizeof(uint32_t), nullptr, nullptr }
        };
        m_graphicsPassBindingSet = NLS::Render::Context::DriverRendererAccess::CreateExplicitBindingSet(
            m_driver,
            graphicsSetDesc,
            descriptorLifetime);

        if (injectionBindingSet == nullptr || compactBindingSet == nullptr || m_graphicsPassBindingSet == nullptr)
        {
            NLS_LOG_ERROR("LightGridPrepass::Prepare failed: one or more clustered lighting binding sets could not be created.");
            return false;
        }

        uint32_t lightDispatchCount = static_cast<uint32_t>((preparedFrameInputs.lights.size() + 63u) / 64u);
        if (lightDispatchCount == 0u)
            lightDispatchCount = 1u;
        const uint32_t clusterCount =
            m_settings.gridSizeX * m_settings.gridSizeY * m_settings.gridSizeZ;
        const uint32_t clusterDispatchCount = (clusterCount + 63u) / 64u;

        m_computeDispatchInputs = {
            {
                "LightGridInjection",
                m_injectionPipeline,
                { { NLS::Render::RHI::BindingPointMap::kPassDescriptorSet, injectionBindingSet } },
                lightDispatchCount,
                1u,
                1u,
                {},
                { clusterCountsBuffer, clusterScratchBuffer },
                {}
            },
            {
                "LightGridCompact",
                m_compactPipeline,
                { { NLS::Render::RHI::BindingPointMap::kPassDescriptorSet, compactBindingSet } },
                clusterDispatchCount,
                1u,
                1u,
                { clusterCountsBuffer, clusterScratchBuffer },
                { compactCounterBuffer },
                { clusterRecordsBuffer, compactLightIndicesBuffer }
            }
        };

        return true;
    }

    LightGridPrepass::PreparedComputeRequest LightGridPrepass::BuildPreparedComputeRequest(
        const NLS::Render::Data::FrameDescriptor& frameDescriptor,
        const std::shared_ptr<LightGridPrepass>& lightGridPrepass,
        const std::optional<PreparedFrameInputs>& preparedFrameInputs)
    {
        PreparedComputeRequest request;
        request.frameDescriptor = frameDescriptor;
        request.lightGridPrepass = lightGridPrepass;
        request.preparedFrameInputs = preparedFrameInputs;
        return request;
    }

    NLS::Render::FrameGraph::PreparedComputeDispatchSource LightGridPrepass::BuildPreparedComputeDispatchSource(
        const PreparedComputeRequest& preparedComputeRequest)
    {
        const auto& lightGridPrepass = preparedComputeRequest.lightGridPrepass;
        const auto& preparedFrameInputs = preparedComputeRequest.preparedFrameInputs;
        if (lightGridPrepass == nullptr)
        {
            NLS_LOG_ERROR("LightGridPrepass::BuildPreparedComputeDispatchSource failed: LightGridPrepass instance is null.");
            return {};
        }

        if (!preparedFrameInputs.has_value())
        {
            NLS_LOG_ERROR("LightGridPrepass::BuildPreparedComputeDispatchSource failed: prepared frame inputs are missing.");
            return {};
        }

        if (!lightGridPrepass->Prepare(preparedComputeRequest.frameDescriptor, preparedFrameInputs.value()))
        {
            NLS_LOG_ERROR("LightGridPrepass::BuildPreparedComputeDispatchSource failed: LightGridPrepass::Prepare returned false.");
            return {};
        }
        return lightGridPrepass->GetPreparedComputeDispatchSource();
    }

    const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& LightGridPrepass::GetGraphicsPassBindingSet() const
    {
        return m_graphicsPassBindingSet;
    }

    NLS::Render::FrameGraph::PreparedComputeDispatchSource LightGridPrepass::GetPreparedComputeDispatchSource() const
    {
        return NLS::Render::FrameGraph::BuildPreparedComputeDispatchSource(m_computeDispatchInputs);
    }

    std::vector<NLS::Render::Context::RecordedComputeDispatchInput> LightGridPrepass::GetPreparedComputeDispatchInputs() const
    {
        return m_computeDispatchInputs;
    }

    bool LightGridPrepass::EnsureShadersLoaded()
    {
        if (!NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::ShaderManager>())
        {
            NLS_LOG_ERROR("LightGridPrepass requires ShaderManager to resolve engine shader assets.");
            return false;
        }

        auto& shaderManager = NLS_SERVICE(NLS::Core::ResourceManagement::ShaderManager);
        if (m_injectionShader == nullptr)
            m_injectionShader = shaderManager[":Shaders/LightGridInjection.hlsl"];
        if (m_compactShader == nullptr)
            m_compactShader = shaderManager[":Shaders/LightGridCompact.hlsl"];
        if (m_injectionShader == nullptr)
            NLS_LOG_ERROR("LightGridPrepass failed to load :Shaders/LightGridInjection.hlsl.");
        if (m_compactShader == nullptr)
            NLS_LOG_ERROR("LightGridPrepass failed to load :Shaders/LightGridCompact.hlsl.");
        return m_injectionShader != nullptr && m_compactShader != nullptr;
    }

    bool LightGridPrepass::EnsurePipelines()
    {
        if (m_injectionPipeline != nullptr && m_compactPipeline != nullptr)
            return true;

        auto device = NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(m_driver);
        auto pipelineCache = NLS::Render::Context::DriverRendererAccess::GetPipelineCache(m_driver);
        if (device == nullptr || m_injectionShader == nullptr || m_compactShader == nullptr)
        {
            NLS_LOG_ERROR("LightGridPrepass::EnsurePipelines failed: device or shader asset is null.");
            return false;
        }

        auto makePassSetLayoutDesc = [](const std::vector<NLS::Render::RHI::RHIBindingLayoutEntry>& entries, std::string_view debugName)
        {
            NLS::Render::RHI::RHIBindingLayoutDesc desc;
            desc.debugName = std::string(debugName);
            desc.entries = entries;
            return desc;
        };

        if (m_injectionBindingLayout == nullptr)
        {
            m_injectionBindingLayout = device->CreateBindingLayout(makePassSetLayoutDesc({
                { "LightGridPassConstants", NLS::Render::RHI::BindingType::UniformBuffer, NLS::Render::RHI::BindingPointMap::kPassDescriptorSet, 0u, 1u, NLS::Render::RHI::ShaderStageMask::Compute, NLS::Render::RHI::BindingPointMap::kPassBindingSpace },
                { "u_LightGridLights", NLS::Render::RHI::BindingType::StructuredBuffer, NLS::Render::RHI::BindingPointMap::kPassDescriptorSet, 0u, 1u, NLS::Render::RHI::ShaderStageMask::Compute, NLS::Render::RHI::BindingPointMap::kPassBindingSpace },
                { "u_LightGridClusterLightCounts", NLS::Render::RHI::BindingType::StorageBuffer, NLS::Render::RHI::BindingPointMap::kPassDescriptorSet, 1u, 1u, NLS::Render::RHI::ShaderStageMask::Compute, NLS::Render::RHI::BindingPointMap::kPassBindingSpace },
                { "u_LightGridClusterScratchIndices", NLS::Render::RHI::BindingType::StorageBuffer, NLS::Render::RHI::BindingPointMap::kPassDescriptorSet, 2u, 1u, NLS::Render::RHI::ShaderStageMask::Compute, NLS::Render::RHI::BindingPointMap::kPassBindingSpace },
                { "u_LightGridCompactCounter", NLS::Render::RHI::BindingType::StorageBuffer, NLS::Render::RHI::BindingPointMap::kPassDescriptorSet, 3u, 1u, NLS::Render::RHI::ShaderStageMask::Compute, NLS::Render::RHI::BindingPointMap::kPassBindingSpace },
                { "u_LightGridClusterRecords", NLS::Render::RHI::BindingType::StorageBuffer, NLS::Render::RHI::BindingPointMap::kPassDescriptorSet, 4u, 1u, NLS::Render::RHI::ShaderStageMask::Compute, NLS::Render::RHI::BindingPointMap::kPassBindingSpace },
                { "u_LightGridCompactIndices", NLS::Render::RHI::BindingType::StorageBuffer, NLS::Render::RHI::BindingPointMap::kPassDescriptorSet, 5u, 1u, NLS::Render::RHI::ShaderStageMask::Compute, NLS::Render::RHI::BindingPointMap::kPassBindingSpace }
            }, "LightGridInjectionBindingLayout"));
        }

        if (m_compactBindingLayout == nullptr)
        {
            m_compactBindingLayout = device->CreateBindingLayout(makePassSetLayoutDesc({
                { "LightGridPassConstants", NLS::Render::RHI::BindingType::UniformBuffer, NLS::Render::RHI::BindingPointMap::kPassDescriptorSet, 0u, 1u, NLS::Render::RHI::ShaderStageMask::Compute, NLS::Render::RHI::BindingPointMap::kPassBindingSpace },
                { "u_LightGridClusterLightCounts", NLS::Render::RHI::BindingType::StructuredBuffer, NLS::Render::RHI::BindingPointMap::kPassDescriptorSet, 1u, 1u, NLS::Render::RHI::ShaderStageMask::Compute, NLS::Render::RHI::BindingPointMap::kPassBindingSpace },
                { "u_LightGridClusterScratchIndices", NLS::Render::RHI::BindingType::StructuredBuffer, NLS::Render::RHI::BindingPointMap::kPassDescriptorSet, 2u, 1u, NLS::Render::RHI::ShaderStageMask::Compute, NLS::Render::RHI::BindingPointMap::kPassBindingSpace },
                { "u_LightGridCompactCounter", NLS::Render::RHI::BindingType::StorageBuffer, NLS::Render::RHI::BindingPointMap::kPassDescriptorSet, 3u, 1u, NLS::Render::RHI::ShaderStageMask::Compute, NLS::Render::RHI::BindingPointMap::kPassBindingSpace },
                { "u_LightGridClusterRecords", NLS::Render::RHI::BindingType::StorageBuffer, NLS::Render::RHI::BindingPointMap::kPassDescriptorSet, 4u, 1u, NLS::Render::RHI::ShaderStageMask::Compute, NLS::Render::RHI::BindingPointMap::kPassBindingSpace },
                { "u_LightGridCompactIndices", NLS::Render::RHI::BindingType::StorageBuffer, NLS::Render::RHI::BindingPointMap::kPassDescriptorSet, 5u, 1u, NLS::Render::RHI::ShaderStageMask::Compute, NLS::Render::RHI::BindingPointMap::kPassBindingSpace }
            }, "LightGridCompactBindingLayout"));
        }

        if (m_graphicsBindingLayout == nullptr)
        {
            m_graphicsBindingLayout = device->CreateBindingLayout(makePassSetLayoutDesc({
                { "LightGridPassConstants", NLS::Render::RHI::BindingType::UniformBuffer, NLS::Render::RHI::BindingPointMap::kPassDescriptorSet, 0u, 1u, NLS::Render::RHI::ShaderStageMask::All, NLS::Render::RHI::BindingPointMap::kPassBindingSpace },
                { "u_LightGridLights", NLS::Render::RHI::BindingType::StructuredBuffer, NLS::Render::RHI::BindingPointMap::kPassDescriptorSet, 0u, 1u, NLS::Render::RHI::ShaderStageMask::AllGraphics, NLS::Render::RHI::BindingPointMap::kPassBindingSpace },
                { "u_LightGridClusterRecords", NLS::Render::RHI::BindingType::StructuredBuffer, NLS::Render::RHI::BindingPointMap::kPassDescriptorSet, 1u, 1u, NLS::Render::RHI::ShaderStageMask::AllGraphics, NLS::Render::RHI::BindingPointMap::kPassBindingSpace },
                { "u_LightGridCompactIndices", NLS::Render::RHI::BindingType::StructuredBuffer, NLS::Render::RHI::BindingPointMap::kPassDescriptorSet, 2u, 1u, NLS::Render::RHI::ShaderStageMask::AllGraphics, NLS::Render::RHI::BindingPointMap::kPassBindingSpace }
            }, "LightGridGraphicsBindingLayout"));
        }

        if (m_injectionPipelineLayout == nullptr)
        {
            NLS::Render::RHI::RHIPipelineLayoutDesc desc;
            desc.debugName = "LightGridInjectionPipelineLayout";
            desc.bindingLayouts.resize(NLS::Render::RHI::BindingPointMap::kPassDescriptorSet + 1u);
            desc.bindingLayouts[NLS::Render::RHI::BindingPointMap::kPassDescriptorSet] = m_injectionBindingLayout;
            m_injectionPipelineLayout = device->CreatePipelineLayout(desc);
        }

        if (m_compactPipelineLayout == nullptr)
        {
            NLS::Render::RHI::RHIPipelineLayoutDesc desc;
            desc.debugName = "LightGridCompactPipelineLayout";
            desc.bindingLayouts.resize(NLS::Render::RHI::BindingPointMap::kPassDescriptorSet + 1u);
            desc.bindingLayouts[NLS::Render::RHI::BindingPointMap::kPassDescriptorSet] = m_compactBindingLayout;
            m_compactPipelineLayout = device->CreatePipelineLayout(desc);
        }

        auto createComputePipeline = [&](NLS::Render::Resources::Shader* shader, const std::shared_ptr<NLS::Render::RHI::RHIPipelineLayout>& pipelineLayout, const std::shared_ptr<NLS::Render::RHI::RHIComputePipeline>& existingPipeline, std::string_view label)
        {
            if (existingPipeline != nullptr)
                return existingPipeline;

            auto shaderModule = shader->GetOrCreateExplicitShaderModule(device, NLS::Render::ShaderCompiler::ShaderStage::Compute);
            if (shaderModule == nullptr)
            {
                NLS_LOG_ERROR("LightGridPrepass::EnsurePipelines failed: compute shader module is null for " + std::string(label) + ".");
                return std::shared_ptr<NLS::Render::RHI::RHIComputePipeline>{};
            }

            if (pipelineCache == nullptr)
            {
                NLS_LOG_ERROR("LightGridPrepass::EnsurePipelines failed: pipeline cache is null for " + std::string(label) + ".");
                return std::shared_ptr<NLS::Render::RHI::RHIComputePipeline>{};
            }

            const auto cacheKey = BuildComputePipelineCacheKey(label, pipelineLayout, shaderModule);
            return pipelineCache->GetOrCreateComputePipeline(
                cacheKey,
                [device, pipelineLayout, shaderModule, label]()
                {
                    NLS::Render::RHI::RHIComputePipelineDesc desc;
                    desc.pipelineLayout = pipelineLayout;
                    desc.computeShader = shaderModule;
                    desc.debugName = std::string(label);
                    return device->CreateComputePipeline(desc);
                },
                NLS::Render::RHI::PipelineCacheRequestMode::Prewarm);
        };

        m_injectionPipeline = createComputePipeline(m_injectionShader, m_injectionPipelineLayout, m_injectionPipeline, "LightGridInjectionPipeline");
        m_compactPipeline = createComputePipeline(m_compactShader, m_compactPipelineLayout, m_compactPipeline, "LightGridCompactPipeline");
        if (m_injectionPipeline == nullptr)
            NLS_LOG_ERROR("LightGridPrepass::EnsurePipelines failed: LightGridInjectionPipeline is null.");
        if (m_compactPipeline == nullptr)
            NLS_LOG_ERROR("LightGridPrepass::EnsurePipelines failed: LightGridCompactPipeline is null.");
        return m_injectionPipeline != nullptr && m_compactPipeline != nullptr;
    }

    bool LightGridPrepass::BuildFrameData(
        const NLS::Render::Data::FrameDescriptor& frameDescriptor,
        const NLS::Render::Data::LightingDescriptor& lightingDescriptor,
        const bool hasSkyboxTexture,
        PackedFrameData& outFrameData) const
    {
        return BuildFrameData(
            frameDescriptor,
            CaptureFrameInputs(lightingDescriptor, hasSkyboxTexture),
            hasSkyboxTexture,
            outFrameData);
    }

    bool LightGridPrepass::BuildFrameData(
        const NLS::Render::Data::FrameDescriptor& frameDescriptor,
        const PreparedFrameInputs& preparedFrameInputs,
        const bool hasSkyboxTexture,
        PackedFrameData& outFrameData) const
    {
        if (frameDescriptor.camera == nullptr)
        {
            NLS_LOG_ERROR("LightGridPrepass::BuildFrameData failed: frame descriptor camera is null.");
            return false;
        }

        outFrameData.constants.viewMatrix = NLS::Maths::Matrix4::Transpose(frameDescriptor.camera->GetViewMatrix());
        outFrameData.constants.projectionMatrix = NLS::Maths::Matrix4::Transpose(frameDescriptor.camera->GetProjectionMatrix());
        const auto viewProjection = frameDescriptor.camera->GetProjectionMatrix() * frameDescriptor.camera->GetViewMatrix();
        outFrameData.constants.inverseViewProjection = NLS::Maths::Matrix4::Transpose(NLS::Maths::Matrix4::Inverse(viewProjection));
        outFrameData.constants.cameraWorldPositionNearPlane = {
            frameDescriptor.camera->GetPosition().x,
            frameDescriptor.camera->GetPosition().y,
            frameDescriptor.camera->GetPosition().z,
            frameDescriptor.camera->GetNear()
        };
        outFrameData.constants.renderSizeFarPlane = {
            static_cast<float>(frameDescriptor.renderWidth),
            static_cast<float>(frameDescriptor.renderHeight),
            1.0f / static_cast<float>(frameDescriptor.renderWidth == 0u ? 1u : frameDescriptor.renderWidth),
            frameDescriptor.camera->GetFar()
        };
        outFrameData.constants.gridParams = {
            static_cast<float>(m_settings.gridSizeX),
            static_cast<float>(m_settings.gridSizeY),
            static_cast<float>(m_settings.gridSizeZ),
            static_cast<float>(m_settings.maxLightsPerCluster)
        };
        outFrameData.constants.lightingParams = {
            static_cast<float>(preparedFrameInputs.lights.size()),
            0.15f,
            kDefaultAmbientFloor,
            hasSkyboxTexture ? 1.0f : 0.0f
        };

        outFrameData.packedLights.reserve(preparedFrameInputs.lights.size() * kLightWordStride);
        for (const auto& light : preparedFrameInputs.lights)
            PackCapturedLight(light, outFrameData.packedLights);

        const uint32_t clusterCount = m_settings.gridSizeX * m_settings.gridSizeY * m_settings.gridSizeZ;
        outFrameData.clusterLightCounts.assign(clusterCount, 0u);
        outFrameData.clusterScratchIndices.assign(clusterCount * m_settings.maxLightsPerCluster, 0u);
        outFrameData.compactCounter.assign(1u, 0u);
        outFrameData.clusterRecords.assign(clusterCount * kRecordWordStride, 0u);
        outFrameData.compactLightIndices.assign(clusterCount * m_settings.maxLightsPerCluster, 0u);
        return true;
    }
}
