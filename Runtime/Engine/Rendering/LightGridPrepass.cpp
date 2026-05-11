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
#include <Rendering/FrameGraph/FrameGraphExecutionPlan.h>
#include <Rendering/RHI/BindingPointMap.h>
#include <Rendering/RHI/Core/RHIDevice.h>
#include <Rendering/RHI/Utils/PipelineCache/PipelineCache.h>
#include <Rendering/Resources/ComputeShaderUtils.h>
#include <Rendering/Shaders/LightGridShaders.h>
#include <Rendering/Settings/DriverSettings.h>

#include <Profiling/Profiler.h>

namespace
{
    constexpr uint32_t kLightWordStride = 16u;
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

    bool ShouldLogLightGridHotPathFailureDiagnostics(const NLS::Render::Context::Driver& driver)
    {
        return NLS::Engine::Rendering::LightGridPrepass::ShouldLogHotPathFailureDiagnostics(
            NLS::Render::Context::DriverRendererAccess::GetDiagnosticsSettings(driver));
    }

    void LogLightGridHotPathFailure(
        const NLS::Render::Context::Driver& driver,
        const std::string& message)
    {
        if (ShouldLogLightGridHotPathFailureDiagnostics(driver))
            NLS_LOG_ERROR(message);
    }

    bool PipelineCacheKeysEqual(
        const NLS::Render::RHI::PipelineCacheKey& lhs,
        const NLS::Render::RHI::PipelineCacheKey& rhs)
    {
        return lhs.hash == rhs.hash &&
            lhs.backend == rhs.backend &&
            lhs.stableDebugName == rhs.stableDebugName;
    }

    bool IsPipelineCacheKeyUnset(const NLS::Render::RHI::PipelineCacheKey& key)
    {
        return key.hash == 0u &&
            key.backend == NLS::Render::RHI::NativeBackendType::None &&
            key.stableDebugName.empty();
    }

    bool MatchesPipelineCacheKey(
        const NLS::Render::RHI::PipelineCacheKey& cachedKey,
        const NLS::Render::RHI::PipelineCacheKey& currentKey)
    {
        return IsPipelineCacheKeyUnset(cachedKey) ||
            PipelineCacheKeysEqual(cachedKey, currentKey);
    }

    bool AreSameLightGridSettings(
        const NLS::Engine::Rendering::ClusteredShadingSettings& lhs,
        const NLS::Engine::Rendering::ClusteredShadingSettings& rhs)
    {
        return lhs.lightGridPixelSize == rhs.lightGridPixelSize &&
            lhs.gridSizeX == rhs.gridSizeX &&
            lhs.gridSizeY == rhs.gridSizeY &&
            lhs.gridSizeZ == rhs.gridSizeZ &&
            lhs.maxLightsPerCluster == rhs.maxLightsPerCluster &&
            lhs.linkedListCulling == rhs.linkedListCulling;
    }

    bool AreSameCapturedLights(
        const std::vector<NLS::Engine::Rendering::LightGridPrepass::CapturedLight>& lhs,
        const std::vector<NLS::Engine::Rendering::LightGridPrepass::CapturedLight>& rhs)
    {
        if (lhs.size() != rhs.size())
            return false;

        for (size_t index = 0u; index < lhs.size(); ++index)
        {
            const auto& left = lhs[index];
            const auto& right = rhs[index];
            if (!(left.position == right.position) ||
                !(left.forward == right.forward) ||
                !(left.color == right.color) ||
                left.effectRange != right.effectRange ||
                left.intensity != right.intensity ||
                left.constant != right.constant ||
                left.linear != right.linear ||
                left.quadratic != right.quadratic ||
                left.outerCutoff != right.outerCutoff ||
                left.type != right.type)
                return false;
        }

        return true;
    }

    std::shared_ptr<NLS::Render::RHI::RHIBuffer> FindBindingBuffer(
        const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& bindingSet,
        const std::string_view name,
        const NLS::Render::RHI::BindingType type,
        const uint32_t binding)
    {
        if (bindingSet == nullptr || bindingSet->GetDesc().layout == nullptr)
            return nullptr;

        const auto& layoutEntries = bindingSet->GetDesc().layout->GetDesc().entries;
        const auto layoutEntry = std::find_if(
            layoutEntries.begin(),
            layoutEntries.end(),
            [name, type, binding](const NLS::Render::RHI::RHIBindingLayoutEntry& entry)
            {
                return entry.name == name && entry.type == type && entry.binding == binding;
            });
        if (layoutEntry == layoutEntries.end())
            return nullptr;

        const auto& bindingEntries = bindingSet->GetDesc().entries;
        const auto bindingEntry = std::find_if(
            bindingEntries.begin(),
            bindingEntries.end(),
            [layoutEntry](const NLS::Render::RHI::RHIBindingSetEntry& entry)
            {
                return entry.type == layoutEntry->type && entry.binding == layoutEntry->binding;
            });
        return bindingEntry != bindingEntries.end() ? bindingEntry->buffer : nullptr;
    }

}

namespace NLS::Engine::Rendering
{
    NLS::Render::Resources::ShaderParameterStruct LightGridPrepass::LightGridResetParameters::Build()
    {
        return NLS::Render::Engine::Shaders::LightGridResetCS::GetStaticShaderType().GetRootParameterStructs().front();
    }

    NLS::Render::Resources::ShaderParameterStruct LightGridPrepass::LightGridInjectionParameters::Build()
    {
        return NLS::Render::Engine::Shaders::LightGridInjectionCS::GetStaticShaderType().GetRootParameterStructs().front();
    }

    NLS::Render::Resources::ShaderParameterStruct LightGridPrepass::LightGridCompactParameters::Build()
    {
        return NLS::Render::Engine::Shaders::LightGridCompactCS::GetStaticShaderType().GetRootParameterStructs().front();
    }

    NLS::Render::Resources::ShaderParameterStruct LightGridPrepass::LightGridGraphicsParameters::Build()
    {
        return NLS::Render::Resources::ShaderParameterStructBuilder("LightGridGraphicsParameters")
            .SetGroup(NLS::Render::Resources::ShaderParameterGroupKind::Pass)
            .AddUniformBuffer("ForwardLightData", 0u, sizeof(ForwardLightData), NLS::Render::RHI::ShaderStageMask::AllGraphics)
            .AddStructuredBuffer("ForwardLocalLightBuffer", 0u, NLS::Render::RHI::ShaderStageMask::AllGraphics)
            .AddStructuredBuffer("NumCulledLightsGrid", 1u, NLS::Render::RHI::ShaderStageMask::AllGraphics)
            .AddStructuredBuffer("CulledLightDataGrid", 2u, NLS::Render::RHI::ShaderStageMask::AllGraphics)
            .Build();
    }

    LightGridPrepass::LightGridPrepass(NLS::Render::Context::Driver& driver)
        : m_driver(driver)
    {
    }

    LightGridPrepass::~LightGridPrepass() = default;

    bool LightGridPrepass::ShouldLogHotPathFailureDiagnostics(
        const NLS::Render::Settings::EngineDiagnosticsSettings& diagnostics)
    {
        return diagnostics.logRenderDrawPath;
    }

    LightGridPrepass::PreparedFrameInputs LightGridPrepass::CaptureFrameInputs(
        const NLS::Render::Data::LightingDescriptor& lightingDescriptor,
        const bool hasSkyboxTexture)
    {
        NLS_PROFILE_SCOPE();
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
        NLS_PROFILE_SCOPE();
        return Prepare(
            frameDescriptor,
            CaptureFrameInputs(lightingDescriptor, hasSkyboxTexture));
    }

    bool LightGridPrepass::Prepare(
        const NLS::Render::Data::FrameDescriptor& frameDescriptor,
        const PreparedFrameInputs& preparedFrameInputs)
    {
        NLS_PROFILE_SCOPE();
        m_computeDispatchInputs.clear();
        const auto preparedCacheKey = BuildPreparedResourceCacheKey(frameDescriptor, preparedFrameInputs);

        auto device = NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(m_driver);
        if (device == nullptr)
        {
            LogLightGridHotPathFailure(m_driver, "LightGridPrepass::Prepare failed: explicit RHI device is unavailable.");
            return false;
        }

        if (!EnsureShadersLoaded())
        {
            LogLightGridHotPathFailure(m_driver, "LightGridPrepass::Prepare failed: clustered lighting shaders are not loaded.");
            return false;
        }

        if (!EnsurePipelines())
        {
            LogLightGridHotPathFailure(m_driver, "LightGridPrepass::Prepare failed: clustered lighting compute pipelines are unavailable.");
            return false;
        }

        auto& frameData = m_frameScratch;
        if (!BuildFrameData(frameDescriptor, preparedFrameInputs, preparedFrameInputs.hasSkyboxTexture, frameData))
        {
            LogLightGridHotPathFailure(m_driver, "LightGridPrepass::Prepare failed: frame data could not be built.");
            return false;
        }

        if (preparedCacheKey.has_value() && TryReusePreparedResources(preparedCacheKey.value(), frameData.forwardLightData))
            return true;

        NLS::Render::RHI::RHIBufferDesc constantsDesc;
        constantsDesc.size = sizeof(ForwardLightData);
        constantsDesc.usage = NLS::Render::RHI::BufferUsageFlags::Uniform;
        constantsDesc.memoryUsage = NLS::Render::RHI::MemoryUsage::CPUToGPU;
        constantsDesc.debugName = "ForwardLightDataUniformBuffer";
        NLS::Render::RHI::RHIBufferUploadDesc constantsUploadDesc;
        constantsUploadDesc.data = &frameData.forwardLightData;
        constantsUploadDesc.dataSize = sizeof(frameData.forwardLightData);
        constantsUploadDesc.debugName = "ForwardLightDataInitialUpload";
        auto constantsBuffer = device->CreateBuffer(constantsDesc, constantsUploadDesc);

        auto createStorageBuffer = [&](std::string_view debugName, const std::vector<uint32_t>& data)
        {
            NLS::Render::RHI::RHIBufferDesc desc;
            desc.size = std::max<size_t>(sizeof(uint32_t), data.size() * sizeof(uint32_t));
            desc.usage = NLS::Render::RHI::BufferUsageFlags::Storage;
            desc.memoryUsage = NLS::Render::RHI::MemoryUsage::GPUOnly;
            desc.debugName = std::string(debugName);
            if (data.empty())
                return device->CreateBuffer(desc);

            NLS::Render::RHI::RHIBufferUploadDesc uploadDesc;
            uploadDesc.data = data.data();
            uploadDesc.dataSize = data.size() * sizeof(uint32_t);
            uploadDesc.debugName = desc.debugName + "InitialUpload";
            return device->CreateBuffer(desc, uploadDesc);
        };

        auto CreateOrReusePreparedBuffer = [&](
            std::shared_ptr<NLS::Render::RHI::RHIBuffer>& cachedBuffer,
            size_t& cachedSize,
            std::string_view debugName,
            const std::vector<uint32_t>& data)
        {
            const size_t requestedSize = std::max<size_t>(sizeof(uint32_t), data.size() * sizeof(uint32_t));
            if (cachedBuffer != nullptr && cachedSize == requestedSize)
                return cachedBuffer;

            NLS::Render::RHI::RHIBufferDesc desc;
            desc.size = requestedSize;
            desc.usage = NLS::Render::RHI::BufferUsageFlags::Storage;
            desc.memoryUsage = NLS::Render::RHI::MemoryUsage::GPUOnly;
            desc.debugName = std::string(debugName);
            cachedBuffer = device->CreateBuffer(desc);
            cachedSize = cachedBuffer != nullptr ? requestedSize : 0u;
            return cachedBuffer;
        };

        auto forwardLocalLightBuffer = createStorageBuffer("ForwardLocalLightBuffer", frameData.forwardLocalLightData);
        auto startOffsetGridBuffer = CreateOrReusePreparedBuffer(
            m_preparedBufferCache.startOffsetGrid,
            m_preparedBufferCache.startOffsetGridSize,
            "LightGridStartOffsetGrid",
            frameData.startOffsetGrid);
        auto culledLightLinksBuffer = CreateOrReusePreparedBuffer(
            m_preparedBufferCache.culledLightLinks,
            m_preparedBufferCache.culledLightLinksSize,
            "LightGridCulledLightLinks",
            frameData.culledLightLinks);
        auto linkCounterBuffer = CreateOrReusePreparedBuffer(
            m_preparedBufferCache.linkCounter,
            m_preparedBufferCache.linkCounterSize,
            "LightGridLinkCounter",
            frameData.linkCounter);
        auto compactCounterBuffer = CreateOrReusePreparedBuffer(
            m_preparedBufferCache.compactCounter,
            m_preparedBufferCache.compactCounterSize,
            "LightGridCompactCounter",
            frameData.compactCounter);
        auto numCulledLightsGridBuffer = CreateOrReusePreparedBuffer(
            m_preparedBufferCache.numCulledLightsGrid,
            m_preparedBufferCache.numCulledLightsGridSize,
            "NumCulledLightsGrid",
            frameData.numCulledLightsGrid);
        auto culledLightDataGridBuffer = CreateOrReusePreparedBuffer(
            m_preparedBufferCache.culledLightDataGrid,
            m_preparedBufferCache.culledLightDataGridSize,
            "CulledLightDataGrid",
            frameData.culledLightDataGrid);

        if (constantsBuffer == nullptr ||
            forwardLocalLightBuffer == nullptr ||
            startOffsetGridBuffer == nullptr ||
            culledLightLinksBuffer == nullptr ||
            linkCounterBuffer == nullptr ||
            compactCounterBuffer == nullptr ||
            numCulledLightsGridBuffer == nullptr ||
            culledLightDataGridBuffer == nullptr)
        {
            LogLightGridHotPathFailure(m_driver, "LightGridPrepass::Prepare failed: one or more clustered lighting buffers could not be created.");
            return false;
        }

        const auto descriptorLifetime = NLS::Render::RHI::DescriptorAllocationLifetime::Persistent;

        auto injectionSetDesc = NLS::Render::Resources::BuildBindingSetDescFromShaderParameters(
            m_injectionGlobalShader.parameters,
            m_injectionBindingLayout,
            {
                NLS::Render::Resources::ShaderParameterBindingValue::UniformBuffer("Forward", constantsBuffer, sizeof(ForwardLightData)),
                NLS::Render::Resources::ShaderParameterBindingValue::StructuredBuffer("ForwardLocalLightBuffer", forwardLocalLightBuffer, frameData.forwardLocalLightData.size() * sizeof(uint32_t)),
                NLS::Render::Resources::ShaderParameterBindingValue::StorageBuffer("RWStartOffsetGrid", startOffsetGridBuffer, frameData.startOffsetGrid.size() * sizeof(uint32_t)),
                NLS::Render::Resources::ShaderParameterBindingValue::StorageBuffer("RWCulledLightLinks", culledLightLinksBuffer, frameData.culledLightLinks.size() * sizeof(uint32_t)),
                NLS::Render::Resources::ShaderParameterBindingValue::StorageBuffer("RWLinkCounter", linkCounterBuffer, frameData.linkCounter.size() * sizeof(uint32_t))
            },
            "LightGridInjectionBindingSet");
        auto injectionBindingSet = NLS::Render::Context::DriverRendererAccess::CreateExplicitBindingSet(
            m_driver,
            injectionSetDesc,
            descriptorLifetime);

        auto resetSetDesc = NLS::Render::Resources::BuildBindingSetDescFromShaderParameters(
            m_resetGlobalShader.parameters,
            m_resetBindingLayout,
            {
                NLS::Render::Resources::ShaderParameterBindingValue::UniformBuffer("Forward", constantsBuffer, sizeof(ForwardLightData)),
                NLS::Render::Resources::ShaderParameterBindingValue::StorageBuffer("RWStartOffsetGrid", startOffsetGridBuffer, frameData.startOffsetGrid.size() * sizeof(uint32_t)),
                NLS::Render::Resources::ShaderParameterBindingValue::StorageBuffer("RWCulledLightLinks", culledLightLinksBuffer, frameData.culledLightLinks.size() * sizeof(uint32_t)),
                NLS::Render::Resources::ShaderParameterBindingValue::StorageBuffer("RWLinkCounter", linkCounterBuffer, frameData.linkCounter.size() * sizeof(uint32_t)),
                NLS::Render::Resources::ShaderParameterBindingValue::StorageBuffer("RWCompactCounter", compactCounterBuffer, frameData.compactCounter.size() * sizeof(uint32_t)),
                NLS::Render::Resources::ShaderParameterBindingValue::StorageBuffer("RWNumCulledLightsGrid", numCulledLightsGridBuffer, frameData.numCulledLightsGrid.size() * sizeof(uint32_t)),
                NLS::Render::Resources::ShaderParameterBindingValue::StorageBuffer("RWCulledLightDataGrid", culledLightDataGridBuffer, frameData.culledLightDataGrid.size() * sizeof(uint32_t))
            },
            "LightGridResetBindingSet");
        auto resetBindingSet = NLS::Render::Context::DriverRendererAccess::CreateExplicitBindingSet(
            m_driver,
            resetSetDesc,
            descriptorLifetime);

        auto compactSetDesc = NLS::Render::Resources::BuildBindingSetDescFromShaderParameters(
            m_compactGlobalShader.parameters,
            m_compactBindingLayout,
            {
                NLS::Render::Resources::ShaderParameterBindingValue::UniformBuffer("Forward", constantsBuffer, sizeof(ForwardLightData)),
                NLS::Render::Resources::ShaderParameterBindingValue::StructuredBuffer("StartOffsetGrid", startOffsetGridBuffer, frameData.startOffsetGrid.size() * sizeof(uint32_t)),
                NLS::Render::Resources::ShaderParameterBindingValue::StructuredBuffer("CulledLightLinks", culledLightLinksBuffer, frameData.culledLightLinks.size() * sizeof(uint32_t)),
                NLS::Render::Resources::ShaderParameterBindingValue::StorageBuffer("RWCompactCounter", compactCounterBuffer, frameData.compactCounter.size() * sizeof(uint32_t)),
                NLS::Render::Resources::ShaderParameterBindingValue::StorageBuffer("RWNumCulledLightsGrid", numCulledLightsGridBuffer, frameData.numCulledLightsGrid.size() * sizeof(uint32_t)),
                NLS::Render::Resources::ShaderParameterBindingValue::StorageBuffer("RWCulledLightDataGrid", culledLightDataGridBuffer, frameData.culledLightDataGrid.size() * sizeof(uint32_t))
            },
            "LightGridCompactBindingSet");
        auto compactBindingSet = NLS::Render::Context::DriverRendererAccess::CreateExplicitBindingSet(
            m_driver,
            compactSetDesc,
            descriptorLifetime);

        auto graphicsSetDesc = NLS::Render::Resources::BuildBindingSetDescFromShaderParameters(
            LightGridGraphicsParameters::Build(),
            m_graphicsBindingLayout,
            {
                NLS::Render::Resources::ShaderParameterBindingValue::UniformBuffer("ForwardLightData", constantsBuffer, sizeof(ForwardLightData)),
                NLS::Render::Resources::ShaderParameterBindingValue::StructuredBuffer("ForwardLocalLightBuffer", forwardLocalLightBuffer, frameData.forwardLocalLightData.size() * sizeof(uint32_t)),
                NLS::Render::Resources::ShaderParameterBindingValue::StructuredBuffer("NumCulledLightsGrid", numCulledLightsGridBuffer, frameData.numCulledLightsGrid.size() * sizeof(uint32_t)),
                NLS::Render::Resources::ShaderParameterBindingValue::StructuredBuffer("CulledLightDataGrid", culledLightDataGridBuffer, frameData.culledLightDataGrid.size() * sizeof(uint32_t))
            },
            "LightGridGraphicsBindingSet");
        m_graphicsPassBindingSet = NLS::Render::Context::DriverRendererAccess::CreateExplicitBindingSet(
            m_driver,
            graphicsSetDesc,
            descriptorLifetime);

        if (resetBindingSet == nullptr || injectionBindingSet == nullptr || compactBindingSet == nullptr || m_graphicsPassBindingSet == nullptr)
        {
            LogLightGridHotPathFailure(m_driver, "LightGridPrepass::Prepare failed: one or more clustered lighting binding sets could not be created.");
            return false;
        }

        const LightGridDimensions gridDimensions{
            static_cast<uint32_t>(frameData.forwardLightData.gridParams.x),
            static_cast<uint32_t>(frameData.forwardLightData.gridParams.y),
            static_cast<uint32_t>(frameData.forwardLightData.gridParams.z)
        };
        const auto gridDispatchGroups = CalculateLightGridDispatchGroups(gridDimensions);

        auto resetDispatchInput = NLS::Render::Resources::ComputeShaderUtils::BuildRecordedDispatch(
            m_resetGlobalShader,
            m_resetPipeline,
            resetBindingSet,
            { gridDispatchGroups.x, gridDispatchGroups.y, gridDispatchGroups.z },
            "LightGridReset");
        resetDispatchInput.shaderWriteBuffersBefore = {
            startOffsetGridBuffer,
            culledLightLinksBuffer,
            linkCounterBuffer,
            compactCounterBuffer,
            numCulledLightsGridBuffer,
            culledLightDataGridBuffer
        };

        auto injectionDispatchInput = NLS::Render::Resources::ComputeShaderUtils::BuildRecordedDispatch(
            m_injectionGlobalShader,
            m_injectionPipeline,
            injectionBindingSet,
            { gridDispatchGroups.x, gridDispatchGroups.y, gridDispatchGroups.z },
            "LightGridInjection");
        injectionDispatchInput.shaderReadBuffersBefore = { forwardLocalLightBuffer };
        injectionDispatchInput.shaderWriteBuffersBefore = {
            startOffsetGridBuffer,
            culledLightLinksBuffer,
            linkCounterBuffer
        };

        auto compactDispatchInput = NLS::Render::Resources::ComputeShaderUtils::BuildRecordedDispatch(
            m_compactGlobalShader,
            m_compactPipeline,
            compactBindingSet,
            { gridDispatchGroups.x, gridDispatchGroups.y, gridDispatchGroups.z },
            "LightGridCompact");
        compactDispatchInput.shaderReadBuffersBefore = { startOffsetGridBuffer, culledLightLinksBuffer };
        compactDispatchInput.shaderWriteBuffersBefore = {
            compactCounterBuffer,
            numCulledLightsGridBuffer,
            culledLightDataGridBuffer
        };
        compactDispatchInput.shaderReadBuffersAfter = { numCulledLightsGridBuffer, culledLightDataGridBuffer };

        m_computeDispatchInputs = {
            std::move(resetDispatchInput),
            std::move(injectionDispatchInput),
            std::move(compactDispatchInput)
        };

        if (preparedCacheKey.has_value())
            StorePreparedResourceCache(preparedCacheKey.value());

        return true;
    }

    std::optional<LightGridPrepass::PreparedResourceCacheKey> LightGridPrepass::BuildPreparedResourceCacheKey(
        const NLS::Render::Data::FrameDescriptor& frameDescriptor,
        const PreparedFrameInputs& preparedFrameInputs) const
    {
        if (frameDescriptor.camera == nullptr)
            return std::nullopt;

        PreparedResourceCacheKey key;
        key.renderWidth = frameDescriptor.renderWidth;
        key.renderHeight = frameDescriptor.renderHeight;
        key.nearPlane = frameDescriptor.camera->GetNear();
        key.farPlane = frameDescriptor.camera->GetFar();
        key.settings = m_settings;
        key.hasSkyboxTexture = preparedFrameInputs.hasSkyboxTexture;
        key.lights = preparedFrameInputs.lights;
        return key;
    }

    bool LightGridPrepass::TryReusePreparedResources(
        const PreparedResourceCacheKey& key,
        const ForwardLightData& forwardLightData)
    {
        if (!m_preparedResourceCache.valid ||
            !LightGridPrepass::AreSamePreparedResourceCacheKeys(m_preparedResourceCache.key, key) ||
            m_preparedResourceCache.forwardLightDataBuffer == nullptr ||
            m_preparedResourceCache.graphicsPassBindingSet == nullptr ||
            m_preparedResourceCache.computeDispatchInputs.size() != 3u ||
            m_preparedResourceCache.computeDispatchInputs[0].pipeline != m_resetPipeline ||
            m_preparedResourceCache.computeDispatchInputs[1].pipeline != m_injectionPipeline ||
            m_preparedResourceCache.computeDispatchInputs[2].pipeline != m_compactPipeline)
            return false;

        NLS::Render::RHI::RHIBufferUploadDesc constantsUploadDesc;
        constantsUploadDesc.data = &forwardLightData;
        constantsUploadDesc.dataSize = sizeof(forwardLightData);
        constantsUploadDesc.debugName = "ForwardLightDataCameraMotionUpload";
        const auto updateResult = m_preparedResourceCache.forwardLightDataBuffer->UpdateData(constantsUploadDesc);
        if (!updateResult.Succeeded())
            return false;

        m_graphicsPassBindingSet = m_preparedResourceCache.graphicsPassBindingSet;
        if (AreSameForwardLightData(m_preparedResourceCache.forwardLightData, forwardLightData))
            m_computeDispatchInputs.clear();
        else
            m_computeDispatchInputs = m_preparedResourceCache.computeDispatchInputs;
        m_preparedResourceCache.forwardLightData = forwardLightData;
        return true;
    }

    bool LightGridPrepass::EnsureGraphicsBindingLayout()
    {
        if (m_graphicsBindingLayout != nullptr)
            return true;

        auto device = NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(m_driver);
        if (device == nullptr)
        {
            LogLightGridHotPathFailure(m_driver, "LightGridPrepass::EnsureGraphicsBindingLayout failed: explicit RHI device is unavailable.");
            return false;
        }

        auto desc = NLS::Render::Resources::BuildBindingLayoutDescFromShaderParameters(
            LightGridGraphicsParameters::Build(),
            "LightGridGraphicsBindingLayout");
        m_graphicsBindingLayout = device->CreateBindingLayout(desc);
        return m_graphicsBindingLayout != nullptr;
    }

    bool LightGridPrepass::EnsureFallbackGraphicsPassBindingSet(
        const NLS::Render::Data::FrameDescriptor& frameDescriptor,
        const bool hasSkyboxTexture)
    {
        auto device = NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(m_driver);
        if (device == nullptr)
        {
            LogLightGridHotPathFailure(m_driver, "LightGridPrepass::EnsureFallbackGraphicsPassBindingSet failed: explicit RHI device is unavailable.");
            return false;
        }

        if (!EnsureGraphicsBindingLayout())
            return false;

        PreparedFrameInputs fallbackInputs;
        fallbackInputs.hasSkyboxTexture = hasSkyboxTexture;
        PackedFrameData frameData;
        if (!BuildFrameData(frameDescriptor, fallbackInputs, hasSkyboxTexture, frameData))
            return false;

        NLS::Render::RHI::RHIBufferDesc constantsDesc;
        constantsDesc.size = sizeof(ForwardLightData);
        constantsDesc.usage = NLS::Render::RHI::BufferUsageFlags::Uniform;
        constantsDesc.memoryUsage = NLS::Render::RHI::MemoryUsage::CPUToGPU;
        constantsDesc.debugName = "ForwardLightDataUniformBuffer";
        NLS::Render::RHI::RHIBufferUploadDesc constantsUploadDesc;
        constantsUploadDesc.data = &frameData.forwardLightData;
        constantsUploadDesc.dataSize = sizeof(frameData.forwardLightData);
        constantsUploadDesc.debugName = "ForwardLightDataFallbackInitialUpload";
        auto constantsBuffer = device->CreateBuffer(constantsDesc, constantsUploadDesc);

        auto createStructuredBuffer = [&device](std::string_view debugName, const std::vector<uint32_t>& data)
        {
            NLS::Render::RHI::RHIBufferDesc desc;
            desc.size = std::max<size_t>(sizeof(uint32_t), data.size() * sizeof(uint32_t));
            desc.usage = NLS::Render::RHI::BufferUsageFlags::Storage;
            desc.memoryUsage = NLS::Render::RHI::MemoryUsage::GPUOnly;
            desc.debugName = std::string(debugName);
            if (data.empty())
                return device->CreateBuffer(desc);

            NLS::Render::RHI::RHIBufferUploadDesc uploadDesc;
            uploadDesc.data = data.data();
            uploadDesc.dataSize = data.size() * sizeof(uint32_t);
            uploadDesc.debugName = desc.debugName + "InitialUpload";
            return device->CreateBuffer(desc, uploadDesc);
        };

        auto forwardLocalLightBuffer = createStructuredBuffer("ForwardLocalLightBuffer", frameData.forwardLocalLightData);
        auto numCulledLightsGridBuffer = createStructuredBuffer("NumCulledLightsGrid", frameData.numCulledLightsGrid);
        auto culledLightDataGridBuffer = createStructuredBuffer("CulledLightDataGrid", frameData.culledLightDataGrid);
        if (constantsBuffer == nullptr ||
            forwardLocalLightBuffer == nullptr ||
            numCulledLightsGridBuffer == nullptr ||
            culledLightDataGridBuffer == nullptr)
        {
            LogLightGridHotPathFailure(m_driver, "LightGridPrepass::EnsureFallbackGraphicsPassBindingSet failed: one or more fallback buffers could not be created.");
            return false;
        }

        auto graphicsSetDesc = NLS::Render::Resources::BuildBindingSetDescFromShaderParameters(
            LightGridGraphicsParameters::Build(),
            m_graphicsBindingLayout,
            {
                NLS::Render::Resources::ShaderParameterBindingValue::UniformBuffer("ForwardLightData", constantsBuffer, sizeof(ForwardLightData)),
                NLS::Render::Resources::ShaderParameterBindingValue::StructuredBuffer("ForwardLocalLightBuffer", forwardLocalLightBuffer, frameData.forwardLocalLightData.size() * sizeof(uint32_t)),
                NLS::Render::Resources::ShaderParameterBindingValue::StructuredBuffer("NumCulledLightsGrid", numCulledLightsGridBuffer, frameData.numCulledLightsGrid.size() * sizeof(uint32_t)),
                NLS::Render::Resources::ShaderParameterBindingValue::StructuredBuffer("CulledLightDataGrid", culledLightDataGridBuffer, frameData.culledLightDataGrid.size() * sizeof(uint32_t))
            },
            "LightGridFallbackGraphicsBindingSet");
        m_fallbackGraphicsPassBindingSet = NLS::Render::Context::DriverRendererAccess::CreateExplicitBindingSet(
            m_driver,
            graphicsSetDesc,
            NLS::Render::RHI::DescriptorAllocationLifetime::Persistent);
        m_graphicsPassBindingSet = m_fallbackGraphicsPassBindingSet;
        m_computeDispatchInputs.clear();
        return m_graphicsPassBindingSet != nullptr;
    }

    void LightGridPrepass::StorePreparedResourceCache(const PreparedResourceCacheKey& key)
    {
        m_preparedResourceCache.valid =
            m_graphicsPassBindingSet != nullptr &&
            !m_computeDispatchInputs.empty();
        if (!m_preparedResourceCache.valid)
        {
            m_preparedResourceCache = {};
            return;
        }

        m_preparedResourceCache.key = key;
        m_preparedResourceCache.forwardLightData = m_frameScratch.forwardLightData;
        m_preparedResourceCache.forwardLightDataBuffer = FindBindingBuffer(
            m_graphicsPassBindingSet,
            "ForwardLightData",
            NLS::Render::RHI::BindingType::UniformBuffer,
            0u);
        m_preparedResourceCache.graphicsPassBindingSet = m_graphicsPassBindingSet;
        m_preparedResourceCache.computeDispatchInputs = m_computeDispatchInputs;
    }

    bool LightGridPrepass::AreSamePreparedResourceCacheKeys(
        const PreparedResourceCacheKey& lhs,
        const PreparedResourceCacheKey& rhs)
    {
        return lhs.renderWidth == rhs.renderWidth &&
            lhs.renderHeight == rhs.renderHeight &&
            lhs.nearPlane == rhs.nearPlane &&
            lhs.farPlane == rhs.farPlane &&
            AreSameLightGridSettings(lhs.settings, rhs.settings) &&
            lhs.hasSkyboxTexture == rhs.hasSkyboxTexture &&
            AreSameCapturedLights(lhs.lights, rhs.lights);
    }

    bool LightGridPrepass::AreSameForwardLightData(
        const ForwardLightData& lhs,
        const ForwardLightData& rhs)
    {
        return std::memcmp(&lhs, &rhs, sizeof(lhs)) == 0;
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
        NLS_PROFILE_SCOPE();
        const auto& lightGridPrepass = preparedComputeRequest.lightGridPrepass;
        const auto& preparedFrameInputs = preparedComputeRequest.preparedFrameInputs;
        if (lightGridPrepass == nullptr)
        {
            return {};
        }

        if (!preparedFrameInputs.has_value())
        {
            LogLightGridHotPathFailure(
                lightGridPrepass->m_driver,
                "LightGridPrepass::BuildPreparedComputeDispatchSource failed: prepared frame inputs are missing.");
            return {};
        }

        if (!lightGridPrepass->Prepare(preparedComputeRequest.frameDescriptor, preparedFrameInputs.value()))
        {
            LogLightGridHotPathFailure(
                lightGridPrepass->m_driver,
                "LightGridPrepass::BuildPreparedComputeDispatchSource failed: LightGridPrepass::Prepare returned false.");
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
        NLS_PROFILE_SCOPE();
        if (!NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::ShaderManager>())
        {
            LogLightGridHotPathFailure(m_driver, "LightGridPrepass requires ShaderManager to resolve engine shader assets.");
            return false;
        }

        auto& shaderManager = NLS_SERVICE(NLS::Core::ResourceManagement::ShaderManager);
        if (m_resetShader == nullptr)
            m_resetShader = shaderManager[":Shaders/LightGridReset.hlsl"];
        if (m_injectionShader == nullptr)
            m_injectionShader = shaderManager[":Shaders/LightGridInjection.hlsl"];
        if (m_compactShader == nullptr)
            m_compactShader = shaderManager[":Shaders/LightGridCompact.hlsl"];
        if (m_resetShader == nullptr)
            LogLightGridHotPathFailure(m_driver, "LightGridPrepass failed to load :Shaders/LightGridReset.hlsl.");
        if (m_injectionShader == nullptr)
            LogLightGridHotPathFailure(m_driver, "LightGridPrepass failed to load :Shaders/LightGridInjection.hlsl.");
        if (m_compactShader == nullptr)
            LogLightGridHotPathFailure(m_driver, "LightGridPrepass failed to load :Shaders/LightGridCompact.hlsl.");
        return m_resetShader != nullptr && m_injectionShader != nullptr && m_compactShader != nullptr;
    }

    bool LightGridPrepass::EnsurePipelines()
    {
        NLS_PROFILE_SCOPE();
        auto device = NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(m_driver);
        auto pipelineCache = NLS::Render::Context::DriverRendererAccess::GetPipelineCache(m_driver);
        if (device == nullptr || m_resetShader == nullptr || m_injectionShader == nullptr || m_compactShader == nullptr)
        {
            LogLightGridHotPathFailure(m_driver, "LightGridPrepass::EnsurePipelines failed: device or shader asset is null.");
            return false;
        }

        m_resetGlobalShader = {
            "LightGridResetCS",
            NLS::Render::ShaderCompiler::ShaderStage::Compute,
            m_resetShader,
            LightGridResetParameters::Build()
        };
        m_injectionGlobalShader = {
            "LightGridInjectionCS",
            NLS::Render::ShaderCompiler::ShaderStage::Compute,
            m_injectionShader,
            LightGridInjectionParameters::Build()
        };
        m_compactGlobalShader = {
            "LightGridCompactCS",
            NLS::Render::ShaderCompiler::ShaderStage::Compute,
            m_compactShader,
            LightGridCompactParameters::Build()
        };

        if (m_resetBindingLayout == nullptr)
        {
            m_resetBindingLayout = NLS::Render::Resources::ComputeShaderUtils::CreatePassBindingLayout(
                device,
                m_resetGlobalShader.parameters);
        }

        if (m_injectionBindingLayout == nullptr)
        {
            m_injectionBindingLayout = NLS::Render::Resources::ComputeShaderUtils::CreatePassBindingLayout(
                device,
                m_injectionGlobalShader.parameters);
        }

        if (m_compactBindingLayout == nullptr)
        {
            m_compactBindingLayout = NLS::Render::Resources::ComputeShaderUtils::CreatePassBindingLayout(
                device,
                m_compactGlobalShader.parameters);
        }

        if (!EnsureGraphicsBindingLayout())
            return false;

        if (m_resetPipelineLayout == nullptr)
        {
            m_resetPipelineLayout = NLS::Render::Resources::ComputeShaderUtils::CreatePipelineLayout(
                device,
                m_resetBindingLayout,
                "LightGridResetPipelineLayout");
        }

        if (m_injectionPipelineLayout == nullptr)
        {
            m_injectionPipelineLayout = NLS::Render::Resources::ComputeShaderUtils::CreatePipelineLayout(
                device,
                m_injectionBindingLayout,
                "LightGridInjectionPipelineLayout");
        }

        if (m_compactPipelineLayout == nullptr)
        {
            m_compactPipelineLayout = NLS::Render::Resources::ComputeShaderUtils::CreatePipelineLayout(
                device,
                m_compactBindingLayout,
                "LightGridCompactPipelineLayout");
        }

        m_resetPipeline = NLS::Render::Resources::ComputeShaderUtils::CreateComputePipeline(
            device,
            pipelineCache,
            m_resetGlobalShader,
            m_resetPipelineLayout,
            m_resetPipeline,
            m_resetPipelineKey,
            "LightGridResetPipeline");
        m_injectionPipeline = NLS::Render::Resources::ComputeShaderUtils::CreateComputePipeline(
            device,
            pipelineCache,
            m_injectionGlobalShader,
            m_injectionPipelineLayout,
            m_injectionPipeline,
            m_injectionPipelineKey,
            "LightGridInjectionPipeline");
        m_compactPipeline = NLS::Render::Resources::ComputeShaderUtils::CreateComputePipeline(
            device,
            pipelineCache,
            m_compactGlobalShader,
            m_compactPipelineLayout,
            m_compactPipeline,
            m_compactPipelineKey,
            "LightGridCompactPipeline");
        if (m_resetPipeline == nullptr)
            LogLightGridHotPathFailure(m_driver, "LightGridPrepass::EnsurePipelines failed: LightGridResetPipeline is null.");
        if (m_injectionPipeline == nullptr)
            LogLightGridHotPathFailure(m_driver, "LightGridPrepass::EnsurePipelines failed: LightGridInjectionPipeline is null.");
        if (m_compactPipeline == nullptr)
            LogLightGridHotPathFailure(m_driver, "LightGridPrepass::EnsurePipelines failed: LightGridCompactPipeline is null.");
        return m_resetPipeline != nullptr && m_injectionPipeline != nullptr && m_compactPipeline != nullptr;
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
            LogLightGridHotPathFailure(m_driver, "LightGridPrepass::BuildFrameData failed: frame descriptor camera is null.");
            return false;
        }

        outFrameData.forwardLightData.viewMatrix = NLS::Maths::Matrix4::Transpose(frameDescriptor.camera->GetViewMatrix());
        outFrameData.forwardLightData.projectionMatrix = NLS::Maths::Matrix4::Transpose(frameDescriptor.camera->GetProjectionMatrix());
        const auto viewProjection = frameDescriptor.camera->GetProjectionMatrix() * frameDescriptor.camera->GetViewMatrix();
        outFrameData.forwardLightData.inverseViewProjection = NLS::Maths::Matrix4::Transpose(NLS::Maths::Matrix4::Inverse(viewProjection));
        outFrameData.forwardLightData.clipToView = NLS::Maths::Matrix4::Transpose(
            NLS::Maths::Matrix4::Inverse(frameDescriptor.camera->GetProjectionMatrix()));
        outFrameData.forwardLightData.cameraWorldPositionNearPlane = {
            frameDescriptor.camera->GetPosition().x,
            frameDescriptor.camera->GetPosition().y,
            frameDescriptor.camera->GetPosition().z,
            frameDescriptor.camera->GetNear()
        };
        outFrameData.forwardLightData.renderSizeFarPlane = {
            static_cast<float>(frameDescriptor.renderWidth),
            static_cast<float>(frameDescriptor.renderHeight),
            1.0f / static_cast<float>(frameDescriptor.renderWidth == 0u ? 1u : frameDescriptor.renderWidth),
            frameDescriptor.camera->GetFar()
        };
        const auto gridDimensions = CalculateLightGridDimensions(
            m_settings,
            frameDescriptor.renderWidth,
            frameDescriptor.renderHeight);
        const auto zParams = CalculateLightGridZParams(
            frameDescriptor.camera->GetNear(),
            frameDescriptor.camera->GetFar() + 10.0f,
            gridDimensions.z);

        outFrameData.forwardLightData.gridParams = {
            static_cast<float>(gridDimensions.x),
            static_cast<float>(gridDimensions.y),
            static_cast<float>(gridDimensions.z),
            static_cast<float>(m_settings.maxLightsPerCluster)
        };
        outFrameData.forwardLightData.lightingParams = {
            static_cast<float>(preparedFrameInputs.lights.size()),
            0.15f,
            kDefaultAmbientFloor,
            hasSkyboxTexture ? 1.0f : 0.0f
        };
        outFrameData.forwardLightData.zParams = {
            zParams.x,
            zParams.y,
            zParams.z,
            m_settings.linkedListCulling ? 1.0f : 0.0f
        };
        outFrameData.forwardLightData.pixelParams = {
            static_cast<float>(m_settings.lightGridPixelSize),
            1.0f / static_cast<float>(frameDescriptor.renderHeight == 0u ? 1u : frameDescriptor.renderHeight),
            0.0f,
            0.0f
        };

        outFrameData.forwardLocalLightData.clear();
        outFrameData.forwardLocalLightData.reserve(preparedFrameInputs.lights.size() * kLightWordStride);
        for (const auto& light : preparedFrameInputs.lights)
            PackCapturedLight(light, outFrameData.forwardLocalLightData);

        const uint32_t clusterCount = gridDimensions.x * gridDimensions.y * gridDimensions.z;
        outFrameData.startOffsetGrid.resize(clusterCount);
        outFrameData.culledLightLinks.resize(clusterCount * m_settings.maxLightsPerCluster * NLS::Engine::Rendering::GetLightLinkStride());
        outFrameData.linkCounter.resize(1u);
        outFrameData.compactCounter.resize(1u);
        outFrameData.numCulledLightsGrid.resize(clusterCount * NLS::Engine::Rendering::GetNumCulledLightsGridStride());
        outFrameData.culledLightDataGrid.resize(clusterCount * m_settings.maxLightsPerCluster);
        return true;
    }
}
