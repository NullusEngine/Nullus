#include <gtest/gtest.h>

#include "Rendering/Assets/MaterialConversion.h"
#include "Rendering/Assets/SceneImportPipeline.h"
#include "Rendering/Assets/ShaderArtifact.h"
#include "Rendering/Assets/TextureArtifact.h"
#include "Rendering/Assets/TextureMipGenerator.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Resources/Loaders/MaterialLoader.h"
#include "Rendering/Resources/Loaders/ShaderLoader.h"
#include "Rendering/Resources/Loaders/TextureLoader.h"
#include "Rendering/RHI/BindingPointMap.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/MaterialResourceSet.h"
#include "Rendering/Resources/Texture2D.h"
#include "Rendering/RHI/Core/RHIBinding.h"
#include "Rendering/ShaderCompiler/ShaderCompilationTypes.h"
#include "Rendering/ShaderLab/ShaderLabParser.h"
#include "Rendering/ShaderLab/ShaderLabTypes.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Core/ResourceManagement/ShaderManager.h"
#include "Core/ResourceManagement/MaterialManager.h"
#include "Core/ResourceManagement/TextureManager.h"
#include "Core/ServiceLocator.h"
#include "Assets/ArtifactDatabase.h"
#include "Assets/NativeArtifactContainer.h"
#include "Debug/Logger.h"
#include "Guid.h"
#include "Image.h"
#include "Jobs/JobSystem.h"
#include "Math/Vector4.h"

#include <algorithm>
#include <any>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>

namespace
{
using NLS::Render::Assets::ConvertedMaterialArtifact;
using NLS::Render::Assets::MaterialAlphaMode;
using NLS::Render::Assets::MaterialSourceModel;
using NLS::Render::Assets::MaterialTextureColorSpace;

const NLS::Render::Assets::ConvertedMaterialTextureSlot* FindSlot(
    const ConvertedMaterialArtifact& material,
    const std::string& slot)
{
    const auto found = std::find_if(
        material.textureSlots.begin(),
        material.textureSlots.end(),
        [&slot](const NLS::Render::Assets::ConvertedMaterialTextureSlot& candidate)
        {
            return candidate.slot == slot;
        });
    return found != material.textureSlots.end() ? &*found : nullptr;
}

size_t CountSlots(
    const ConvertedMaterialArtifact& material,
    const std::string& slot)
{
    return static_cast<size_t>(std::count_if(
        material.textureSlots.begin(),
        material.textureSlots.end(),
        [&slot](const NLS::Render::Assets::ConvertedMaterialTextureSlot& candidate)
        {
            return candidate.slot == slot;
        }));
}

const NLS::Render::Assets::ConvertedMaterialFactor* FindFactor(
    const ConvertedMaterialArtifact& material,
    const std::string& name)
{
    const auto found = std::find_if(
        material.factors.begin(),
        material.factors.end(),
        [&name](const NLS::Render::Assets::ConvertedMaterialFactor& candidate)
        {
            return candidate.name == name;
        });
    return found != material.factors.end() ? &*found : nullptr;
}

bool HasDiagnosticCode(
    const ConvertedMaterialArtifact& material,
    const std::string& code)
{
    return std::any_of(
        material.diagnostics.begin(),
        material.diagnostics.end(),
        [&code](const NLS::Render::Assets::MaterialConversionDiagnostic& diagnostic)
        {
            return diagnostic.code == code;
        });
}

size_t CountDiagnosticCode(
    const ConvertedMaterialArtifact& material,
    const std::string& code)
{
    return static_cast<size_t>(std::count_if(
        material.diagnostics.begin(),
        material.diagnostics.end(),
        [&code](const NLS::Render::Assets::MaterialConversionDiagnostic& diagnostic)
        {
            return diagnostic.code == code;
        }));
}

struct TestVec3
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct TestTangentFrame
{
    TestVec3 tangent;
    TestVec3 bitangent;
    TestVec3 normal;
};

constexpr double kTestSafeNormalEpsilon = 1.0e-8;
constexpr double kTestSafeNormalMaxLengthSq = 1.0e20;
constexpr double kTestSafeNormalMaxComponent = 1.0e30;

TestVec3 Add(const TestVec3 a, const TestVec3 b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

TestVec3 Subtract(const TestVec3 a, const TestVec3 b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

TestVec3 Scale(const TestVec3 value, const double scale)
{
    return {value.x * scale, value.y * scale, value.z * scale};
}

double Dot(const TestVec3 a, const TestVec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

TestVec3 Cross(const TestVec3 a, const TestVec3 b)
{
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x};
}

bool TestIsFinite3(const TestVec3 value)
{
    return std::isfinite(value.x) &&
        std::isfinite(value.y) &&
        std::isfinite(value.z) &&
        std::fabs(value.x) < kTestSafeNormalMaxComponent &&
        std::fabs(value.y) < kTestSafeNormalMaxComponent &&
        std::fabs(value.z) < kTestSafeNormalMaxComponent;
}

TestVec3 TestNormalizeFallback(const TestVec3 fallback)
{
    const double lengthSq = Dot(fallback, fallback);
    if (TestIsFinite3(fallback) &&
        lengthSq > kTestSafeNormalEpsilon &&
        lengthSq < kTestSafeNormalMaxLengthSq)
    {
        return Scale(fallback, 1.0 / std::sqrt(lengthSq));
    }

    return {0.0, 0.0, 1.0};
}

TestVec3 TestSafeNormalize(const TestVec3 value, const TestVec3 fallback)
{
    const double lengthSq = Dot(value, value);
    if (TestIsFinite3(value) &&
        lengthSq > kTestSafeNormalEpsilon &&
        lengthSq < kTestSafeNormalMaxLengthSq)
    {
        return Scale(value, 1.0 / std::sqrt(lengthSq));
    }

    return TestNormalizeFallback(fallback);
}

TestVec3 TestSafePerpendicular(const TestVec3 normal)
{
    const TestVec3 safeNormal = TestSafeNormalize(normal, {0.0, 0.0, 1.0});
    const TestVec3 reference = std::fabs(safeNormal.z) < 0.999
        ? TestVec3{0.0, 0.0, 1.0}
        : TestVec3{0.0, 1.0, 0.0};
    return TestSafeNormalize(Cross(reference, safeNormal), {1.0, 0.0, 0.0});
}

TestTangentFrame TestBuildSafeTangentFrame(
    const TestVec3 normal,
    const TestVec3 tangent,
    const TestVec3 bitangent)
{
    TestTangentFrame frame;
    frame.normal = TestSafeNormalize(normal, {0.0, 0.0, 1.0});

    const TestVec3 tangentCandidate = Subtract(tangent, Scale(frame.normal, Dot(tangent, frame.normal)));
    const double tangentLengthSq = Dot(tangentCandidate, tangentCandidate);
    if (TestIsFinite3(tangentCandidate) &&
        tangentLengthSq > kTestSafeNormalEpsilon &&
        tangentLengthSq < kTestSafeNormalMaxLengthSq)
    {
        frame.tangent = Scale(tangentCandidate, 1.0 / std::sqrt(tangentLengthSq));
    }
    else
    {
        frame.tangent = TestSafePerpendicular(frame.normal);
    }

    const TestVec3 bitangentCandidate = Subtract(
        Subtract(bitangent, Scale(frame.normal, Dot(bitangent, frame.normal))),
        Scale(frame.tangent, Dot(bitangent, frame.tangent)));
    const double bitangentLengthSq = Dot(bitangentCandidate, bitangentCandidate);
    if (TestIsFinite3(bitangentCandidate) &&
        bitangentLengthSq > kTestSafeNormalEpsilon &&
        bitangentLengthSq < kTestSafeNormalMaxLengthSq)
    {
        frame.bitangent = Scale(bitangentCandidate, 1.0 / std::sqrt(bitangentLengthSq));
    }
    else
    {
        frame.bitangent = TestNormalizeFallback(Cross(frame.normal, frame.tangent));
    }

    return frame;
}

TestVec3 TestApplyTangentNormal(const TestVec3 tangentNormal, const TestTangentFrame& frame)
{
    const TestVec3 mapped = Add(
        Add(Scale(frame.tangent, tangentNormal.x), Scale(frame.bitangent, tangentNormal.y)),
        Scale(frame.normal, tangentNormal.z));
    return TestSafeNormalize(mapped, frame.normal);
}

uint32_t NextTextureTestMipDimension(const uint32_t value)
{
    return value > 1u ? value / 2u : 1u;
}

NLS::Render::Assets::TextureArtifactData MakeDescriptorBackedTextureArtifact(
    const NLS::Render::RHI::TextureFormat format,
    const NLS::Render::Assets::TextureArtifactColorSpace colorSpace,
    const uint32_t width,
    const uint32_t height,
    const uint32_t mipCount)
{
    NLS::Render::Assets::TextureArtifactData artifact;
    artifact.width = width;
    artifact.height = height;
    artifact.format = format;
    artifact.colorSpace = colorSpace;

    uint32_t mipWidth = width;
    uint32_t mipHeight = height;
    for (uint32_t level = 0u; level < mipCount; ++level)
    {
        NLS::Render::Assets::TextureArtifactMip mip;
        mip.level = level;
        mip.width = mipWidth;
        mip.height = mipHeight;
        mip.rowPitch = NLS::Render::RHI::CalculateTextureRowPitch(format, mipWidth);
        mip.slicePitch = NLS::Render::RHI::CalculateTextureSlicePitch(format, mipWidth, mipHeight, 1u);
        mip.pixels.resize(mip.slicePitch);
        for (size_t byteIndex = 0u; byteIndex < mip.pixels.size(); ++byteIndex)
            mip.pixels[byteIndex] = static_cast<uint8_t>((byteIndex + level * 17u) & 0xFFu);

        artifact.mips.push_back(std::move(mip));
        mipWidth = NextTextureTestMipDimension(mipWidth);
        mipHeight = NextTextureTestMipDimension(mipHeight);
    }

    return artifact;
}

class ScopedDriverService final
{
public:
    explicit ScopedDriverService(NLS::Render::Context::Driver& driver)
    {
        NLS::Core::ServiceLocator::Provide(driver);
    }

    ~ScopedDriverService()
    {
        NLS::Core::ServiceLocator::Remove<NLS::Render::Context::Driver>();
    }

    ScopedDriverService(const ScopedDriverService&) = delete;
    ScopedDriverService& operator=(const ScopedDriverService&) = delete;
};

class ScopedNoDriverService final
{
public:
    ScopedNoDriverService()
    {
        if (NLS::Core::ServiceLocator::Contains<NLS::Render::Context::Driver>())
            m_previousDriver = &NLS::Core::ServiceLocator::Get<NLS::Render::Context::Driver>();
        NLS::Core::ServiceLocator::Remove<NLS::Render::Context::Driver>();
    }

    ~ScopedNoDriverService()
    {
        NLS::Core::ServiceLocator::Remove<NLS::Render::Context::Driver>();
        if (m_previousDriver != nullptr)
            NLS::Core::ServiceLocator::Provide(*m_previousDriver);
    }

    ScopedNoDriverService(const ScopedNoDriverService&) = delete;
    ScopedNoDriverService& operator=(const ScopedNoDriverService&) = delete;

private:
    NLS::Render::Context::Driver* m_previousDriver = nullptr;
};

class ScopedMaterialConversionJobSystem final
{
public:
    explicit ScopedMaterialConversionJobSystem(const uint32_t backgroundWorkerCount = 1u)
    {
        NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::Immediate);
#if defined(NLS_ENABLE_TEST_HOOKS)
        NLS::Base::Jobs::ResetJobSystemForTesting();
#endif

        NLS::Base::Jobs::JobSystemConfig config;
        config.workerCount = 0u;
        config.backgroundWorkerCount = backgroundWorkerCount;
        m_initialized = NLS::Base::Jobs::InitializeJobSystem(config);
    }

    ~ScopedMaterialConversionJobSystem()
    {
        NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::Immediate);
#if defined(NLS_ENABLE_TEST_HOOKS)
        NLS::Base::Jobs::ResetJobSystemForTesting();
#endif
    }

    [[nodiscard]] bool IsInitialized() const
    {
        return m_initialized;
    }

    ScopedMaterialConversionJobSystem(const ScopedMaterialConversionJobSystem&) = delete;
    ScopedMaterialConversionJobSystem& operator=(const ScopedMaterialConversionJobSystem&) = delete;

private:
    bool m_initialized = false;
};

class TextureLoaderTestAdapter final : public NLS::Render::RHI::RHIAdapter
{
public:
    std::string_view GetDebugName() const override { return "AssetMaterialConversionTextureLoaderAdapter"; }
    NLS::Render::RHI::NativeBackendType GetBackendType() const override { return NLS::Render::RHI::NativeBackendType::DX12; }
    std::string_view GetVendor() const override { return "TestVendor"; }
    std::string_view GetHardware() const override { return "TestHardware"; }
};

class TextureLoaderTestTexture final : public NLS::Render::RHI::RHITexture
{
public:
    explicit TextureLoaderTestTexture(NLS::Render::RHI::RHITextureDesc desc)
        : m_desc(std::move(desc))
    {
    }

    std::string_view GetDebugName() const override { return m_desc.debugName; }
    const NLS::Render::RHI::RHITextureDesc& GetDesc() const override { return m_desc; }
    NLS::Render::RHI::ResourceState GetState() const override { return NLS::Render::RHI::ResourceState::Unknown; }

private:
    NLS::Render::RHI::RHITextureDesc m_desc {};
};

class TextureLoaderTestTextureView final : public NLS::Render::RHI::RHITextureView
{
public:
    TextureLoaderTestTextureView(
        std::shared_ptr<NLS::Render::RHI::RHITexture> texture,
        NLS::Render::RHI::RHITextureViewDesc desc)
        : m_texture(std::move(texture))
        , m_desc(std::move(desc))
    {
    }

    std::string_view GetDebugName() const override { return m_desc.debugName; }
    const NLS::Render::RHI::RHITextureViewDesc& GetDesc() const override { return m_desc; }
    const std::shared_ptr<NLS::Render::RHI::RHITexture>& GetTexture() const override { return m_texture; }

private:
    std::shared_ptr<NLS::Render::RHI::RHITexture> m_texture;
    NLS::Render::RHI::RHITextureViewDesc m_desc {};
};

class TextureLoaderTestBindingLayout final : public NLS::Render::RHI::RHIBindingLayout
{
public:
    explicit TextureLoaderTestBindingLayout(NLS::Render::RHI::RHIBindingLayoutDesc desc)
        : m_desc(std::move(desc))
    {
    }

    std::string_view GetDebugName() const override { return m_desc.debugName; }
    const NLS::Render::RHI::RHIBindingLayoutDesc& GetDesc() const override { return m_desc; }

private:
    NLS::Render::RHI::RHIBindingLayoutDesc m_desc {};
};

class TextureLoaderTestBindingSet final : public NLS::Render::RHI::RHIBindingSet
{
public:
    explicit TextureLoaderTestBindingSet(NLS::Render::RHI::RHIBindingSetDesc desc)
        : m_desc(std::move(desc))
    {
    }

    std::string_view GetDebugName() const override { return m_desc.debugName; }
    const NLS::Render::RHI::RHIBindingSetDesc& GetDesc() const override { return m_desc; }

private:
    NLS::Render::RHI::RHIBindingSetDesc m_desc {};
};

class TextureLoaderTestCommandBuffer final : public NLS::Render::RHI::RHICommandBuffer
{
public:
    explicit TextureLoaderTestCommandBuffer(std::string debugName)
        : m_debugName(std::move(debugName))
    {
    }

    std::string_view GetDebugName() const override { return m_debugName; }
    void Begin() override { m_recording = true; m_closed = false; }
    void End() override { m_recording = false; m_closed = true; }
    void Reset() override { m_recording = false; m_closed = false; }
    bool IsRecording() const override { return m_recording; }
    bool IsClosedForSubmission() const override { return m_closed; }
    NLS::Render::RHI::NativeHandle GetNativeCommandBuffer() const override { return {}; }
    void BeginRenderPass(const NLS::Render::RHI::RHIRenderPassDesc&) override {}
    void EndRenderPass() override {}
    void SetViewport(const NLS::Render::RHI::RHIViewport&) override {}
    void SetScissor(const NLS::Render::RHI::RHIRect2D&) override {}
    void BindGraphicsPipeline(const std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline>&) override {}
    void BindComputePipeline(const std::shared_ptr<NLS::Render::RHI::RHIComputePipeline>&) override {}
    void BindBindingSet(uint32_t, const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>&) override {}
    void PushConstants(NLS::Render::RHI::ShaderStageMask, uint32_t, uint32_t, const void*) override {}
    void BindVertexBuffer(uint32_t, const NLS::Render::RHI::RHIVertexBufferView&) override {}
    void BindIndexBuffer(const NLS::Render::RHI::RHIIndexBufferView&) override {}
    void Draw(uint32_t, uint32_t, uint32_t, uint32_t) override {}
    void DrawIndexed(uint32_t, uint32_t, uint32_t, int32_t, uint32_t) override {}
    void Dispatch(uint32_t, uint32_t, uint32_t) override {}
    void CopyBuffer(
        const std::shared_ptr<NLS::Render::RHI::RHIBuffer>&,
        const std::shared_ptr<NLS::Render::RHI::RHIBuffer>&,
        const NLS::Render::RHI::RHIBufferCopyRegion&) override {}
    void CopyBufferToTexture(const NLS::Render::RHI::RHIBufferToTextureCopyDesc&) override {}
    void CopyTexture(const NLS::Render::RHI::RHITextureCopyDesc&) override {}
    void Barrier(const NLS::Render::RHI::RHIBarrierDesc&) override {}

private:
    std::string m_debugName;
    bool m_recording = false;
    bool m_closed = false;
};

class TextureLoaderTestCommandPool final : public NLS::Render::RHI::RHICommandPool
{
public:
    TextureLoaderTestCommandPool(NLS::Render::RHI::QueueType queueType, std::string debugName)
        : m_queueType(queueType)
        , m_debugName(std::move(debugName))
    {
    }

    std::string_view GetDebugName() const override { return m_debugName; }
    NLS::Render::RHI::QueueType GetQueueType() const override { return m_queueType; }
    std::shared_ptr<NLS::Render::RHI::RHICommandBuffer> CreateCommandBuffer(std::string debugName = {}) override
    {
        return std::make_shared<TextureLoaderTestCommandBuffer>(std::move(debugName));
    }
    void Reset() override {}

private:
    NLS::Render::RHI::QueueType m_queueType = NLS::Render::RHI::QueueType::Graphics;
    std::string m_debugName;
};

class TextureLoaderTestFence final : public NLS::Render::RHI::RHIFence
{
public:
    explicit TextureLoaderTestFence(std::string debugName)
        : m_debugName(std::move(debugName))
    {
    }

    std::string_view GetDebugName() const override { return m_debugName; }
    bool IsSignaled() const override { return m_signaled; }
    void Reset() override { m_signaled = false; }
    bool Wait(uint64_t = 0u) override
    {
        m_signaled = true;
        return true;
    }

private:
    std::string m_debugName;
    bool m_signaled = true;
};

class TextureLoaderTestSemaphore final : public NLS::Render::RHI::RHISemaphore
{
public:
    explicit TextureLoaderTestSemaphore(std::string debugName)
        : m_debugName(std::move(debugName))
    {
    }

    std::string_view GetDebugName() const override { return m_debugName; }
    bool IsSignaled() const override { return false; }
    void Reset() override {}

private:
    std::string m_debugName;
};

class TextureLoaderTestDevice final : public NLS::Render::RHI::RHIDevice
{
public:
    TextureLoaderTestDevice()
        : m_adapter(std::make_shared<TextureLoaderTestAdapter>())
    {
        m_nativeDeviceInfo.backend = NLS::Render::RHI::NativeBackendType::DX12;
        m_capabilities.backendReady = true;
        m_capabilities.supportsGraphics = true;
        m_capabilities.supportsCurrentSceneRenderer = true;
        for (const auto& descriptor : NLS::Render::RHI::kTextureFormatDescriptors)
        {
            m_capabilities.SetTextureFormatCapability(
                descriptor.format,
                {
                    descriptor.format,
                    descriptor.sampled,
                    descriptor.supportsUpload,
                    descriptor.colorAttachment,
                    descriptor.storage,
                    descriptor.supportsSrgbView,
                    descriptor.requiresAlignedTopLevelBlocks,
                    true,
                    {}
                });
        }
    }

    std::string_view GetDebugName() const override { return "AssetMaterialConversionTextureLoaderDevice"; }
    const std::shared_ptr<NLS::Render::RHI::RHIAdapter>& GetAdapter() const override { return m_adapter; }
    const NLS::Render::RHI::RHIDeviceCapabilities& GetCapabilities() const override { return m_capabilities; }
    NLS::Render::RHI::NativeRenderDeviceInfo GetNativeDeviceInfo() const override { return m_nativeDeviceInfo; }
    bool IsBackendReady() const override { return true; }
    std::shared_ptr<NLS::Render::RHI::RHIQueue> GetQueue(NLS::Render::RHI::QueueType) override { return nullptr; }
    std::shared_ptr<NLS::Render::RHI::RHISwapchain> CreateSwapchain(const NLS::Render::RHI::SwapchainDesc&) override { return nullptr; }
    std::shared_ptr<NLS::Render::RHI::RHIBuffer> CreateBuffer(
        const NLS::Render::RHI::RHIBufferDesc&,
        const NLS::Render::RHI::RHIBufferUploadDesc&) override
    {
        return nullptr;
    }
    std::shared_ptr<NLS::Render::RHI::RHITexture> CreateTexture(
        const NLS::Render::RHI::RHITextureDesc& desc,
        const NLS::Render::RHI::RHITextureUploadDesc& uploadDesc) override
    {
        ++textureCreateCalls;
        lastTextureDesc = desc;
        lastTextureUploadDesc = uploadDesc;
        return std::make_shared<TextureLoaderTestTexture>(desc);
    }
    std::shared_ptr<NLS::Render::RHI::RHITextureView> CreateTextureView(
        const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
        const NLS::Render::RHI::RHITextureViewDesc& desc) override
    {
        lastTextureViewDesc = desc;
        return std::make_shared<TextureLoaderTestTextureView>(texture, desc);
    }
    std::shared_ptr<NLS::Render::RHI::RHISampler> CreateSampler(const NLS::Render::RHI::SamplerDesc&, std::string = {}) override { return nullptr; }
    std::shared_ptr<NLS::Render::RHI::RHIBindingLayout> CreateBindingLayout(const NLS::Render::RHI::RHIBindingLayoutDesc& desc) override
    {
        ++bindingLayoutCreateCalls;
        return std::make_shared<TextureLoaderTestBindingLayout>(desc);
    }
    std::shared_ptr<NLS::Render::RHI::RHIBindingSet> CreateBindingSet(const NLS::Render::RHI::RHIBindingSetDesc& desc) override
    {
        ++bindingSetCreateCalls;
        lastBindingSetDesc = desc;
        return std::make_shared<TextureLoaderTestBindingSet>(desc);
    }
    std::shared_ptr<NLS::Render::RHI::RHIPipelineLayout> CreatePipelineLayout(const NLS::Render::RHI::RHIPipelineLayoutDesc&) override { return nullptr; }
    std::shared_ptr<NLS::Render::RHI::RHIShaderModule> CreateShaderModule(const NLS::Render::RHI::RHIShaderModuleDesc&) override { return nullptr; }
    std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline> CreateGraphicsPipeline(const NLS::Render::RHI::RHIGraphicsPipelineDesc&) override { return nullptr; }
    std::shared_ptr<NLS::Render::RHI::RHIComputePipeline> CreateComputePipeline(const NLS::Render::RHI::RHIComputePipelineDesc&) override { return nullptr; }
    std::shared_ptr<NLS::Render::RHI::RHICommandPool> CreateCommandPool(
        NLS::Render::RHI::QueueType queueType,
        std::string debugName = {}) override
    {
        return std::make_shared<TextureLoaderTestCommandPool>(queueType, std::move(debugName));
    }
    std::shared_ptr<NLS::Render::RHI::RHIFence> CreateFence(std::string debugName = {}) override
    {
        return std::make_shared<TextureLoaderTestFence>(std::move(debugName));
    }
    std::shared_ptr<NLS::Render::RHI::RHISemaphore> CreateSemaphore(std::string debugName = {}) override
    {
        return std::make_shared<TextureLoaderTestSemaphore>(std::move(debugName));
    }
    void ReadPixels(
        const std::shared_ptr<NLS::Render::RHI::RHITexture>&,
        uint32_t,
        uint32_t,
        uint32_t,
        uint32_t,
        NLS::Render::Settings::EPixelDataFormat,
        NLS::Render::Settings::EPixelDataType,
        void*) override {}

    size_t textureCreateCalls = 0u;
    size_t bindingLayoutCreateCalls = 0u;
    size_t bindingSetCreateCalls = 0u;
    NLS::Render::RHI::RHITextureDesc lastTextureDesc {};
    NLS::Render::RHI::RHITextureUploadDesc lastTextureUploadDesc {};
    NLS::Render::RHI::RHITextureViewDesc lastTextureViewDesc {};
    NLS::Render::RHI::RHIBindingSetDesc lastBindingSetDesc {};

private:
    std::shared_ptr<NLS::Render::RHI::RHIAdapter> m_adapter;
    NLS::Render::RHI::NativeRenderDeviceInfo m_nativeDeviceInfo {};
    NLS::Render::RHI::RHIDeviceCapabilities m_capabilities {};
};

class ScopedShaderManagerAssetPaths final
{
public:
    ScopedShaderManagerAssetPaths(
        const std::string& projectAssetsPath,
        const std::string& engineAssetsPath)
    {
        NLS::Core::ResourceManagement::ShaderManager::ProvideAssetPaths(
            projectAssetsPath,
            engineAssetsPath);
    }

    ~ScopedShaderManagerAssetPaths()
    {
        NLS::Core::ResourceManagement::ShaderManager::ProvideAssetPaths({}, {});
        NLS::Render::Resources::Loaders::ShaderLoader::SetDefaultProjectAssetsPath({});
    }

    ScopedShaderManagerAssetPaths(const ScopedShaderManagerAssetPaths&) = delete;
    ScopedShaderManagerAssetPaths& operator=(const ScopedShaderManagerAssetPaths&) = delete;
};

void WriteBinaryFile(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
}

void WriteTextFile(const std::filesystem::path& path, const std::string& contents)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << contents;
}

void WriteNativeArtifactTextFile(
    const std::filesystem::path& path,
    const NLS::Core::Assets::ArtifactType artifactType,
    const std::string& schemaName,
    const uint32_t schemaVersion,
    const std::string& contents)
{
    NLS::Core::Assets::NativeArtifactMetadata metadata;
    metadata.artifactType = artifactType;
    metadata.schemaName = schemaName;
    metadata.schemaVersion = schemaVersion;

    const auto payload = std::vector<uint8_t>(contents.begin(), contents.end());
    WriteBinaryFile(path, NLS::Core::Assets::WriteNativeArtifactContainer(std::move(metadata), payload));
}

void WriteNativeMaterialArtifactFile(
    const std::filesystem::path& path,
    const std::string& contents)
{
    WriteNativeArtifactTextFile(
        path,
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        1u,
        contents);
}

void WriteMinimalShaderLabMaterialArtifactFile(
    const std::filesystem::path& path,
    const std::string& shaderPath = "Assets/Shaders/AsyncMaterial.shader")
{
    WriteNativeMaterialArtifactFile(
        path,
        "shaderLabMaterialVersion=1\n"
        "shader=" + shaderPath + "\n"
        "surfaceMode=Opaque\n"
        "alphaMode=Opaque\n"
        "doubleSided=false\n"
        "depthWrite=true\n");
}

std::filesystem::path MaterialArtifactPath(
    const std::filesystem::path& root,
    const std::string& hash = "9ecdfef170d27a0c3492458106af879cd7920bf5392f8461ca7d01b0ced0b2c6")
{
    return root / "Library" / "Artifacts" / NLS::Core::Assets::BuildArtifactStorageRelativePath(hash);
}

std::string IndexedArtifactHash(const std::string_view prefix, const size_t index)
{
    return NLS::Core::Assets::BuildArtifactStorageFileName(
        std::string(prefix) + ":" + std::to_string(index));
}

std::filesystem::path RuntimeMaterialArtifactPath(
    const std::filesystem::path& root,
    const std::string& hash)
{
    return root / "Data" / "Artifacts" / NLS::Core::Assets::BuildArtifactStorageRelativePath(hash);
}

std::string LibraryArtifactPath(const std::string& hash)
{
    return (std::filesystem::path("Library") / "Artifacts" /
        NLS::Core::Assets::BuildArtifactStorageRelativePath(hash)).generic_string();
}

std::string RuntimeArtifactPath(const std::string& hash)
{
    return (std::filesystem::path("Artifacts") /
        NLS::Core::Assets::BuildArtifactStorageRelativePath(hash)).generic_string();
}

std::filesystem::path ArtifactPath(const std::filesystem::path& root, const std::string& hash)
{
    return root / LibraryArtifactPath(hash);
}

std::filesystem::path TextureArtifactPathForBytes(
    const std::filesystem::path& root,
    const std::vector<uint8_t>& bytes)
{
    const auto hash = NLS::Core::Assets::BuildArtifactStorageFileName(bytes.data(), bytes.size());
    return ArtifactPath(root, hash);
}

std::filesystem::path WriteTextureArtifactBytes(
    const std::filesystem::path& root,
    const std::vector<uint8_t>& bytes)
{
    const auto artifactPath = TextureArtifactPathForBytes(root, bytes);
    WriteBinaryFile(artifactPath, bytes);
    return artifactPath;
}

std::string ReadNativeArtifactPayloadText(
    const std::filesystem::path& path,
    const NLS::Core::Assets::ArtifactType artifactType,
    const uint32_t schemaVersion)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    const std::vector<uint8_t> bytes{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    const auto container = NLS::Core::Assets::ReadNativeArtifactContainer(bytes, artifactType, schemaVersion);
    if (!container.has_value())
        return {};
    return std::string(container->payload.begin(), container->payload.end());
}

NLS::Render::Resources::ShaderReflection MakeAlbedoMapShaderReflection()
{
    NLS::Render::Resources::ShaderReflection reflection;
    reflection.properties = {
        {
            "_BaseMap",
            NLS::Render::Resources::UniformType::UNIFORM_SAMPLER_2D,
            NLS::Render::Resources::ShaderResourceKind::SampledTexture,
            NLS::Render::ShaderCompiler::ShaderStage::Pixel,
            NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace,
            0u,
            -1,
            1,
            0u,
            0u,
            {}
        }
    };
    return reflection;
}

NLS::Render::Resources::ShaderReflection MakeBaseMapWithSamplerShaderReflection()
{
    auto reflection = MakeAlbedoMapShaderReflection();
    reflection.properties.push_back({
        "sampler_BaseMap",
        NLS::Render::Resources::UniformType::UNIFORM_SAMPLER_2D,
        NLS::Render::Resources::ShaderResourceKind::Sampler,
        NLS::Render::ShaderCompiler::ShaderStage::Pixel,
        NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace,
        0u,
        -1,
        1,
        0u,
        0u,
        {}
    });
    return reflection;
}

NLS::Render::Resources::ShaderReflection MakeStandardPbrShaderReflection()
{
    auto reflection = MakeAlbedoMapShaderReflection();
    reflection.constantBuffers.push_back({
        "MaterialConstants",
        NLS::Render::ShaderCompiler::ShaderStage::Pixel,
        NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace,
        0u,
        32u,
        {
            {
                "u_Albedo",
                NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC4,
                0u,
                16u,
                1u
            },
            {
                "u_Metallic",
                NLS::Render::Resources::UniformType::UNIFORM_FLOAT,
                16u,
                4u,
                1u
            },
            {
                "u_Roughness",
                NLS::Render::Resources::UniformType::UNIFORM_FLOAT,
                20u,
                4u,
                1u
            }
        }
    });
    reflection.properties.push_back({
        "u_Albedo",
        NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC4,
        NLS::Render::Resources::ShaderResourceKind::Value,
        NLS::Render::ShaderCompiler::ShaderStage::Pixel,
        NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace,
        0u,
        -1,
        1,
        0u,
        16u,
        "MaterialConstants"
    });
    reflection.properties.push_back({
        "u_Metallic",
        NLS::Render::Resources::UniformType::UNIFORM_FLOAT,
        NLS::Render::Resources::ShaderResourceKind::Value,
        NLS::Render::ShaderCompiler::ShaderStage::Pixel,
        NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace,
        0u,
        -1,
        1,
        16u,
        4u,
        "MaterialConstants"
    });
    reflection.properties.push_back({
        "u_Roughness",
        NLS::Render::Resources::UniformType::UNIFORM_FLOAT,
        NLS::Render::Resources::ShaderResourceKind::Value,
        NLS::Render::ShaderCompiler::ShaderStage::Pixel,
        NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace,
        0u,
        -1,
        1,
        20u,
        4u,
        "MaterialConstants"
    });
    reflection.properties.push_back({
        "_NormalMap",
        NLS::Render::Resources::UniformType::UNIFORM_SAMPLER_2D,
        NLS::Render::Resources::ShaderResourceKind::SampledTexture,
        NLS::Render::ShaderCompiler::ShaderStage::Pixel,
        NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace,
        1u,
        -1,
        1,
        0u,
        0u,
        {}
    });
    reflection.properties.push_back({
        "_MetallicMap",
        NLS::Render::Resources::UniformType::UNIFORM_SAMPLER_2D,
        NLS::Render::Resources::ShaderResourceKind::SampledTexture,
        NLS::Render::ShaderCompiler::ShaderStage::Pixel,
        NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace,
        2u,
        -1,
        1,
        0u,
        0u,
        {}
    });
    reflection.properties.push_back({
        "_RoughnessMap",
        NLS::Render::Resources::UniformType::UNIFORM_SAMPLER_2D,
        NLS::Render::Resources::ShaderResourceKind::SampledTexture,
        NLS::Render::ShaderCompiler::ShaderStage::Pixel,
        NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace,
        3u,
        -1,
        1,
        0u,
        0u,
        {}
    });
    return reflection;
}

NLS::Render::Resources::ShaderReflection MakeShaderLabNamedMaterialReflection()
{
    NLS::Render::Resources::ShaderReflection reflection;
    reflection.constantBuffers.push_back({
        "MaterialProperties",
        NLS::Render::ShaderCompiler::ShaderStage::Pixel,
        NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace,
        0u,
        32u,
        {
            {
                "_BaseColor",
                NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC4,
                0u,
                16u,
                1u
            },
            {
                "_Metallic",
                NLS::Render::Resources::UniformType::UNIFORM_FLOAT,
                16u,
                4u,
                1u
            },
            {
                "_Roughness",
                NLS::Render::Resources::UniformType::UNIFORM_FLOAT,
                20u,
                4u,
                1u
            }
        }
    });
    reflection.properties.push_back({
        "_BaseColor",
        NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC4,
        NLS::Render::Resources::ShaderResourceKind::Value,
        NLS::Render::ShaderCompiler::ShaderStage::Pixel,
        NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace,
        0u,
        -1,
        1,
        0u,
        16u,
        "MaterialProperties"
    });
    reflection.properties.push_back({
        "_Metallic",
        NLS::Render::Resources::UniformType::UNIFORM_FLOAT,
        NLS::Render::Resources::ShaderResourceKind::Value,
        NLS::Render::ShaderCompiler::ShaderStage::Pixel,
        NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace,
        0u,
        -1,
        1,
        16u,
        4u,
        "MaterialProperties"
    });
    reflection.properties.push_back({
        "_Roughness",
        NLS::Render::Resources::UniformType::UNIFORM_FLOAT,
        NLS::Render::Resources::ShaderResourceKind::Value,
        NLS::Render::ShaderCompiler::ShaderStage::Pixel,
        NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace,
        0u,
        -1,
        1,
        20u,
        4u,
        "MaterialProperties"
    });
    reflection.properties.push_back({
        "_BaseMap",
        NLS::Render::Resources::UniformType::UNIFORM_SAMPLER_2D,
        NLS::Render::Resources::ShaderResourceKind::SampledTexture,
        NLS::Render::ShaderCompiler::ShaderStage::Pixel,
        NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace,
        1u,
        -1,
        1,
        0u,
        0u,
        {}
    });
    return reflection;
}

NLS::Render::Assets::ShaderArtifact MakeShaderArtifact(
    std::string sourcePath,
    std::string subAssetKey,
    NLS::Render::Resources::ShaderReflection reflection)
{
    NLS::Render::Assets::ShaderArtifact artifact;
    artifact.sourcePath = std::move(sourcePath);
    artifact.subAssetKey = std::move(subAssetKey);
    artifact.reflection = std::move(reflection);
    artifact.stages.push_back({
        NLS::Render::ShaderCompiler::ShaderStage::Vertex,
        NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL,
        "VSMain",
        "vs_6_0",
        {
            NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded,
            {1u, 2u, 3u, 4u},
            {},
            {},
            "test-vertex",
            LibraryArtifactPath("2b40aa9d7e26302abaee46d90172b24a111dda5b6d466fcf2e7a2aff001a0607")
        }
    });
    artifact.stages.push_back({
        NLS::Render::ShaderCompiler::ShaderStage::Pixel,
        NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL,
        "PSMain",
        "ps_6_0",
        {
            NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded,
            {5u, 6u, 7u, 8u},
            {},
            {},
            "test-pixel",
            LibraryArtifactPath("2b40aa9d7e26302abaee46d90172b24a111dda5b6d466fcf2e7a2aff001a0607")
        }
    });
    return artifact;
}

NLS::Render::Assets::ShaderArtifact MakeAlbedoMapShaderArtifact()
{
    return MakeShaderArtifact(
        "App/Assets/Engine/Shaders/StandardPBR.hlsl",
        "shader:StandardPBR",
        MakeAlbedoMapShaderReflection());
}

NLS::Render::Assets::ShaderArtifact MakeBaseMapWithSamplerShaderArtifact()
{
    return MakeShaderArtifact(
        "App/Assets/Engine/Shaders/StandardPBR.hlsl",
        "shader:StandardPBR",
        MakeBaseMapWithSamplerShaderReflection());
}

NLS::Render::Assets::ShaderArtifact MakeStandardPbrShaderArtifact()
{
    return MakeShaderArtifact(
        "App/Assets/Engine/Shaders/StandardPBR.hlsl",
        "shader:StandardPBR",
        MakeStandardPbrShaderReflection());
}

NLS::Render::ShaderLab::ShaderLabPassState MakeTransparentShaderLabPassState()
{
    NLS::Render::ShaderLab::ShaderLabPassState state;
    state.cullMode = NLS::Render::ShaderLab::ShaderLabCullMode::Front;
    state.depthWrite = false;
    state.depthCompare = NLS::Render::Settings::EComparaisonAlgorithm::ALWAYS;
    state.blend.enabled = true;
    state.blend.colorWrite = true;
    state.blend.renderTargets.resize(1u);
    state.blend.renderTargets[0].blendEnable = true;
    state.blend.renderTargets[0].srcColor = NLS::Render::RHI::RHIBlendFactor::SrcAlpha;
    state.blend.renderTargets[0].dstColor = NLS::Render::RHI::RHIBlendFactor::InvSrcAlpha;
    state.blend.renderTargets[0].srcAlpha = NLS::Render::RHI::RHIBlendFactor::One;
    state.blend.renderTargets[0].dstAlpha = NLS::Render::RHI::RHIBlendFactor::Zero;
    return state;
}

std::filesystem::path WriteStandardPbrShaderArtifact(const std::filesystem::path& root)
{
    const auto shaderArtifactPath = ArtifactPath(root, "2b40aa9d7e26302abaee46d90172b24a111dda5b6d466fcf2e7a2aff001a0607");
    WriteBinaryFile(shaderArtifactPath, NLS::Render::Assets::SerializeShaderArtifact(MakeStandardPbrShaderArtifact()));
    return shaderArtifactPath;
}

std::filesystem::path WriteShaderLabSourceBackedShaderArtifact(
    const std::filesystem::path& root,
    const std::filesystem::path& sourcePath,
    const std::string& artifactHash,
    NLS::Render::Assets::ShaderArtifact artifact,
    const std::string& displayName = "ShaderLab Forward")
{
    const auto artifactPath = ArtifactPath(root, artifactHash);
    artifact.sourcePath = sourcePath.generic_string();
    if (artifact.subAssetKey.empty())
        artifact.subAssetKey = "shader:" + sourcePath.stem().generic_string() + "/Forward#0";
    if (artifact.shaderLabLightMode.empty())
        artifact.shaderLabLightMode = "Forward";
    if (!artifact.shaderLabPassState.has_value())
    {
        NLS::Render::ShaderLab::ShaderLabPassState passState;
        passState.depthWrite = true;
        passState.depthCompare = NLS::Render::Settings::EComparaisonAlgorithm::LESS_EQUAL;
        artifact.shaderLabPassState = passState;
    }
    WriteBinaryFile(artifactPath, NLS::Render::Assets::SerializeShaderArtifact(artifact));

    NLS::Core::Assets::ArtifactManifest manifest;
    manifest.sourceAssetId = NLS::Core::Assets::AssetId(NLS::Guid::NewDeterministic(sourcePath.generic_string()));
    manifest.importerId = "ShaderLabImporter";
    manifest.importerVersion = 1u;
    manifest.targetPlatform = "editor";
    manifest.primarySubAssetKey = artifact.subAssetKey;
    manifest.subAssets.push_back({
        manifest.sourceAssetId,
        artifact.subAssetKey,
        NLS::Core::Assets::ArtifactType::Shader,
        "ShaderLoader",
        "editor",
        LibraryArtifactPath(artifactHash),
        artifactHash,
        displayName
    });

    NLS::Core::Assets::ArtifactDatabase database;
    database.Load(root / "Library" / "ArtifactDB");
    database.UpsertManifest(
        manifest,
        sourcePath.generic_string(),
        NLS::Core::Assets::ArtifactRecordStatus::UpToDate);
    EXPECT_TRUE(database.Save(root / "Library" / "ArtifactDB"));
    return sourcePath;
}

std::filesystem::path WriteShaderLabSourceBackedStandardPbrShaderArtifact(const std::filesystem::path& root)
{
    auto artifact = MakeStandardPbrShaderArtifact();
    artifact.subAssetKey = "shader:StandardPBR/Forward#0";
    return WriteShaderLabSourceBackedShaderArtifact(
        root,
        std::filesystem::path("Assets") / "Shaders" / "StandardPBR.shader",
        "2b40aa9d7e26302abaee46d90172b24a111dda5b6d466fcf2e7a2aff001a0607",
        std::move(artifact),
        "StandardPBR Forward");
}

std::vector<uint8_t> TinyPng()
{
    return {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
        0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x04, 0x00, 0x00, 0x00, 0xB5, 0x1C, 0x0C,
        0x02, 0x00, 0x00, 0x00, 0x0B, 0x49, 0x44, 0x41,
        0x54, 0x78, 0xDA, 0x63, 0xFC, 0xFF, 0x1F, 0x00,
        0x03, 0x03, 0x02, 0x00, 0xEF, 0xBF, 0x4A, 0x3B,
        0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44,
        0xAE, 0x42, 0x60, 0x82
    };
}

std::vector<uint8_t> TinyRgb16Png()
{
    return {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
        0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x10, 0x02, 0x00, 0x00, 0x00, 0xC0, 0xE7, 0x8F,
        0x9D, 0x00, 0x00, 0x00, 0x0F, 0x49, 0x44, 0x41,
        0x54, 0x78, 0x9C, 0x63, 0x10, 0x32, 0x09, 0xAB,
        0x98, 0xB5, 0x07, 0x00, 0x06, 0x27, 0x02, 0x6B,
        0x0E, 0xDE, 0xD5, 0x7A, 0x00, 0x00, 0x00, 0x00,
        0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82
    };
}

void AppendUInt32Le(std::vector<uint8_t>& bytes, const uint32_t value)
{
    bytes.push_back(static_cast<uint8_t>(value & 0xFFu));
    bytes.push_back(static_cast<uint8_t>((value >> 8u) & 0xFFu));
    bytes.push_back(static_cast<uint8_t>((value >> 16u) & 0xFFu));
    bytes.push_back(static_cast<uint8_t>((value >> 24u) & 0xFFu));
}

void AppendUInt64Le(std::vector<uint8_t>& bytes, const uint64_t value)
{
    for (uint32_t byteIndex = 0u; byteIndex < 8u; ++byteIndex)
        bytes.push_back(static_cast<uint8_t>((value >> (byteIndex * 8u)) & 0xFFu));
}

std::vector<uint8_t> MakeLegacyTextureArtifactPayloadV2()
{
    std::vector<uint8_t> bytes;
    bytes.reserve(32u + 2u * 36u + 20u);

    constexpr uint32_t kMagic = 0x5845544Eu;
    constexpr uint32_t kVersion = 2u;
    AppendUInt32Le(bytes, kMagic);
    AppendUInt32Le(bytes, kVersion);
    AppendUInt32Le(bytes, 2u);
    AppendUInt32Le(bytes, 2u);
    AppendUInt32Le(bytes, static_cast<uint32_t>(NLS::Render::RHI::TextureFormat::RGBA8));
    AppendUInt32Le(bytes, static_cast<uint32_t>(NLS::Render::Assets::TextureArtifactColorSpace::Srgb));
    AppendUInt32Le(bytes, 2u);
    AppendUInt32Le(bytes, 0u);

    AppendUInt32Le(bytes, 0u);
    AppendUInt32Le(bytes, 2u);
    AppendUInt32Le(bytes, 2u);
    AppendUInt32Le(bytes, 8u);
    AppendUInt32Le(bytes, 16u);
    AppendUInt64Le(bytes, 104u);
    AppendUInt64Le(bytes, 16u);

    AppendUInt32Le(bytes, 1u);
    AppendUInt32Le(bytes, 1u);
    AppendUInt32Le(bytes, 1u);
    AppendUInt32Le(bytes, 4u);
    AppendUInt32Le(bytes, 4u);
    AppendUInt64Le(bytes, 120u);
    AppendUInt64Le(bytes, 4u);

    const std::vector<uint8_t> basePixels{
        255u, 0u, 0u, 255u,
        0u, 255u, 0u, 255u,
        0u, 0u, 255u, 255u,
        255u, 255u, 255u, 255u
    };
    const std::vector<uint8_t> mipPixels{128u, 128u, 128u, 255u};
    bytes.insert(bytes.end(), basePixels.begin(), basePixels.end());
    bytes.insert(bytes.end(), mipPixels.begin(), mipPixels.end());
    return bytes;
}

std::vector<uint8_t> EncodeNormalVector(float x, float y, float z)
{
    const auto encode = [](float component)
    {
        const float normalized = std::clamp(component * 0.5f + 0.5f, 0.0f, 1.0f);
        return static_cast<uint8_t>(std::lround(normalized * 255.0f));
    };

    return {encode(x), encode(y), encode(z), 255u};
}

std::array<float, 3> DecodeNormalVector(const std::vector<uint8_t>& rgba)
{
    const auto decode = [](uint8_t component)
    {
        return (static_cast<float>(component) / 255.0f) * 2.0f - 1.0f;
    };

    return {decode(rgba[0]), decode(rgba[1]), decode(rgba[2])};
}

uint16_t FloatToHalfBitsForTest(const float value)
{
    uint32_t bits = 0u;
    std::memcpy(&bits, &value, sizeof(float));

    const uint32_t sign = (bits >> 16u) & 0x8000u;
    int32_t exponent = static_cast<int32_t>((bits >> 23u) & 0xFFu) - 127 + 15;
    uint32_t mantissa = bits & 0x7FFFFFu;

    if (exponent <= 0)
    {
        if (exponent < -10)
            return static_cast<uint16_t>(sign);
        mantissa = (mantissa | 0x800000u) >> static_cast<uint32_t>(1 - exponent);
        return static_cast<uint16_t>(sign | ((mantissa + 0x1000u) >> 13u));
    }

    if (exponent >= 31)
        return static_cast<uint16_t>(sign | 0x7C00u);

    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exponent) << 10u) | ((mantissa + 0x1000u) >> 13u));
}

float HalfBitsToFloatForTest(const uint16_t half)
{
    const uint32_t sign = static_cast<uint32_t>(half & 0x8000u) << 16u;
    int32_t exponent = static_cast<int32_t>((half >> 10u) & 0x1Fu);
    uint32_t mantissa = half & 0x03FFu;

    uint32_t bits = 0u;
    if (exponent == 0)
    {
        if (mantissa == 0u)
        {
            bits = sign;
        }
        else
        {
            exponent = 1u;
            while ((mantissa & 0x0400u) == 0u)
            {
                mantissa <<= 1u;
                --exponent;
            }
            mantissa &= 0x03FFu;
            bits = sign | (static_cast<uint32_t>(exponent + (127 - 15)) << 23u) | (mantissa << 13u);
        }
    }
    else if (exponent == 31)
    {
        bits = sign | 0x7F800000u | (mantissa << 13u);
    }
    else
    {
        bits = sign | (static_cast<uint32_t>(exponent + (127 - 15)) << 23u) | (mantissa << 13u);
    }

    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(float));
    return value;
}

void AppendHalfFloatForTest(std::vector<uint8_t>& bytes, const float value)
{
    const uint16_t half = FloatToHalfBitsForTest(value);
    bytes.push_back(static_cast<uint8_t>(half & 0xFFu));
    bytes.push_back(static_cast<uint8_t>((half >> 8u) & 0xFFu));
}

float ReadHalfFloatForTest(const std::vector<uint8_t>& bytes, const size_t halfIndex)
{
    const size_t byteIndex = halfIndex * 2u;
    const uint16_t half = static_cast<uint16_t>(bytes[byteIndex]) |
        (static_cast<uint16_t>(bytes[byteIndex + 1u]) << 8u);
    return HalfBitsToFloatForTest(half);
}
}

TEST(AssetMaterialConversionTests, GltfPbrConversionMapsTextureSlotsFactorsSamplerAndAlpha)
{
    const std::string gltf = R"(
    {
      "asset": { "version": "2.0" },
      "samplers": [
        { "wrapS": 33071, "wrapT": 10497, "minFilter": 9987, "magFilter": 9729 }
      ],
      "images": [
        { "uri": "BaseColor.png", "name": "BaseColor" },
        { "uri": "MetalRough.png", "name": "MetalRough" },
        { "uri": "Normal.png", "name": "Normal" },
        { "uri": "Occlusion.png", "name": "Occlusion" },
        { "uri": "Emissive.png", "name": "Emissive" }
      ],
      "textures": [
        { "source": 0, "sampler": 0 },
        { "source": 1, "sampler": 0 },
        { "source": 2, "sampler": 0 },
        { "source": 3, "sampler": 0 },
        { "source": 4, "sampler": 0 }
      ],
      "materials": [
        {
          "name": "HeroMaterial",
          "doubleSided": true,
          "alphaMode": "BLEND",
          "alphaCutoff": 0.4,
          "emissiveFactor": [0.1, 0.2, 0.3],
          "pbrMetallicRoughness": {
            "baseColorFactor": [0.8, 0.7, 0.6, 0.5],
            "metallicFactor": 0.25,
            "roughnessFactor": 0.75,
            "baseColorTexture": { "index": 0 },
            "metallicRoughnessTexture": { "index": 1 }
          },
          "normalTexture": { "index": 2, "scale": 0.6 },
          "occlusionTexture": { "index": 3, "strength": 0.8 },
          "emissiveTexture": { "index": 4 }
        }
      ]
    })";

    const auto scene = NLS::Render::Assets::ImportGltfSceneJson(
        gltf,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("e1010101-0101-4101-8101-010101010101")),
        "Hero");
    ASSERT_EQ(scene.materials.size(), 1u);

    const auto material = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        scene.materials.front(),
        MaterialSourceModel::GltfPbrMetallicRoughness);

    EXPECT_EQ(material.subAssetKey, "material:material/0");
    EXPECT_EQ(material.workflow, "metallic-roughness");
    EXPECT_TRUE(material.doubleSided);
    EXPECT_EQ(material.alphaMode, MaterialAlphaMode::Blend);
    EXPECT_DOUBLE_EQ(material.alphaCutoff, 0.4);

    const auto* baseColor = FindSlot(material, "BaseColor");
    ASSERT_NE(baseColor, nullptr);
    EXPECT_EQ(baseColor->textureKey, "image/0");
    EXPECT_EQ(baseColor->textureResourcePath, "BaseColor.png");
    EXPECT_EQ(baseColor->colorSpace, MaterialTextureColorSpace::SRgb);
    EXPECT_EQ(baseColor->sampler.wrapS, "ClampToEdge");
    EXPECT_EQ(baseColor->sampler.wrapT, "Repeat");
    EXPECT_EQ(baseColor->sampler.minFilter, "LinearMipmapLinear");

    const auto* metalRough = FindSlot(material, "MetallicRoughness");
    ASSERT_NE(metalRough, nullptr);
    EXPECT_EQ(metalRough->colorSpace, MaterialTextureColorSpace::Linear);
    EXPECT_NE(FindSlot(material, "Normal"), nullptr);
    EXPECT_NE(FindSlot(material, "Occlusion"), nullptr);
    EXPECT_NE(FindSlot(material, "Emissive"), nullptr);

    const auto* baseFactor = FindFactor(material, "BaseColor");
    ASSERT_NE(baseFactor, nullptr);
    ASSERT_EQ(baseFactor->values.size(), 4u);
    EXPECT_DOUBLE_EQ(baseFactor->values[0], 0.8);
    EXPECT_DOUBLE_EQ(baseFactor->values[3], 0.5);
    EXPECT_DOUBLE_EQ(FindFactor(material, "Metallic")->scalar, 0.25);
    EXPECT_DOUBLE_EQ(FindFactor(material, "Roughness")->scalar, 0.75);
    EXPECT_DOUBLE_EQ(FindFactor(material, "NormalScale")->scalar, 0.6);
    EXPECT_DOUBLE_EQ(FindFactor(material, "OcclusionStrength")->scalar, 0.8);
    EXPECT_FALSE(material.serializedPayload.empty());
    EXPECT_NE(
        material.serializedPayload.find("property _MetallicMapChannel Vector 0.000000 0.000000 1.000000 0.000000"),
        std::string::npos)
        << "glTF metallic-roughness textures store metallic in the blue channel.";
    EXPECT_NE(
        material.serializedPayload.find("property _RoughnessMapChannel Vector 0.000000 1.000000 0.000000 0.000000"),
        std::string::npos)
        << "glTF metallic-roughness textures store roughness in the green channel.";
}

TEST(AssetMaterialConversionTests, ConvertedStandardPbrPropertiesExistInBuiltInShaderLabAsset)
{
    const std::string gltf = R"(
    {
      "asset": { "version": "2.0" },
      "images": [
        { "uri": "BaseColor.png", "name": "BaseColor" },
        { "uri": "MetalRough.png", "name": "MetalRough" },
        { "uri": "Normal.png", "name": "Normal" },
        { "uri": "Occlusion.png", "name": "Occlusion" },
        { "uri": "Opacity.png", "name": "Opacity" },
        { "uri": "Emissive.png", "name": "Emissive" },
        { "uri": "Specular.png", "name": "Specular" }
      ],
      "textures": [
        { "source": 0 },
        { "source": 1 },
        { "source": 2 },
        { "source": 3 },
        { "source": 4 },
        { "source": 5 },
        { "source": 6 }
      ],
      "materials": [
        {
          "name": "HeroMaterial",
          "alphaMode": "MASK",
          "alphaCutoff": 0.4,
          "emissiveFactor": [0.1, 0.2, 0.3],
          "pbrMetallicRoughness": {
            "baseColorFactor": [0.8, 0.7, 0.6, 0.5],
            "metallicFactor": 0.25,
            "roughnessFactor": 0.75,
            "baseColorTexture": { "index": 0 },
            "metallicRoughnessTexture": { "index": 1 }
          },
          "normalTexture": { "index": 2, "scale": 0.6 },
          "occlusionTexture": { "index": 3, "strength": 0.8 }
        }
      ]
    })";

    const auto scene = NLS::Render::Assets::ImportGltfSceneJson(
        gltf,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("e1010101-0101-4101-8101-010101010102")),
        "Hero");
    ASSERT_EQ(scene.materials.size(), 1u);

    const auto material = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        scene.materials.front(),
        MaterialSourceModel::GltfPbrMetallicRoughness);

    const auto shaderPath =
        std::filesystem::path(NLS_ROOT_DIR) /
        "App" / "Assets" / "Engine" / "Shaders" / "ShaderLab" / "StandardPBR.shader";
    std::ifstream shaderStream(shaderPath, std::ios::binary);
    ASSERT_TRUE(shaderStream) << shaderPath.string();
    std::ostringstream shaderBuffer;
    shaderBuffer << shaderStream.rdbuf();
    const auto shaderSource = shaderBuffer.str();
    const auto parsed = NLS::Render::ShaderLab::ParseShaderLabSource(shaderSource, shaderPath.generic_string());
    ASSERT_TRUE(parsed.Succeeded()) << parsed.DiagnosticsToString();
    EXPECT_NE(shaderSource.find("dot(_MetallicMap.Sample(sampler_MetallicMap, input.uv), _MetallicMapChannel)"), std::string::npos);
    EXPECT_NE(shaderSource.find("dot(_RoughnessMap.Sample(sampler_RoughnessMap, input.uv), _RoughnessMapChannel)"), std::string::npos);

    std::unordered_set<std::string> shaderPropertyNames;
    for (const auto& property : parsed.asset.properties)
        shaderPropertyNames.insert(property.name);

    std::istringstream payload(material.serializedPayload);
    std::string line;
    while (std::getline(payload, line))
    {
        if (line.rfind("property ", 0u) != 0u)
            continue;
        std::istringstream propertyLine(line);
        std::string marker;
        std::string propertyName;
        propertyLine >> marker >> propertyName;
        EXPECT_NE(shaderPropertyNames.find(propertyName), shaderPropertyNames.end()) << propertyName;
    }
}

TEST(AssetMaterialConversionTests, GltfTextureUrisSerializeAsRuntimeResourcePaths)
{
    const std::string gltf = R"(
    {
      "asset": { "version": "2.0" },
      "images": [
        { "uri": "textures/BaseColor.png", "name": "BaseColor" }
      ],
      "textures": [
        { "source": 0 }
      ],
      "materials": [
        {
          "name": "HeroMaterial",
          "pbrMetallicRoughness": {
            "baseColorTexture": { "index": 0 }
          }
        }
      ]
    })";

    const auto scene = NLS::Render::Assets::ImportGltfSceneJson(
        gltf,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("e1020101-0101-4101-8101-010101010101")),
        "Hero");
    ASSERT_EQ(scene.materials.size(), 1u);

    const auto material = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        scene.materials.front(),
        MaterialSourceModel::GltfPbrMetallicRoughness,
        {std::filesystem::path("Models/Hero")});

    const auto* baseColor = FindSlot(material, "BaseColor");
    ASSERT_NE(baseColor, nullptr);
    EXPECT_EQ(baseColor->textureKey, "image/0");
    EXPECT_EQ(baseColor->textureResourcePath, "Models/Hero/textures/BaseColor.png");
    EXPECT_NE(material.serializedPayload.find("shaderLabMaterialVersion=1"), std::string::npos);
    EXPECT_NE(
        material.serializedPayload.find("property _BaseMap Texture2D Models/Hero/textures/BaseColor.png"),
        std::string::npos);
    EXPECT_NE(material.serializedPayload.find("resourcePath=Models/Hero/textures/BaseColor.png"), std::string::npos);
    EXPECT_EQ(material.serializedPayload.find("value=\"image/0\""), std::string::npos);
}

TEST(AssetMaterialConversionTests, DeferredTextureSourceKeysDoNotSerializeSourceUrisAsRuntimeResourcePaths)
{
    const std::string gltf = R"(
    {
      "asset": { "version": "2.0" },
      "images": [
        { "uri": "textures/BulkAlbedo0.png", "name": "BaseColor" }
      ],
      "textures": [
        { "source": 0 }
      ],
      "materials": [
        {
          "name": "HeroMaterial",
          "pbrMetallicRoughness": {
            "baseColorTexture": { "index": 0 }
          }
        }
      ]
    })";

    const auto scene = NLS::Render::Assets::ImportGltfSceneJson(
        gltf,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("e1020101-0101-4101-8101-010101010103")),
        "Hero");
    ASSERT_EQ(scene.materials.size(), 1u);

    NLS::Render::Assets::MaterialConversionContext context;
    context.texturePathPrefix = std::filesystem::path("Models/Hero");
    context.deferredTextureSourceKeys.insert("image/0");
    const auto material = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        scene.materials.front(),
        MaterialSourceModel::GltfPbrMetallicRoughness,
        context);

    const auto* baseColor = FindSlot(material, "BaseColor");
    ASSERT_NE(baseColor, nullptr);
    EXPECT_EQ(baseColor->textureKey, "image/0");
    EXPECT_TRUE(baseColor->textureResourcePath.empty());
    EXPECT_EQ(baseColor->colorSpace, MaterialTextureColorSpace::SRgb);
    EXPECT_EQ(material.serializedPayload.find("property _BaseMap Texture2D"), std::string::npos);
    EXPECT_NE(material.serializedPayload.find("textureSlot _BaseMap texture=image/0 resourcePath="), std::string::npos);
    EXPECT_EQ(material.serializedPayload.find("BulkAlbedo"), std::string::npos);
}

TEST(AssetMaterialConversionTests, EmbeddedOrUnnamedGltfImagesDoNotSerializeVirtualImageKeysAsDiskPaths)
{
    const std::string gltf = R"(
    {
      "asset": { "version": "2.0" },
      "images": [
        { "name": "EmbeddedBaseColor" }
      ],
      "textures": [
        { "source": 0 }
      ],
      "materials": [
        {
          "name": "EmbeddedMaterial",
          "pbrMetallicRoughness": {
            "baseColorTexture": { "index": 0 }
          }
        }
      ]
    })";

    const auto scene = NLS::Render::Assets::ImportGltfSceneJson(
        gltf,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("e1020202-0202-4202-8202-020202020202")),
        "Hero");
    ASSERT_EQ(scene.materials.size(), 1u);

    const auto material = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        scene.materials.front(),
        MaterialSourceModel::GltfPbrMetallicRoughness,
        {std::filesystem::path("Models/Hero")});

    const auto* baseColor = FindSlot(material, "BaseColor");
    ASSERT_NE(baseColor, nullptr);
    EXPECT_EQ(baseColor->textureKey, "image/0");
    EXPECT_TRUE(baseColor->textureResourcePath.empty());
    EXPECT_EQ(material.serializedPayload.find("value=\"image/0\""), std::string::npos);
    EXPECT_EQ(material.serializedPayload.find("resourcePath=\"image/0\""), std::string::npos);
}

TEST(AssetMaterialConversionTests, ConvertedMaterialPayloadUsesShaderLabOnlySchema)
{
    const std::string gltf = R"(
    {
      "asset": { "version": "2.0" },
      "materials": [
        {
          "name": "HeroMaterial",
          "doubleSided": true,
          "alphaMode": "BLEND",
          "alphaCutoff": 0.4,
          "pbrMetallicRoughness": {
            "baseColorFactor": [0.8, 0.7, 0.6, 0.5],
            "metallicFactor": 0.25,
            "roughnessFactor": 0.75
          }
        }
      ]
    })";

    const auto scene = NLS::Render::Assets::ImportGltfSceneJson(
        gltf,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("e1110101-0101-4101-8101-010101010101")),
        "Hero");
    ASSERT_EQ(scene.materials.size(), 1u);

    const auto material = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        scene.materials.front(),
        MaterialSourceModel::GltfPbrMetallicRoughness);

    EXPECT_NE(material.serializedPayload.find("shaderLabMaterialVersion=1"), std::string::npos);
    EXPECT_NE(
        material.serializedPayload.find("shader=Assets/Engine/Shaders/ShaderLab/StandardPBR.shader"),
        std::string::npos);
    EXPECT_NE(material.serializedPayload.find("surfaceMode=Transparent"), std::string::npos);
    EXPECT_NE(material.serializedPayload.find("alphaMode=Blend"), std::string::npos);
    EXPECT_NE(material.serializedPayload.find("doubleSided=true"), std::string::npos);
    EXPECT_NE(material.serializedPayload.find("depthWrite=false"), std::string::npos);
    EXPECT_NE(material.serializedPayload.find("property _BaseColor Color 0.800000 0.700000 0.600000 0.500000"), std::string::npos);
    EXPECT_NE(material.serializedPayload.find("property _Metallic Float 0.250000"), std::string::npos);
    EXPECT_NE(material.serializedPayload.find("property _Roughness Float 0.750000"), std::string::npos);
    EXPECT_NE(material.serializedPayload.find("property _Cutoff Float 0.400000"), std::string::npos);
    EXPECT_EQ(material.serializedPayload.find("<root>"), std::string::npos);
    EXPECT_EQ(material.serializedPayload.find("u_Albedo"), std::string::npos);
    EXPECT_EQ(material.serializedPayload.find("MATERIAL="), std::string::npos);
    EXPECT_EQ(material.serializedPayload.find("TEXTURE_SLOT="), std::string::npos);
}

TEST(AssetMaterialConversionTests, ConvertedMaterialPayloadUsesShaderLabMaterialSchema)
{
    const std::string gltf = R"(
    {
      "asset": { "version": "2.0" },
      "materials": [
        {
          "name": "HeroMaterial",
          "alphaMode": "MASK",
          "alphaCutoff": 0.35,
          "pbrMetallicRoughness": {
            "baseColorFactor": [0.25, 0.5, 0.75, 0.8],
            "metallicFactor": 0.4,
            "roughnessFactor": 0.6
          }
        }
      ]
    })";

    const auto scene = NLS::Render::Assets::ImportGltfSceneJson(
        gltf,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("e1110606-0606-4606-8606-060606060606")),
        "Hero");
    ASSERT_EQ(scene.materials.size(), 1u);

    const auto material = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        scene.materials.front(),
        MaterialSourceModel::GltfPbrMetallicRoughness);

    EXPECT_NE(material.serializedPayload.find("shaderLabMaterialVersion=1"), std::string::npos);
    EXPECT_NE(
        material.serializedPayload.find("shader=Assets/Engine/Shaders/ShaderLab/StandardPBR.shader"),
        std::string::npos);
    EXPECT_NE(material.serializedPayload.find("property _BaseColor Color 0.250000 0.500000 0.750000 0.800000"), std::string::npos);
    EXPECT_NE(material.serializedPayload.find("property _Metallic Float 0.400000"), std::string::npos);
    EXPECT_NE(material.serializedPayload.find("property _Roughness Float 0.600000"), std::string::npos);
    EXPECT_NE(material.serializedPayload.find("keyword _ALPHATEST_ON"), std::string::npos);
    EXPECT_NE(material.serializedPayload.find("property _Cutoff Float 0.350000"), std::string::npos);
    EXPECT_EQ(material.serializedPayload.find("<root>"), std::string::npos);
    EXPECT_EQ(material.serializedPayload.find("u_Albedo"), std::string::npos);
}

TEST(AssetMaterialConversionTests, GltfBlendMaterialWithDecalNameSerializesAsDecalSurface)
{
    const std::string gltf = R"(
    {
      "asset": { "version": "2.0" },
      "materials": [
        {
          "name": "dirt_decal",
          "alphaMode": "BLEND",
          "pbrMetallicRoughness": {
            "baseColorFactor": [1.0, 1.0, 1.0, 0.65]
          }
        }
      ]
    })";

    const auto scene = NLS::Render::Assets::ImportGltfSceneJson(
        gltf,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("e1110303-0303-4303-8303-030303030303")),
        "DecalHero");
    ASSERT_EQ(scene.materials.size(), 1u);

    const auto material = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        scene.materials.front(),
        MaterialSourceModel::GltfPbrMetallicRoughness);

    EXPECT_EQ(material.alphaMode, MaterialAlphaMode::Blend);
    EXPECT_NE(material.serializedPayload.find("name=dirt_decal"), std::string::npos);
    EXPECT_NE(material.serializedPayload.find("surfaceMode=Decal"), std::string::npos);
    EXPECT_NE(material.serializedPayload.find("alphaMode=Blend"), std::string::npos);
    EXPECT_NE(material.serializedPayload.find("depthWrite=false"), std::string::npos);
    EXPECT_EQ(material.serializedPayload.find("surfaceMode=Transparent"), std::string::npos);
}

TEST(AssetMaterialConversionTests, GltfBlendMaterialWithCamelCaseDecalNameSerializesAsDecalSurface)
{
    const std::string gltf = R"(
    {
      "asset": { "version": "2.0" },
      "materials": [
        {
          "name": "DirtDecal",
          "alphaMode": "BLEND",
          "pbrMetallicRoughness": {
            "baseColorFactor": [1.0, 1.0, 1.0, 0.5]
          }
        }
      ]
    })";

    const auto scene = NLS::Render::Assets::ImportGltfSceneJson(
        gltf,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("e1110505-0505-4505-8505-050505050505")),
        "CamelDecalHero");
    ASSERT_EQ(scene.materials.size(), 1u);

    const auto material = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        scene.materials.front(),
        MaterialSourceModel::GltfPbrMetallicRoughness);

    EXPECT_EQ(material.alphaMode, MaterialAlphaMode::Blend);
    EXPECT_NE(material.serializedPayload.find("surfaceMode=Decal"), std::string::npos);
    EXPECT_EQ(material.serializedPayload.find("surfaceMode=Transparent"), std::string::npos);
}

TEST(AssetMaterialConversionTests, GltfBlendMaterialWithAcronymDecalNameSerializesAsDecalSurface)
{
    const std::string gltf = R"(
    {
      "asset": { "version": "2.0" },
      "materials": [
        {
          "name": "UIDecal",
          "alphaMode": "BLEND",
          "pbrMetallicRoughness": {
            "baseColorFactor": [1.0, 1.0, 1.0, 0.5]
          }
        }
      ]
    })";

    const auto scene = NLS::Render::Assets::ImportGltfSceneJson(
        gltf,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("e1110606-0606-4606-8606-060606060606")),
        "AcronymDecalHero");
    ASSERT_EQ(scene.materials.size(), 1u);

    const auto material = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        scene.materials.front(),
        MaterialSourceModel::GltfPbrMetallicRoughness);

    EXPECT_EQ(material.alphaMode, MaterialAlphaMode::Blend);
    EXPECT_NE(material.serializedPayload.find("surfaceMode=Decal"), std::string::npos);
    EXPECT_EQ(material.serializedPayload.find("surfaceMode=Transparent"), std::string::npos);
}

TEST(AssetMaterialConversionTests, GltfBlendMaterialWithoutDecalTokenStaysTransparent)
{
    const std::string gltf = R"(
    {
      "asset": { "version": "2.0" },
      "materials": [
        {
          "name": "DecalogueGlass",
          "alphaMode": "BLEND",
          "pbrMetallicRoughness": {
            "baseColorFactor": [1.0, 1.0, 1.0, 0.35]
          }
        }
      ]
    })";

    const auto scene = NLS::Render::Assets::ImportGltfSceneJson(
        gltf,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("e1110404-0404-4404-8404-040404040404")),
        "TransparentHero");
    ASSERT_EQ(scene.materials.size(), 1u);

    const auto material = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        scene.materials.front(),
        MaterialSourceModel::GltfPbrMetallicRoughness);

    EXPECT_EQ(material.alphaMode, MaterialAlphaMode::Blend);
    EXPECT_NE(material.serializedPayload.find("surfaceMode=Transparent"), std::string::npos);
    EXPECT_EQ(material.serializedPayload.find("surfaceMode=Decal"), std::string::npos);
    EXPECT_NE(material.serializedPayload.find("alphaMode=Blend"), std::string::npos);
}

TEST(AssetMaterialConversionTests, MaskAlphaModeDoesNotAdvertiseUnimplementedAlphaTestSurfaceMode)
{
    const std::string gltf = R"(
    {
      "asset": { "version": "2.0" },
      "materials": [
        {
          "name": "MaskedMaterial",
          "alphaMode": "MASK",
          "alphaCutoff": 0.35,
          "pbrMetallicRoughness": {
            "baseColorFactor": [0.9, 0.8, 0.7, 0.6]
          }
        }
      ]
    })";

    const auto scene = NLS::Render::Assets::ImportGltfSceneJson(
        gltf,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("e1110202-0202-4202-8202-020202020202")),
        "Hero");
    ASSERT_EQ(scene.materials.size(), 1u);

    const auto material = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        scene.materials.front(),
        MaterialSourceModel::GltfPbrMetallicRoughness);

    EXPECT_EQ(material.alphaMode, MaterialAlphaMode::Mask);
    EXPECT_DOUBLE_EQ(material.alphaCutoff, 0.35);
    EXPECT_NE(material.serializedPayload.find("alphaMode=Mask"), std::string::npos);
    EXPECT_NE(material.serializedPayload.find("property _Cutoff Float 0.350000"), std::string::npos);
    EXPECT_NE(material.serializedPayload.find("surfaceMode=Opaque"), std::string::npos);
    EXPECT_EQ(material.serializedPayload.find("surfaceMode=AlphaTest"), std::string::npos);
}

TEST(AssetMaterialConversionTests, MaterialLoaderAppliesExplicitDecalSurfaceMode)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_decal_material_" + NLS::Guid::New().ToString());
    const auto shaderArtifactPath = WriteStandardPbrShaderArtifact(root);
    const auto shaderSourcePath = WriteShaderLabSourceBackedStandardPbrShaderArtifact(root);
    static NLS::Core::ResourceManagement::ShaderManager shaderManager;
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::ShaderManager>(shaderManager);
    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(shaderArtifactPath.string());
    ASSERT_NE(shader, nullptr);
    shaderManager.RegisterResource(shaderArtifactPath.string(), shader);

    const auto materialPath = MaterialArtifactPath(root, "a90fd6f4632b901599c09ad982289d73ae8e35fded0786f396c5c3b48864cce8");
    WriteNativeArtifactTextFile(
        materialPath,
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        1u,
        "shaderLabMaterialVersion=1\n"
        "shader=" + shaderSourcePath.generic_string() + "\n"
        "surfaceMode=Decal\n"
        "alphaMode=Blend\n"
        "doubleSided=true\n"
        "depthWrite=false\n");

    auto* loaded = NLS::Render::Resources::Loaders::MaterialLoader::Create(materialPath.string());
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->GetSurfaceMode(), NLS::Render::Resources::MaterialSurfaceMode::Decal);
    EXPECT_TRUE(loaded->IsDecal());
    EXPECT_TRUE(loaded->IsBlendable());

    EXPECT_TRUE(NLS::Render::Resources::Loaders::MaterialLoader::Destroy(loaded));
    shaderManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialLoaderCreateRejectsSourceMaterialText)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_runtime_reject_source_material_" + NLS::Guid::New().ToString());
    const auto materialPath = root / "Materials" / "Source.mat";
    const std::string payload =
        "shaderLabMaterialVersion=1\n"
        "shader=?\n"
        "surfaceMode=Opaque\n";
    WriteTextFile(
        materialPath,
        payload);

    auto* defaultLoaded = NLS::Render::Resources::Loaders::MaterialLoader::Create(materialPath.string(), {false, false});
    EXPECT_EQ(defaultLoaded, nullptr)
        << "Runtime/default material loading must not treat source .mat files as authorized artifacts.";

    NLS::Render::Resources::Loaders::MaterialLoader::LoadOptions sourceOptions;
    sourceOptions.loadMissingTextures = false;
    sourceOptions.loadMissingShaders = false;
    sourceOptions.allowSourceAssetNativeContainer = true;
    auto* loaded = NLS::Render::Resources::Loaders::MaterialLoader::Create(materialPath.string(), sourceOptions);
    EXPECT_EQ(loaded, nullptr);

    auto* parsed = NLS::Render::Resources::Loaders::MaterialLoader::CreateFromSerializedPayload(
        materialPath.string(),
        payload,
        {false, false});
    EXPECT_EQ(parsed, nullptr)
        << "ShaderLab .mat payloads must reference an authoritative .shader source instead of shader=?.";
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialLoaderRejectsShaderLabMaterialWithoutShaderReference)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_shaderlab_material_missing_shader_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(root);

    const auto materialPath = MaterialArtifactPath(root, "3efafaddf8c80942dcb86b4727df50af94b636e293b43a6812bc91eba6f19f9b");
    WriteNativeArtifactTextFile(
        materialPath,
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        1u,
        "shaderLabMaterialVersion=1\n"
        "shader=?\n"
        "name=dirt_decal\n"
        "sourceSubAsset=material:material/21\n"
        "surfaceMode=Decal\n"
        "alphaMode=Blend\n");

    auto* loaded = NLS::Render::Resources::Loaders::MaterialLoader::Create(materialPath.string());
    EXPECT_EQ(loaded, nullptr)
        << "Runtime material artifacts without a .shader source reference must not half-load.";

    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialLoaderReloadRejectsMissingShaderWithoutMutatingExistingMaterial)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_shaderlab_reload_missing_shader_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(root);

    const auto materialPath = MaterialArtifactPath(root, "a01b94c6c07819e8d86e295b9c3643a485f09fb2709f1da8354b6ca64e755860");
    WriteNativeArtifactTextFile(
        materialPath,
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        1u,
        "shaderLabMaterialVersion=1\n"
        "shader=?\n"
        "name=HeroGlass\n"
        "surfaceMode=Transparent\n"
        "alphaMode=Blend\n");

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(WriteStandardPbrShaderArtifact(root).string());
    ASSERT_NE(shader, nullptr);
    NLS::Render::Resources::Material material(shader);
    material.path = "Library/Artifacts/00/good-material";
    const auto originalPath = material.path;
    material.SetShaderLabSourcePath("Assets/Shaders/AlreadyLoaded.shader");
    material.SetSurfaceMode(NLS::Render::Resources::MaterialSurfaceMode::Opaque);

    NLS::Render::Resources::Loaders::MaterialLoader::Reload(material, materialPath.string());

    EXPECT_EQ(material.GetShader(), shader);
    EXPECT_EQ(material.path, originalPath);
    EXPECT_EQ(material.GetShaderLabSourcePath(), "Assets/Shaders/AlreadyLoaded.shader");
    EXPECT_EQ(material.GetSurfaceMode(), NLS::Render::Resources::MaterialSurfaceMode::Opaque)
        << "Failed reloads must leave shared live materials untouched.";

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialLoaderReloadRejectsInvalidSurfaceModeWithoutMutatingExistingMaterial)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_shaderlab_reload_invalid_surface_mode_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(root);

    const auto materialPath = MaterialArtifactPath(root, "e0389d5b6b28e7d56873629e1fdbab6f3f8e0d62dbfeec1ddfa4b9d32271c00e");
    WriteNativeMaterialArtifactFile(
        materialPath,
        "shaderLabMaterialVersion=1\n"
        "shader=Assets/Shaders/BadReload.shader\n"
        "surfaceMode=NotAMode\n"
        "keyword _BAD_RELOAD\n"
        "property _Roughness Float 0.125\n");

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(WriteStandardPbrShaderArtifact(root).string());
    ASSERT_NE(shader, nullptr);
    NLS::Render::Resources::Material material(shader);
    material.path = "Library/Artifacts/00/original-material";
    const auto originalPath = material.path;
    material.SetShaderLabSourcePath("Assets/Shaders/Original.shader");
    material.SetSurfaceMode(NLS::Render::Resources::MaterialSurfaceMode::Opaque);
    material.EnableKeyword("_ORIGINAL");
    material.SetRawParameter("_Roughness", 0.75f);

    NLS::Render::Resources::Loaders::MaterialLoader::Reload(material, materialPath.string());

    EXPECT_EQ(material.GetShader(), shader);
    EXPECT_EQ(material.path, originalPath);
    EXPECT_EQ(material.GetShaderLabSourcePath(), "Assets/Shaders/Original.shader");
    EXPECT_EQ(material.GetSurfaceMode(), NLS::Render::Resources::MaterialSurfaceMode::Opaque);
    EXPECT_TRUE(material.IsKeywordEnabled("_ORIGINAL"));
    EXPECT_FALSE(material.IsKeywordEnabled("_BAD_RELOAD"));
    const auto* roughnessValue = material.GetParameterBlock().TryGet("_Roughness");
    ASSERT_NE(roughnessValue, nullptr);
    ASSERT_EQ(roughnessValue->type(), typeid(float));
    EXPECT_FLOAT_EQ(std::any_cast<float>(*roughnessValue), 0.75f);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialLoaderUnescapesShaderLabMaterialFields)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_shaderlab_material_unescape_" + NLS::Guid::New().ToString());
    const auto materialPath = MaterialArtifactPath(root, "58f43d62e295d8c9cb6a1d07a0169c0da1516b723a5e6cd1250c73546b59c021");

    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::ShaderManager>(shaderManager);

    const auto shaderArtifactPath = WriteStandardPbrShaderArtifact(root);
    const auto shaderSourcePath = WriteShaderLabSourceBackedStandardPbrShaderArtifact(root);
    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(shaderArtifactPath.string());
    ASSERT_NE(shader, nullptr);
    shaderManager.RegisterResource(shaderArtifactPath.string(), shader);

    WriteNativeMaterialArtifactFile(
        materialPath,
        "shaderLabMaterialVersion=1\n"
        "shader=" + shaderSourcePath.generic_string() + "\n"
        "name=Hero%0AsurfaceMode%3DDecal\n"
        "surfaceMode=Transparent\n"
        "property _BaseMap Texture2D Textures%5CHero%5CBase%0AColor.png\n");

    auto* loaded = NLS::Render::Resources::Loaders::MaterialLoader::Create(
        materialPath.string(),
        {false});
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->GetSurfaceMode(), NLS::Render::Resources::MaterialSurfaceMode::Transparent);
    EXPECT_EQ(loaded->GetTextureResourcePath("_BaseMap"), "Textures\\Hero\\Base\nColor.png");

    EXPECT_TRUE(NLS::Render::Resources::Loaders::MaterialLoader::Destroy(loaded));
    shaderManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialLoaderRoundTripsEscapedShaderLabTextureFields)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_shaderlab_material_codec_" + NLS::Guid::New().ToString());
    const auto materialPath = MaterialArtifactPath(root, "0bdf633d7ddc46c6fe6eb4262de6be2e7b9e30a7dc44fbe2a13a964813bbf3b9");

    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::ShaderManager>(shaderManager);

    const auto shaderArtifactPath = WriteStandardPbrShaderArtifact(root);
    const auto shaderSourcePath = WriteShaderLabSourceBackedStandardPbrShaderArtifact(root);
    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(shaderArtifactPath.string());
    ASSERT_NE(shader, nullptr);
    shaderManager.RegisterResource(shaderArtifactPath.string(), shader);

    const std::string texturePath = "Textures\\new folder\\Base=Color A.png";
    WriteNativeMaterialArtifactFile(
        materialPath,
        "shaderLabMaterialVersion=1\n"
        "shader=" + shaderSourcePath.generic_string() + "\n"
        "surfaceMode=Opaque\n"
        "property _BaseMap Texture2D Textures%5Cnew%20folder%5CBase%3DColor%20A.png\n"
        "textureSlot _BaseMap texture=image%2FBase%3DSlot resourcePath=Textures%5Cnew%20folder%5CBase%3DColor%20A.png "
            "wrapS=ClampToEdge wrapT=MirrorRepeat minFilter=Nearest magFilter=Nearest\n");

    auto* loaded = NLS::Render::Resources::Loaders::MaterialLoader::Create(
        materialPath.string(),
        {false});
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->GetTextureResourcePath("_BaseMap"), texturePath);
    const auto* sampler = loaded->GetSamplerOverride("sampler_BaseMap");
    ASSERT_NE(sampler, nullptr);
    EXPECT_EQ(sampler->wrapU, NLS::Render::RHI::TextureWrap::ClampToEdge);
    EXPECT_EQ(sampler->wrapV, NLS::Render::RHI::TextureWrap::MirrorRepeat);

    const auto savedPath = MaterialArtifactPath(root, "1111111111111111111111111111111111111111111111111111111111111111");
    NLS::Render::Resources::Loaders::MaterialLoader::Save(*loaded, savedPath.string());
    auto* reloaded = NLS::Render::Resources::Loaders::MaterialLoader::Create(
        savedPath.string(),
        {false});
    ASSERT_NE(reloaded, nullptr);
    EXPECT_EQ(reloaded->GetTextureResourcePath("_BaseMap"), texturePath);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::MaterialLoader::Destroy(reloaded));
    EXPECT_TRUE(NLS::Render::Resources::Loaders::MaterialLoader::Destroy(loaded));
    shaderManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialLoaderRejectsRuntimeShaderSourceReferences)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_shaderlab_material_source_shader_rejected_" + NLS::Guid::New().ToString());
    const auto materialPath = MaterialArtifactPath(root, "5199818771a0f8d0e627b99056be30b7b95ed6328e7b2e58d6b81ea391060137");

    WriteNativeMaterialArtifactFile(
        materialPath,
        "shaderLabMaterialVersion=1\n"
        "shader=Shaders/Cold.hlsl\n"
        "surfaceMode=Opaque\n");

    auto* rejected = NLS::Render::Resources::Loaders::MaterialLoader::Create(materialPath.string());
    EXPECT_EQ(rejected, nullptr);

    NLS::Render::Resources::Loaders::MaterialLoader::LoadOptions loadOptions;
    loadOptions.allowSourceAssetNativeContainer = true;
    auto* loaded = NLS::Render::Resources::Loaders::MaterialLoader::Create(materialPath.string(), loadOptions);
    EXPECT_EQ(loaded, nullptr);

    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialLoaderRejectsShaderArtifactPathReferences)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_shaderlab_material_artifact_reference_rejected_" + NLS::Guid::New().ToString());
    const auto materialPath = MaterialArtifactPath(root, "a3c991ef5899760f0c1c0c924443469336cfb9ab1c89f88c42ade95691a3d235");

    WriteNativeMaterialArtifactFile(
        materialPath,
        "shaderLabMaterialVersion=1\n"
        "shader=Library/Artifacts/2b/2b40aa9d7e26302abaee46d90172b24a111dda5b6d466fcf2e7a2aff001a0607\n"
        "surfaceMode=Opaque\n");

    auto* rejected = NLS::Render::Resources::Loaders::MaterialLoader::Create(materialPath.string());
    EXPECT_EQ(rejected, nullptr);

    NLS::Render::Resources::Loaders::MaterialLoader::LoadOptions loadOptions;
    loadOptions.allowSourceAssetNativeContainer = true;
    auto* loaded = NLS::Render::Resources::Loaders::MaterialLoader::Create(materialPath.string(), loadOptions);
    EXPECT_EQ(loaded, nullptr)
        << ".mat must reference the authoritative .shader source; ArtifactDB owns artifact payload paths.";

    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialLoaderAcceptsShaderLabSourceAsAuthoritativeShaderReference)
{
    auto* loaded = NLS::Render::Resources::Loaders::MaterialLoader::CreateFromSerializedPayload(
        "Assets/Materials/SourceBacked.mat",
        "shaderLabMaterialVersion=1\n"
        "shader=Assets/Shaders/Multi.shader\n"
        "surfaceMode=Opaque\n",
        {false, false});

    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->GetShaderLabSourcePath(), "Assets/Shaders/Multi.shader");
    EXPECT_EQ(loaded->GetShader(), nullptr)
        << "A .mat source shader reference must not direct-load or compile the .shader source.";

    EXPECT_TRUE(NLS::Render::Resources::Loaders::MaterialLoader::Destroy(loaded));
}

TEST(AssetMaterialConversionTests, MaterialLoaderAppliesShaderLabSourceBackedPropertiesWithoutAttachedShader)
{
    auto* loaded = NLS::Render::Resources::Loaders::MaterialLoader::CreateFromSerializedPayload(
        "Assets/Materials/SourceBackedProperties.mat",
        "shaderLabMaterialVersion=1\n"
        "shader=Assets/Shaders/StandardPBR.shader\n"
        "surfaceMode=Opaque\n"
        "property _BaseColor Color 0.25 0.50 0.75 1.00\n"
        "property _Roughness Float 0.625\n",
        {false, false});

    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->GetShader(), nullptr);
    EXPECT_EQ(loaded->GetShaderLabSourcePath(), "Assets/Shaders/StandardPBR.shader");

    const auto* baseColorValue = loaded->GetParameterBlock().TryGet("_BaseColor");
    ASSERT_NE(baseColorValue, nullptr)
        << "ShaderLab source-backed materials must preserve serialized properties even before pass shaders are attached.";
    ASSERT_EQ(baseColorValue->type(), typeid(NLS::Maths::Vector4));
    const auto& baseColor = std::any_cast<const NLS::Maths::Vector4&>(*baseColorValue);
    EXPECT_FLOAT_EQ(baseColor.x, 0.25f);
    EXPECT_FLOAT_EQ(baseColor.y, 0.50f);
    EXPECT_FLOAT_EQ(baseColor.z, 0.75f);
    EXPECT_FLOAT_EQ(baseColor.w, 1.00f);

    const auto* roughnessValue = loaded->GetParameterBlock().TryGet("_Roughness");
    ASSERT_NE(roughnessValue, nullptr);
    ASSERT_EQ(roughnessValue->type(), typeid(float));
    EXPECT_FLOAT_EQ(std::any_cast<float>(*roughnessValue), 0.625f);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::MaterialLoader::Destroy(loaded));
}

TEST(AssetMaterialConversionTests, MaterialLoaderResolvesShaderLabSourcePassArtifactsFromArtifactDatabase)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_shaderlab_source_pass_resolve_" + NLS::Guid::New().ToString());
    const auto projectAssets = root / "Assets";
    const auto shaderSourcePath = WriteShaderLabSourceBackedStandardPbrShaderArtifact(root);
    const auto materialPath = MaterialArtifactPath(root, "8fd3390cfac85bb6b452329d3766f8b91a789f9a51dcc4e3648752375a20ec07");

    WriteNativeMaterialArtifactFile(
        materialPath,
        "shaderLabMaterialVersion=1\n"
        "shader=" + shaderSourcePath.generic_string() + "\n"
        "surfaceMode=Opaque\n"
        "property _BaseColor Color 0.250000 0.500000 0.750000 1.000000\n");

    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    NLS::Core::ResourceManagement::TextureManager textureManager;
    const ScopedShaderManagerAssetPaths shaderAssetPaths(
        projectAssets.string() + "/",
        "App/Assets/Engine/");
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::ShaderManager>(shaderManager);
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::TextureManager>(textureManager);

    auto* loaded = NLS::Render::Resources::Loaders::MaterialLoader::Create(materialPath.string(), {false, true});
    ASSERT_NE(loaded, nullptr);
    ASSERT_EQ(loaded->GetShaderLabSourcePath(), shaderSourcePath.generic_string());
    auto* forwardShader = loaded->ResolveShaderForLightMode("Forward");
    ASSERT_NE(forwardShader, nullptr);
    EXPECT_EQ(forwardShader, loaded->GetShader());
    EXPECT_EQ(forwardShader->GetImportedArtifactSourcePath(), shaderSourcePath.generic_string());
    EXPECT_EQ(forwardShader->GetShaderLabLightMode(), "Forward");
    EXPECT_EQ(loaded->ResolveShaderForLightMode("DepthOnly"), nullptr);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::MaterialLoader::Destroy(loaded));
    shaderManager.UnloadResources();
    textureManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::TextureManager>();
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialLoaderMatchesShaderLabPassArtifactsByNormalizedSourcePath)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_shaderlab_source_pass_normalized_resolve_" + NLS::Guid::New().ToString());
    const auto projectAssets = root / "Assets";
    const auto shaderSourcePath = WriteShaderLabSourceBackedStandardPbrShaderArtifact(root);
    const auto materialPath = MaterialArtifactPath(root, "b849801dcde7e91a308f68782aa84df2038d786d32c05df2df0f2c742f695d57");

    WriteNativeMaterialArtifactFile(
        materialPath,
        "shaderLabMaterialVersion=1\n"
        "shader=Assets/./Shaders/StandardPBR.shader\n"
        "surfaceMode=Opaque\n"
        "property _BaseColor Color 0.250000 0.500000 0.750000 1.000000\n");

    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    NLS::Core::ResourceManagement::TextureManager textureManager;
    const ScopedShaderManagerAssetPaths shaderAssetPaths(
        projectAssets.string() + "/",
        "App/Assets/Engine/");
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::ShaderManager>(shaderManager);
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::TextureManager>(textureManager);

    auto* loaded = NLS::Render::Resources::Loaders::MaterialLoader::Create(materialPath.string(), {false, true});
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->GetShaderLabSourcePath(), "Assets/./Shaders/StandardPBR.shader");
    auto* forwardShader = loaded->ResolveShaderForLightMode("Forward");
    ASSERT_NE(forwardShader, nullptr)
        << "Material pass registration must compare ShaderLab source identities after path normalization.";
    EXPECT_EQ(forwardShader->GetImportedArtifactSourcePath(), shaderSourcePath.generic_string());

    EXPECT_TRUE(NLS::Render::Resources::Loaders::MaterialLoader::Destroy(loaded));
    shaderManager.UnloadResources();
    textureManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::TextureManager>();
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialLoaderResolvesRuntimeShaderLabPassArtifactsFromExplicitArtifactDatabase)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_shaderlab_runtime_source_pass_resolve_" + NLS::Guid::New().ToString());
    const auto dataRoot = root / "Data";
    const auto shaderSourcePath = std::filesystem::path("Assets") / "Shaders" / "RuntimeMulti.shader";
    const auto shaderSourceId = NLS::Core::Assets::AssetId(NLS::Guid::New());
    const auto shaderArtifactHash =
        "9a14e94a076b68fcad5f2bc73efbdf8d7e88eb5fc171edc925d03ac358a64bc9";
    const auto materialArtifactHash =
        "392390f453ae436a1093c55c6b4d804ae257f4fa1645b60cb9433a2b958f24f0";

    auto artifact = MakeStandardPbrShaderArtifact();
    artifact.sourcePath = shaderSourcePath.generic_string();
    artifact.subAssetKey = "shader:RuntimeMulti/Forward#0";
    artifact.shaderLabLightMode = "Forward";
    artifact.shaderLabPassState = NLS::Render::ShaderLab::ShaderLabPassState{};
    WriteBinaryFile(
        dataRoot / RuntimeArtifactPath(shaderArtifactHash),
        NLS::Render::Assets::SerializeShaderArtifact(artifact));

    NLS::Core::Assets::ArtifactManifest manifest;
    manifest.sourceAssetId = shaderSourceId;
    manifest.importerId = "ShaderLabImporter";
    manifest.importerVersion = 1u;
    manifest.targetPlatform = "win64";
    manifest.primarySubAssetKey = artifact.subAssetKey;
    manifest.subAssets.push_back({
        shaderSourceId,
        artifact.subAssetKey,
        NLS::Core::Assets::ArtifactType::Shader,
        "ShaderLoader",
        "win64",
        RuntimeArtifactPath(shaderArtifactHash),
        "sha256:" + std::string(shaderArtifactHash),
        "RuntimeMulti Forward"
    });

    NLS::Core::Assets::ArtifactDatabase database;
    database.UpsertManifest(
        manifest,
        shaderSourcePath.generic_string(),
        NLS::Core::Assets::ArtifactRecordStatus::UpToDate);
    ASSERT_TRUE(database.Save(dataRoot / "ArtifactDB"));

    const auto materialPath = RuntimeMaterialArtifactPath(root, materialArtifactHash);
    WriteNativeMaterialArtifactFile(
        materialPath,
        "shaderLabMaterialVersion=1\n"
        "shader=" + shaderSourcePath.generic_string() + "\n"
        "surfaceMode=Opaque\n");

    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    NLS::Core::ResourceManagement::TextureManager textureManager;
    const ScopedShaderManagerAssetPaths shaderAssetPaths(
        (root / "Assets").string() + "/",
        "App/Assets/Engine/");
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::ShaderManager>(shaderManager);
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::TextureManager>(textureManager);

    NLS::Render::Resources::Loaders::MaterialLoader::LoadOptions options;
    options.loadMissingTextures = false;
    options.loadMissingShaders = true;
    options.artifactDatabasePath = dataRoot / "ArtifactDB";
    options.targetPlatform = "win64";
    auto* loaded = NLS::Render::Resources::Loaders::MaterialLoader::Create(materialPath.string(), options);

    ASSERT_NE(loaded, nullptr);
    ASSERT_EQ(loaded->GetShaderLabSourcePath(), shaderSourcePath.generic_string());
    auto* forwardShader = loaded->ResolveShaderForLightMode("Forward");
    ASSERT_NE(forwardShader, nullptr);
    EXPECT_EQ(forwardShader->GetImportedArtifactSourcePath(), shaderSourcePath.generic_string());
    EXPECT_EQ(forwardShader->GetShaderLabLightMode(), "Forward");
    EXPECT_EQ(forwardShader->path.find((dataRoot / "Artifacts").string()), 0u);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::MaterialLoader::Destroy(loaded));
    shaderManager.UnloadResources();
    textureManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::TextureManager>();
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialLoaderReusesCachedShaderLabPassArtifactPaths)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_shaderlab_runtime_source_pass_cache_" + NLS::Guid::New().ToString());
    const auto dataRoot = root / "Data";
    const auto shaderSourcePath = std::filesystem::path("Assets") / "Shaders" / "CachedMulti.shader";
    const auto shaderSourceId = NLS::Core::Assets::AssetId(NLS::Guid::New());
    const auto shaderArtifactHash =
        "9e24e94a076b68fcad5f2bc73efbdf8d7e88eb5fc171edc925d03ac358a64bc9";
    const auto firstMaterialArtifactHash =
        "492390f453ae436a1093c55c6b4d804ae257f4fa1645b60cb9433a2b958f24f0";
    const auto secondMaterialArtifactHash =
        "592390f453ae436a1093c55c6b4d804ae257f4fa1645b60cb9433a2b958f24f0";

    auto artifact = MakeStandardPbrShaderArtifact();
    artifact.sourcePath = shaderSourcePath.generic_string();
    artifact.subAssetKey = "shader:CachedMulti/Forward#0";
    artifact.shaderLabLightMode = "Forward";
    artifact.shaderLabPassState = NLS::Render::ShaderLab::ShaderLabPassState{};
    WriteBinaryFile(
        dataRoot / RuntimeArtifactPath(shaderArtifactHash),
        NLS::Render::Assets::SerializeShaderArtifact(artifact));

    NLS::Core::Assets::ArtifactManifest manifest;
    manifest.sourceAssetId = shaderSourceId;
    manifest.importerId = "ShaderLabImporter";
    manifest.importerVersion = 1u;
    manifest.targetPlatform = "win64";
    manifest.primarySubAssetKey = artifact.subAssetKey;
    manifest.subAssets.push_back({
        shaderSourceId,
        artifact.subAssetKey,
        NLS::Core::Assets::ArtifactType::Shader,
        "ShaderLoader",
        "win64",
        RuntimeArtifactPath(shaderArtifactHash),
        "sha256:" + std::string(shaderArtifactHash),
        "CachedMulti Forward"
    });

    NLS::Core::Assets::ArtifactDatabase database;
    database.UpsertManifest(
        manifest,
        shaderSourcePath.generic_string(),
        NLS::Core::Assets::ArtifactRecordStatus::UpToDate);
    ASSERT_TRUE(database.Save(dataRoot / "ArtifactDB"));

    const auto firstMaterialPath = RuntimeMaterialArtifactPath(root, firstMaterialArtifactHash);
    const auto secondMaterialPath = RuntimeMaterialArtifactPath(root, secondMaterialArtifactHash);
    const auto materialPayload =
        "shaderLabMaterialVersion=1\n"
        "shader=" + shaderSourcePath.generic_string() + "\n"
        "surfaceMode=Opaque\n";
    WriteNativeMaterialArtifactFile(firstMaterialPath, materialPayload);
    WriteNativeMaterialArtifactFile(secondMaterialPath, materialPayload);

    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    NLS::Core::ResourceManagement::TextureManager textureManager;
    const ScopedShaderManagerAssetPaths shaderAssetPaths(
        (root / "Assets").string() + "/",
        "App/Assets/Engine/");
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::ShaderManager>(shaderManager);
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::TextureManager>(textureManager);

    NLS::Render::Resources::Loaders::MaterialLoader::LoadOptions options;
    options.loadMissingTextures = false;
    options.loadMissingShaders = true;
    options.artifactDatabasePath = dataRoot / "ArtifactDB";
    options.targetPlatform = "win64";
    auto* first = NLS::Render::Resources::Loaders::MaterialLoader::Create(firstMaterialPath.string(), options);
    auto* second = NLS::Render::Resources::Loaders::MaterialLoader::Create(secondMaterialPath.string(), options);

    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    auto* firstForward = first->ResolveShaderForLightMode("Forward");
    auto* secondForward = second->ResolveShaderForLightMode("Forward");
    ASSERT_NE(firstForward, nullptr);
    ASSERT_NE(secondForward, nullptr);
    EXPECT_EQ(firstForward->GetImportedArtifactSourcePath(), shaderSourcePath.generic_string());
    EXPECT_EQ(secondForward->GetImportedArtifactSourcePath(), shaderSourcePath.generic_string());
    EXPECT_EQ(secondForward->GetShaderLabLightMode(), "Forward");

    EXPECT_TRUE(NLS::Render::Resources::Loaders::MaterialLoader::Destroy(first));
    EXPECT_TRUE(NLS::Render::Resources::Loaders::MaterialLoader::Destroy(second));
    shaderManager.UnloadResources();
    textureManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::TextureManager>();
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialLoaderDefaultTargetPlatformDoesNotWildcardShaderLabPassArtifacts)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_shaderlab_material_platform_filter_" + NLS::Guid::New().ToString());
    const auto shaderSourcePath = std::filesystem::path("Assets") / "Shaders" / "PlatformFilter.shader";
    const auto shaderSourceId = NLS::Core::Assets::AssetId(NLS::Guid::New());
    const auto editorShaderHash =
        "3ce8c6cf9f32ef118b65e72f8193fbc930a4538e60335bce37dcf8d5fa83a101";
    const auto runtimeShaderHash =
        "c0f2355c058aa0f17a4812603ecf1816097706b89a75a8f27589cb546a3611ad";
    const auto materialHash =
        "2f209e74ea4f4b4fc34b4c54ba7f9bb90bdab3b771e5d6ebe0cc1af9ea57a773";

    auto editorShader = MakeStandardPbrShaderArtifact();
    editorShader.sourcePath = shaderSourcePath.generic_string();
    editorShader.subAssetKey = "shader:PlatformFilter/Forward#0";
    editorShader.shaderLabLightMode = "Forward";
    editorShader.shaderLabPassState = NLS::Render::ShaderLab::ShaderLabPassState{};
    WriteBinaryFile(
        root / "Library" / "Artifacts" / NLS::Core::Assets::BuildArtifactStorageRelativePath(editorShaderHash),
        NLS::Render::Assets::SerializeShaderArtifact(editorShader));

    auto runtimeShader = editorShader;
    runtimeShader.subAssetKey = "shader:PlatformFilter/Forward#1";
    WriteBinaryFile(
        root / "Library" / "Artifacts" / NLS::Core::Assets::BuildArtifactStorageRelativePath(runtimeShaderHash),
        NLS::Render::Assets::SerializeShaderArtifact(runtimeShader));

    NLS::Core::Assets::ArtifactManifest editorManifest;
    editorManifest.sourceAssetId = shaderSourceId;
    editorManifest.importerId = "ShaderLabImporter";
    editorManifest.importerVersion = 1u;
    editorManifest.targetPlatform = "editor";
    editorManifest.primarySubAssetKey = editorShader.subAssetKey;
    editorManifest.subAssets.push_back({
        shaderSourceId,
        editorShader.subAssetKey,
        NLS::Core::Assets::ArtifactType::Shader,
        "ShaderLoader",
        "editor",
        (std::filesystem::path("Library") / "Artifacts" / NLS::Core::Assets::BuildArtifactStorageRelativePath(editorShaderHash)).generic_string(),
        "sha256:" + std::string(editorShaderHash),
        "PlatformFilter editor Forward"
    });

    auto runtimeManifest = editorManifest;
    runtimeManifest.targetPlatform = "win64-dx12";
    runtimeManifest.primarySubAssetKey = runtimeShader.subAssetKey;
    runtimeManifest.subAssets.clear();
    runtimeManifest.subAssets.push_back({
        shaderSourceId,
        runtimeShader.subAssetKey,
        NLS::Core::Assets::ArtifactType::Shader,
        "ShaderLoader",
        "win64-dx12",
        (std::filesystem::path("Library") / "Artifacts" / NLS::Core::Assets::BuildArtifactStorageRelativePath(runtimeShaderHash)).generic_string(),
        "sha256:" + std::string(runtimeShaderHash),
        "PlatformFilter runtime Forward"
    });

    NLS::Core::Assets::ArtifactDatabase database;
    database.UpsertManifest(
        runtimeManifest,
        shaderSourcePath.generic_string(),
        NLS::Core::Assets::ArtifactRecordStatus::UpToDate);
    database.UpsertManifest(
        editorManifest,
        shaderSourcePath.generic_string(),
        NLS::Core::Assets::ArtifactRecordStatus::UpToDate);
    ASSERT_TRUE(database.Save(root / "Library" / "ArtifactDB"));

    const auto materialPath = MaterialArtifactPath(root, materialHash);
    WriteNativeMaterialArtifactFile(
        materialPath,
        "shaderLabMaterialVersion=1\n"
        "shader=" + shaderSourcePath.generic_string() + "\n"
        "surfaceMode=Opaque\n");

    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    NLS::Core::ResourceManagement::TextureManager textureManager;
    const ScopedShaderManagerAssetPaths shaderAssetPaths(
        (root / "Assets").string() + "/",
        "App/Assets/Engine/");
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::ShaderManager>(shaderManager);
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::TextureManager>(textureManager);

    NLS::Render::Resources::Loaders::MaterialLoader::LoadOptions options;
    options.loadMissingTextures = false;
    options.loadMissingShaders = true;
    auto* loaded = NLS::Render::Resources::Loaders::MaterialLoader::Create(materialPath.string(), options);

    ASSERT_NE(loaded, nullptr);
    auto* forwardShader = loaded->ResolveShaderForLightMode("Forward");
    ASSERT_NE(forwardShader, nullptr);
    EXPECT_EQ(forwardShader->GetImportedArtifactSubAssetKey(), editorShader.subAssetKey)
        << "Default material loads must filter ShaderLab pass artifacts by editor target instead of wildcarding all platforms.";

    EXPECT_TRUE(NLS::Render::Resources::Loaders::MaterialLoader::Destroy(loaded));
    shaderManager.UnloadResources();
    textureManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::TextureManager>();
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialLoaderSkipsInvalidNumericShaderLabProperties)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_shaderlab_material_invalid_numbers_" + NLS::Guid::New().ToString());
    const auto materialPath = MaterialArtifactPath(root);

    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::ShaderManager>(shaderManager);

    const auto shaderArtifactPath = WriteStandardPbrShaderArtifact(root);
    const auto shaderSourcePath = WriteShaderLabSourceBackedStandardPbrShaderArtifact(root);
    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(shaderArtifactPath.string());
    ASSERT_NE(shader, nullptr);
    shaderManager.RegisterResource(shaderArtifactPath.string(), shader);

    WriteNativeMaterialArtifactFile(
        materialPath,
        "shaderLabMaterialVersion=1\n"
        "shader=" + shaderSourcePath.generic_string() + "\n"
        "property _Metallic Float not-a-number\n"
        "property _Roughness Float also-bad\n");

    EXPECT_NO_THROW({
        auto* loaded = NLS::Render::Resources::Loaders::MaterialLoader::Create(materialPath.string());
        ASSERT_NE(loaded, nullptr);
        EXPECT_EQ(loaded->GetParameterBlock().TryGet("_Metallic"), nullptr);
        EXPECT_EQ(loaded->GetParameterBlock().TryGet("_Roughness"), nullptr);
        EXPECT_TRUE(NLS::Render::Resources::Loaders::MaterialLoader::Destroy(loaded));
    });

    shaderManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialLoaderRejectsLegacyXmlPayloadsInShaderLabOnlyMode)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_legacy_xml_material_rejected_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(root);

    const auto materialPath = root / "92d4459b0f8881533f26ed9a6d6c4f1575af27bc9e877cba3396b377d3bbc46b";
    {
        std::ofstream output(materialPath, std::ios::binary | std::ios::trunc);
        output <<
            "<root>\n"
            "  <shader>?</shader>\n"
            "  <surfaceMode>Opaque</surfaceMode>\n"
            "</root>\n";
    }

    auto* loaded = NLS::Render::Resources::Loaders::MaterialLoader::Create(materialPath.string());
    EXPECT_EQ(loaded, nullptr);

    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialLoaderSaveWritesShaderLabOnlyPayload)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_shaderlab_material_save_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(root);

    NLS::Render::Resources::Material material;
    material.SetShaderLabSourcePath("Assets/Shaders/Unlit.shader");
    material.SetSurfaceMode(NLS::Render::Resources::MaterialSurfaceMode::Opaque);
    material.SetBackfaceCulling(false);
    material.SetFrontfaceCulling(false);
    material.SetDepthWriting(true);

    const auto materialPath = MaterialArtifactPath(root);
    NLS::Render::Resources::Loaders::MaterialLoader::Save(material, materialPath.string());

    const auto payload = ReadNativeArtifactPayloadText(
        materialPath,
        NLS::Core::Assets::ArtifactType::Material,
        1u);

    EXPECT_NE(payload.find("shaderLabMaterialVersion=1"), std::string::npos);
    EXPECT_NE(payload.find("shader=Assets/Shaders/Unlit.shader"), std::string::npos);
    EXPECT_NE(payload.find("surfaceMode=Opaque"), std::string::npos);
    EXPECT_NE(payload.find("doubleSided=true"), std::string::npos);
    EXPECT_NE(payload.find("depthWrite=true"), std::string::npos);
    EXPECT_EQ(payload.find("<root>"), std::string::npos);
    EXPECT_EQ(payload.find("<shader>"), std::string::npos);

    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialLoaderSaveWritesReadableSourceMatNativeContainer)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_shaderlab_material_source_save_" + NLS::Guid::New().ToString());
    const auto materialPath = root / "Assets" / "Materials" / "Saved.mat";

    NLS::Render::Resources::Material material;
    material.SetShaderLabSourcePath("Assets/Shaders/Unlit.shader");
    material.SetSurfaceMode(NLS::Render::Resources::MaterialSurfaceMode::Opaque);
    material.SetBackfaceCulling(false);
    material.SetDepthWriting(true);

    NLS::Render::Resources::Loaders::MaterialLoader::Save(material, materialPath.string());

    const auto payload = ReadNativeArtifactPayloadText(
        materialPath,
        NLS::Core::Assets::ArtifactType::Material,
        1u);

    EXPECT_NE(payload.find("shaderLabMaterialVersion=1"), std::string::npos);
    EXPECT_NE(payload.find("shader=Assets/Shaders/Unlit.shader"), std::string::npos);

    auto* rejected = NLS::Render::Resources::Loaders::MaterialLoader::Create(materialPath.string());
    EXPECT_EQ(rejected, nullptr);

    NLS::Render::Resources::Loaders::MaterialLoader::LoadOptions loadOptions;
    loadOptions.allowSourceAssetNativeContainer = true;
    auto* loaded = NLS::Render::Resources::Loaders::MaterialLoader::Create(materialPath.string(), loadOptions);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->GetShaderLabSourcePath(), "Assets/Shaders/Unlit.shader");
    EXPECT_FALSE(loaded->HasBackfaceCulling());
    EXPECT_TRUE(NLS::Render::Resources::Loaders::MaterialLoader::Destroy(loaded));

    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialLoaderSavePreservesShaderLabSourceBackedRawProperties)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_shaderlab_material_save_source_backed_raw_properties_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(root);

    NLS::Render::Resources::Material material;
    material.SetShaderLabSourcePath("Assets/Shaders/StandardPBR.shader");
    material.SetRawParameter("_BaseColor", NLS::Maths::Vector4 {0.2f, 0.4f, 0.6f, 1.0f});
    material.SetRawParameter("_Roughness", 0.55f);
    material.SetRawParameter("_UseDetail", 1);
    material.SetTextureResourcePath("_BaseMap", LibraryArtifactPath("2222222222222222222222222222222222222222222222222222222222222222"));

    const auto materialPath = MaterialArtifactPath(root, "2db56a4dbb0f967815069bbd8a075fbb66e8f6aa2f82327c962f831de58ef24c");
    NLS::Render::Resources::Loaders::MaterialLoader::Save(material, materialPath.string());

    const auto payload = ReadNativeArtifactPayloadText(
        materialPath,
        NLS::Core::Assets::ArtifactType::Material,
        1u);

    EXPECT_NE(payload.find("shader=Assets/Shaders/StandardPBR.shader"), std::string::npos);
    EXPECT_NE(payload.find("property _BaseColor Color 0.200000 0.400000 0.600000 1.000000"), std::string::npos);
    EXPECT_NE(payload.find("property _Roughness Float 0.550000"), std::string::npos);
    EXPECT_NE(payload.find("property _UseDetail Int 1"), std::string::npos);
    EXPECT_NE(payload.find("property _BaseMap Texture2D Library/Artifacts/22/2222222222222222222222222222222222222222222222222222222222222222"), std::string::npos);

    auto* reloaded = NLS::Render::Resources::Loaders::MaterialLoader::Create(materialPath.string(), {false, false});
    ASSERT_NE(reloaded, nullptr);
    EXPECT_NE(reloaded->GetParameterBlock().TryGet("_BaseColor"), nullptr);
    EXPECT_NE(reloaded->GetParameterBlock().TryGet("_Roughness"), nullptr);
    EXPECT_NE(reloaded->GetParameterBlock().TryGet("_UseDetail"), nullptr);
    EXPECT_EQ(
        reloaded->GetTextureResourcePath("_BaseMap"),
        LibraryArtifactPath("2222222222222222222222222222222222222222222222222222222222222222"));

    EXPECT_TRUE(NLS::Render::Resources::Loaders::MaterialLoader::Destroy(reloaded));
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialLoaderSaveWritesShaderLabPropertyNames)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_shaderlab_material_save_properties_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(root);

    const auto shaderArtifactPath = ArtifactPath(
        root,
        "2b40aa9d7e26302abaee46d90172b24a111dda5b6d466fcf2e7a2aff001a0607");
    WriteBinaryFile(
        shaderArtifactPath,
        NLS::Render::Assets::SerializeShaderArtifact(MakeShaderArtifact(
            "Assets/Shaders/StandardPBR.shader",
            "shader:StandardPBR",
            MakeShaderLabNamedMaterialReflection())));
    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(shaderArtifactPath.string());
    ASSERT_NE(shader, nullptr);

    NLS::Render::Resources::Material material(shader);
    material.SetShaderLabSourcePath("Assets/Shaders/StandardPBR.shader");
    material.Set<NLS::Maths::Vector4>("_BaseColor", {0.25f, 0.5f, 0.75f, 1.0f});
    material.Set<float>("_Metallic", 0.35f);
    material.Set<float>("_Roughness", 0.65f);

    const auto materialPath = MaterialArtifactPath(root);
    NLS::Render::Resources::Loaders::MaterialLoader::Save(material, materialPath.string());

    const auto payload = ReadNativeArtifactPayloadText(
        materialPath,
        NLS::Core::Assets::ArtifactType::Material,
        1u);

    EXPECT_NE(payload.find("property _BaseColor Color 0.250000 0.500000 0.750000 1.000000"), std::string::npos);
    EXPECT_NE(payload.find("property _Metallic Float 0.350000"), std::string::npos);
    EXPECT_NE(payload.find("property _Roughness Float 0.650000"), std::string::npos);
    EXPECT_EQ(payload.find("property u_Albedo"), std::string::npos);
    EXPECT_EQ(payload.find("property u_Metallic"), std::string::npos);
    EXPECT_EQ(payload.find("property u_Roughness"), std::string::npos);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialLoaderSavePreservesNativeShaderLabPropertyNames)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_shaderlab_material_save_native_properties_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(root);

    const auto shaderArtifactPath = ArtifactPath(
        root,
        "2b40aa9d7e26302abaee46d90172b24a111dda5b6d466fcf2e7a2aff001a0607");
    WriteBinaryFile(
        shaderArtifactPath,
        NLS::Render::Assets::SerializeShaderArtifact(MakeShaderArtifact(
            "Assets/Shaders/Native.shader",
            "shader:Native",
            MakeShaderLabNamedMaterialReflection())));

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(shaderArtifactPath.string());
    ASSERT_NE(shader, nullptr);

    NLS::Render::Resources::Material material(shader);
    material.SetShaderLabSourcePath("Assets/Shaders/Native.shader");
    material.Set<NLS::Maths::Vector4>("_BaseColor", {0.125f, 0.25f, 0.5f, 1.0f});
    const auto textureResourcePath =
        LibraryArtifactPath("1111111111111111111111111111111111111111111111111111111111111111");
    material.SetTextureResourcePath("_BaseMap", textureResourcePath);

    const auto materialPath = MaterialArtifactPath(root, "43c80be0cf7d568c4af735cd446da1049d27d23d750f2fb1525afc8ddc5c2a6d");
    NLS::Render::Resources::Loaders::MaterialLoader::Save(material, materialPath.string());

    const auto payload = ReadNativeArtifactPayloadText(
        materialPath,
        NLS::Core::Assets::ArtifactType::Material,
        1u);
    EXPECT_NE(payload.find("property _BaseColor Color 0.125000 0.250000 0.500000 1.000000"), std::string::npos);
    EXPECT_NE(
        payload.find("property _BaseMap Texture2D " + textureResourcePath),
        std::string::npos);
    EXPECT_EQ(payload.find("property u_Albedo"), std::string::npos);
    EXPECT_EQ(payload.find("property u_AlbedoMap"), std::string::npos);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialLoaderSaveRoundTripsKeywordsAndSamplerOverrides)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_shaderlab_material_save_keywords_samplers_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(root);

    NLS::Render::Resources::Material material;
    material.SetShaderLabSourcePath("Assets/Shaders/StandardPBR.shader");
    material.EnableKeyword("_NORMALMAP");
    material.EnableKeyword("_ALPHATEST_ON");
    NLS::Render::RHI::SamplerDesc sampler;
    sampler.wrapU = NLS::Render::RHI::TextureWrap::ClampToEdge;
    sampler.wrapV = NLS::Render::RHI::TextureWrap::MirrorRepeat;
    sampler.wrapW = sampler.wrapV;
    sampler.minFilter = NLS::Render::RHI::TextureFilter::Nearest;
    sampler.magFilter = NLS::Render::RHI::TextureFilter::Nearest;
    material.SetSamplerOverride("u_LinearWrapSampler", sampler);

    const auto materialPath = MaterialArtifactPath(root);
    NLS::Render::Resources::Loaders::MaterialLoader::Save(material, materialPath.string());

    auto* loaded = NLS::Render::Resources::Loaders::MaterialLoader::Create(materialPath.string());
    ASSERT_NE(loaded, nullptr);
    EXPECT_TRUE(loaded->IsKeywordEnabled("_NORMALMAP"));
    EXPECT_TRUE(loaded->IsKeywordEnabled("_ALPHATEST_ON"));
    const auto* loadedSampler = loaded->GetSamplerOverride("u_LinearWrapSampler");
    ASSERT_NE(loadedSampler, nullptr);
    EXPECT_EQ(loadedSampler->wrapU, NLS::Render::RHI::TextureWrap::ClampToEdge);
    EXPECT_EQ(loadedSampler->wrapV, NLS::Render::RHI::TextureWrap::MirrorRepeat);
    EXPECT_EQ(loadedSampler->minFilter, NLS::Render::RHI::TextureFilter::Nearest);
    EXPECT_EQ(loadedSampler->magFilter, NLS::Render::RHI::TextureFilter::Nearest);

    const auto clearedPath = MaterialArtifactPath(root, "81cf3bfb7d919aff75b7c03bd58ed20fce34c36f6b0b777f2b1c5c663c0108c1");
    WriteNativeMaterialArtifactFile(
        clearedPath,
        "shaderLabMaterialVersion=1\n"
        "shader=Assets/Shaders/StandardPBR.shader\n"
        "surfaceMode=Opaque\n");
    NLS::Render::Resources::Loaders::MaterialLoader::Reload(*loaded, clearedPath.string());
    EXPECT_FALSE(loaded->IsKeywordEnabled("_NORMALMAP"));
    EXPECT_EQ(loaded->GetSamplerOverride("u_LinearWrapSampler"), nullptr);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::MaterialLoader::Destroy(loaded));
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialLoaderUsesExplicitShaderLabSurfaceModeOverNames)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_legacy_source_decal_transparent_material_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(root);

    const auto materialPath = MaterialArtifactPath(root, "d548daeb0f1714261c9db43313aaab7e880782327762d412e864cb26c7e0da15");
    WriteNativeMaterialArtifactFile(
        materialPath,
        "shaderLabMaterialVersion=1\n"
        "shader=Assets/Shaders/StandardPBR.shader\n"
        "name=HeroGlass\n"
        "sourceSubAsset=material:decal_folder/hero_glass\n"
        "surfaceMode=Transparent\n"
        "alphaMode=Blend\n");

    auto* loaded = NLS::Render::Resources::Loaders::MaterialLoader::Create(materialPath.string());
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->GetSurfaceMode(), NLS::Render::Resources::MaterialSurfaceMode::Transparent);
    EXPECT_FALSE(loaded->IsDecal());
    EXPECT_TRUE(loaded->IsBlendable());

    EXPECT_TRUE(NLS::Render::Resources::Loaders::MaterialLoader::Destroy(loaded));
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialLoaderAllowsShaderLabDecalWithoutName)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_legacy_source_decal_material_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(root);

    const auto materialPath = MaterialArtifactPath(root, "b2a3baea1e4e7f8fb677157fdaa83df9fadd88f63ffadbe5112432f8b508cc89");
    WriteNativeMaterialArtifactFile(
        materialPath,
        "shaderLabMaterialVersion=1\n"
        "shader=Assets/Shaders/StandardPBR.shader\n"
        "sourceSubAsset=material:dirt_decal\n"
        "surfaceMode=Decal\n"
        "alphaMode=Blend\n");

    auto* loaded = NLS::Render::Resources::Loaders::MaterialLoader::Create(materialPath.string());
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->GetSurfaceMode(), NLS::Render::Resources::MaterialSurfaceMode::Decal);
    EXPECT_TRUE(loaded->IsDecal());
    EXPECT_TRUE(loaded->IsBlendable());

    EXPECT_TRUE(NLS::Render::Resources::Loaders::MaterialLoader::Destroy(loaded));
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialLoaderExplicitShaderLabTransparentOverridesDecalName)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_explicit_transparent_decal_name_material_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(root);

    const auto materialPath = MaterialArtifactPath(root, "95b8c567cab4f5edfc5a77111fbad92b4b83677d135dcc4f7f89eeb894dab9d1");
    WriteNativeMaterialArtifactFile(
        materialPath,
        "shaderLabMaterialVersion=1\n"
        "shader=Assets/Shaders/StandardPBR.shader\n"
        "surfaceMode=Transparent\n"
        "name=dirt_decal\n"
        "alphaMode=Blend\n");

    auto* loaded = NLS::Render::Resources::Loaders::MaterialLoader::Create(materialPath.string());
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->GetSurfaceMode(), NLS::Render::Resources::MaterialSurfaceMode::Transparent);
    EXPECT_FALSE(loaded->IsDecal());
    EXPECT_TRUE(loaded->IsBlendable());

    EXPECT_TRUE(NLS::Render::Resources::Loaders::MaterialLoader::Destroy(loaded));
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialLoaderExplicitShaderLabOpaqueOverridesBlendAlphaMode)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_explicit_opaque_blendable_material_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(root);

    const auto materialPath = MaterialArtifactPath(root, "8c764dcb75697236ea073d25403306f7c3461ecee2dd8e26bd1aed6f281ca3b1");
    WriteNativeMaterialArtifactFile(
        materialPath,
        "shaderLabMaterialVersion=1\n"
        "shader=Assets/Shaders/StandardPBR.shader\n"
        "surfaceMode=Opaque\n"
        "name=dirt_decal\n"
        "alphaMode=Blend\n");

    auto* loaded = NLS::Render::Resources::Loaders::MaterialLoader::Create(materialPath.string());
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->GetSurfaceMode(), NLS::Render::Resources::MaterialSurfaceMode::Opaque);
    EXPECT_FALSE(loaded->IsBlendable());
    EXPECT_FALSE(loaded->IsDecal());

    EXPECT_TRUE(NLS::Render::Resources::Loaders::MaterialLoader::Destroy(loaded));
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialLoaderRejectsUnimplementedAlphaTestSurfaceMode)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_alpha_test_surface_mode_material_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(root);

    const auto materialPath = MaterialArtifactPath(root, "606714587d5412f975380472fe84f412b86c7fe501a30c2562f25df98401dab7");
    WriteNativeMaterialArtifactFile(
        materialPath,
        "shaderLabMaterialVersion=1\n"
        "shader=?\n"
        "surfaceMode=AlphaTest\n");

    auto* loaded = NLS::Render::Resources::Loaders::MaterialLoader::Create(materialPath.string());
    EXPECT_EQ(loaded, nullptr);

    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialLoaderRejectsInvalidExplicitSurfaceMode)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_invalid_surface_mode_material_" + NLS::Guid::New().ToString());
    const auto shaderArtifactPath = ArtifactPath(
        root,
        "2b40aa9d7e26302abaee46d90172b24a111dda5b6d466fcf2e7a2aff001a0607");
    WriteBinaryFile(
        shaderArtifactPath,
        NLS::Render::Assets::SerializeShaderArtifact(MakeShaderArtifact(
            "Assets/Shaders/StandardPBR.shader",
            "shader:StandardPBR",
            MakeShaderLabNamedMaterialReflection())));
    static NLS::Core::ResourceManagement::ShaderManager shaderManager;
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::ShaderManager>(shaderManager);
    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(shaderArtifactPath.string());
    ASSERT_NE(shader, nullptr);
    shaderManager.RegisterResource(shaderArtifactPath.string(), shader);
    const auto shaderSourcePath = WriteShaderLabSourceBackedStandardPbrShaderArtifact(root);

    const auto materialPath = MaterialArtifactPath(root, "cd323ad76fbf5bdde05a2a1f6532dca53a037395afbe9b0fc1a8268fca276e1d");
    WriteNativeMaterialArtifactFile(
        materialPath,
        "shaderLabMaterialVersion=1\n"
        "shader=" + shaderSourcePath.generic_string() + "\n"
        "surfaceMode=Decaal\n");

    auto* loaded = NLS::Render::Resources::Loaders::MaterialLoader::Create(materialPath.string());
    EXPECT_EQ(loaded, nullptr);

    shaderManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialConversionReferencesAuthoritativeShaderLabSource)
{
    const std::string gltf = R"(
    {
      "asset": { "version": "2.0" },
      "materials": [{ "name": "HeroMaterial" }]
    })";

    const auto scene = NLS::Render::Assets::ImportGltfSceneJson(
        gltf,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("e1110101-0101-4101-8101-010101010102")),
        "Hero");
    ASSERT_EQ(scene.materials.size(), 1u);

    NLS::Render::Assets::MaterialConversionContext context;
    context.shaderResourcePath = "Assets/Shaders/StandardPBR.shader";
    const auto material = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        scene.materials.front(),
        MaterialSourceModel::GltfPbrMetallicRoughness,
        context);

    EXPECT_NE(
        material.serializedPayload.find("shader=Assets/Shaders/StandardPBR.shader"),
        std::string::npos);
    EXPECT_EQ(material.serializedPayload.find("shader=Library/Artifacts/"), std::string::npos);
}

TEST(AssetMaterialConversionTests, EditorMaterialCreationSourcesReferenceAuthoritativeStandardPbrShader)
{
    const auto read = [](const std::filesystem::path& relativePath)
    {
        std::ifstream input(std::filesystem::path(NLS_ROOT_DIR) / relativePath, std::ios::binary);
        EXPECT_TRUE(input) << relativePath.generic_string();
        return std::string{
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
    };

    const auto defaults = read("Project/Editor/Assets/ShaderLabMaterialDefaults.h");
    EXPECT_NE(
        defaults.find("Assets/Engine/Shaders/ShaderLab/StandardPBR.shader"),
        std::string::npos)
        << "New .mat assets must bind the authoritative ShaderLab StandardPBR source so prefab "
           "instantiation can resolve material pass artifacts through ArtifactDB.";
    EXPECT_EQ(defaults.find("shader=?"), std::string::npos)
        << "The default material template must never create a ShaderLab payload without a shader reference.";

    for (const auto& relativePath : {
             std::filesystem::path("Project/Editor/Panels/AssetBrowser.cpp"),
             std::filesystem::path("Project/Editor/Assets/AssetDragDropWorkflow.cpp")})
    {
        const auto source = read(relativePath);
        EXPECT_EQ(source.find("\"shader=?\\\\n\""), std::string::npos) << relativePath.generic_string();
        EXPECT_EQ(source.find("shader=?"), std::string::npos) << relativePath.generic_string();
    }
}

TEST(AssetMaterialConversionTests, ImportedModelMaterialsDefaultToDoubleSidedVisibility)
{
    const std::string gltf = R"(
    {
      "asset": { "version": "2.0" },
      "materials": [
        {
          "name": "OneSidedSource",
          "doubleSided": false
        }
      ]
    })";

    const auto scene = NLS::Render::Assets::ImportGltfSceneJson(
        gltf,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("e1110202-0202-4202-8202-020202020202")),
        "Hero");
    ASSERT_EQ(scene.materials.size(), 1u);

    const auto material = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        scene.materials.front(),
        MaterialSourceModel::GltfPbrMetallicRoughness);

    EXPECT_NE(material.serializedPayload.find("doubleSided=false"), std::string::npos);
    EXPECT_EQ(material.serializedPayload.find("doubleSided=true"), std::string::npos);
}

TEST(AssetMaterialConversionTests, EngineDefaultMaterialIsDoubleSidedForDeferredAssetVisibility)
{
    const auto defaultMaterialPath =
        std::filesystem::path(NLS_ROOT_DIR) / "App/Assets/Engine/Materials/Default.mat";

    std::ifstream input(defaultMaterialPath, std::ios::binary);
    const std::string payload{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(payload.empty());
    EXPECT_NE(payload.find("shaderLabMaterialVersion=1"), std::string::npos);
    EXPECT_NE(
        payload.find("shader=Assets/Engine/Shaders/ShaderLab/StandardPBR.shader"),
        std::string::npos);
    EXPECT_EQ(payload.find("Library/Artifacts/"), std::string::npos);
    EXPECT_NE(payload.find("doubleSided=true"), std::string::npos);
}

TEST(AssetMaterialConversionTests, ConvertedMaterialPayloadEscapesLineFieldsWithoutInjectingSchemaKeys)
{
    const std::string gltf = R"(
    {
      "asset": { "version": "2.0" },
      "materials": [
        {
          "name": "Hero\nsurfaceMode=Opaque\nproperty _Metallic Float 1",
          "alphaMode": "BLEND"
        }
      ]
    })";

    const auto scene = NLS::Render::Assets::ImportGltfSceneJson(
        gltf,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("e1110707-0707-4707-8707-070707070707")),
        "Hero");
    ASSERT_EQ(scene.materials.size(), 1u);

    const auto material = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        scene.materials.front(),
        MaterialSourceModel::GltfPbrMetallicRoughness);

    EXPECT_NE(
        material.serializedPayload.find("name=Hero%0AsurfaceMode%3DOpaque%0Aproperty%20_Metallic%20Float%201"),
        std::string::npos);
    EXPECT_EQ(material.serializedPayload.find("\nsurfaceMode=Opaque\n"), std::string::npos);
    EXPECT_EQ(material.serializedPayload.find("\nproperty _Metallic Float 1\n"), std::string::npos);
    EXPECT_NE(material.serializedPayload.find("surfaceMode=Transparent"), std::string::npos);
}

TEST(AssetMaterialConversionTests, DefaultWhiteTextureRecoversRhiHandleAfterHeadlessMaterialLoad)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_default_white_texture_headless_recover_" + NLS::Guid::New().ToString());
    const auto shaderArtifactPath = ArtifactPath(
        root,
        "2b40aa9d7e26302abaee46d90172b24a111dda5b6d466fcf2e7a2aff001a0607");
    WriteBinaryFile(
        shaderArtifactPath,
        NLS::Render::Assets::SerializeShaderArtifact(MakeShaderArtifact(
            "Assets/Shaders/HeadlessDefaultWhiteTexture.hlsl",
            "shader:HeadlessDefaultWhiteTexture",
            MakeAlbedoMapShaderReflection())));

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(shaderArtifactPath.string());
    ASSERT_NE(shader, nullptr);

    {
        const ScopedNoDriverService noDriverService;
        NLS::Render::Resources::Material headlessMaterial(shader);
        EXPECT_FALSE(headlessMaterial.HasExplicitBindingErrors());
    }

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    auto explicitDevice = std::make_shared<TextureLoaderTestDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    NLS::Render::Resources::Material material(shader);
    const auto& bindingSet = material.GetExplicitBindingSet(explicitDevice);

    ASSERT_NE(bindingSet, nullptr);
    ASSERT_EQ(explicitDevice->bindingSetCreateCalls, 1u);
    const auto textureEntry = std::find_if(
        explicitDevice->lastBindingSetDesc.entries.begin(),
        explicitDevice->lastBindingSetDesc.entries.end(),
        [](const NLS::Render::RHI::RHIBindingSetEntry& entry)
        {
            return entry.type == NLS::Render::RHI::BindingType::Texture &&
                entry.textureView != nullptr;
        });
    ASSERT_NE(textureEntry, explicitDevice->lastBindingSetDesc.entries.end());
    ASSERT_NE(textureEntry->textureView->GetTexture(), nullptr);
    EXPECT_EQ(textureEntry->textureView->GetTexture()->GetDesc().extent.width, 1u);
    EXPECT_EQ(textureEntry->textureView->GetTexture()->GetDesc().extent.height, 1u);
    EXPECT_TRUE(material.GetLastExplicitBindingDiagnostics().empty());
    EXPECT_FALSE(material.HasExplicitBindingErrors());

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, ShaderReflectionFallsBackToRuntimeCompileBackendWhenLocatedDriverHasNoRhi)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_shader_reflection_artifact_" + NLS::Guid::New().ToString());
    const auto shaderArtifactPath = ArtifactPath(
        root,
        "2b40aa9d7e26302abaee46d90172b24a111dda5b6d466fcf2e7a2aff001a0607");
    WriteBinaryFile(
        shaderArtifactPath,
        NLS::Render::Assets::SerializeShaderArtifact(MakeShaderArtifact(
            "Assets/Shaders/StandardPBR.shader",
            "shader:StandardPBR",
            MakeShaderLabNamedMaterialReflection())));

    static NLS::Render::Settings::DriverSettings settings = []()
    {
        NLS::Render::Settings::DriverSettings driverSettings;
        driverSettings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
        driverSettings.enableThreadedRendering = true;
        driverSettings.threadedFrameSlotCount = 1u;
        return driverSettings;
    }();
    static NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(shaderArtifactPath.string());
    ASSERT_NE(shader, nullptr);

    EXPECT_FALSE(shader->GetReflection().constantBuffers.empty());
    ASSERT_FALSE(shader->GetReflection().properties.empty());
    EXPECT_TRUE(shader->GetUniformInfo(shader->GetReflection().properties.front().name).has_value());

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, ConvertedMaterialPayloadLoadsAsRuntimeMaterialResource)
{
    const std::string gltf = R"(
    {
      "asset": { "version": "2.0" },
      "materials": [
        {
          "name": "HeroMaterial",
          "pbrMetallicRoughness": {
            "baseColorFactor": [0.2, 0.4, 0.6, 0.8],
            "metallicFactor": 0.3,
            "roughnessFactor": 0.7
          }
        }
      ]
    })";

    const auto scene = NLS::Render::Assets::ImportGltfSceneJson(
        gltf,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("e1120101-0101-4101-8101-010101010101")),
        "Hero");
    ASSERT_EQ(scene.materials.size(), 1u);

    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_converted_material_" + NLS::Guid::New().ToString());
    constexpr const char* shaderArtifactHash = "2b40aa9d7e26302abaee46d90172b24a111dda5b6d466fcf2e7a2aff001a0607";
    const auto shaderSourcePath = WriteShaderLabSourceBackedShaderArtifact(
        root,
        std::filesystem::path("Assets") / "Shaders" / "StandardPBR.shader",
        shaderArtifactHash,
        MakeShaderArtifact(
            "Assets/Shaders/StandardPBR.shader",
            "shader:StandardPBR/Forward#0",
            MakeShaderLabNamedMaterialReflection()),
        "StandardPBR Forward");
    const auto shaderArtifactPath = ArtifactPath(root, shaderArtifactHash);
    NLS::Render::Assets::MaterialConversionContext context;
    context.shaderResourcePath = shaderSourcePath.generic_string();
    const auto converted = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        scene.materials.front(),
        MaterialSourceModel::GltfPbrMetallicRoughness,
        context);
    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::ShaderManager>(shaderManager);

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    const auto materialPath = MaterialArtifactPath(root, "8a3fa59975ef635aeac76089dc7d962bd2c065ae524270fb7f01b790805fc1bf");
    WriteNativeMaterialArtifactFile(materialPath, converted.serializedPayload);

    auto* loaded = NLS::Render::Resources::Loaders::MaterialLoader::Create(materialPath.string());
    ASSERT_NE(loaded, nullptr);
    ASSERT_NE(loaded->GetShader(), nullptr);
    EXPECT_EQ(loaded->GetShader()->GetImportedArtifactSourcePath(), shaderSourcePath.generic_string());
    EXPECT_EQ(loaded->GetShader()->GetShaderLabLightMode(), "Forward");

    const auto* albedoValue = loaded->GetParameterBlock().TryGet("_BaseColor");
    ASSERT_NE(albedoValue, nullptr);
    ASSERT_EQ(albedoValue->type(), typeid(NLS::Maths::Vector4));
    const auto& albedo = std::any_cast<const NLS::Maths::Vector4&>(*albedoValue);
    EXPECT_FLOAT_EQ(albedo.x, 0.2f);
    EXPECT_FLOAT_EQ(albedo.y, 0.4f);
    EXPECT_FLOAT_EQ(albedo.z, 0.6f);
    EXPECT_FLOAT_EQ(albedo.w, 0.8f);
    EXPECT_FLOAT_EQ(loaded->Get<float>("_Metallic"), 0.3f);
    EXPECT_FLOAT_EQ(loaded->Get<float>("_Roughness"), 0.7f);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::MaterialLoader::Destroy(loaded));
    shaderManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialLoaderRejectsShaderArtifactReferenceEvenWhenArtifactExists)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_shader_artifact_material_" + NLS::Guid::New().ToString());
    const auto shaderArtifactResourcePath =
        LibraryArtifactPath("2b40aa9d7e26302abaee46d90172b24a111dda5b6d466fcf2e7a2aff001a0607");
    const auto shaderArtifactPath = ArtifactPath(
        root,
        "2b40aa9d7e26302abaee46d90172b24a111dda5b6d466fcf2e7a2aff001a0607");
    WriteNativeArtifactTextFile(
        shaderArtifactPath,
        NLS::Core::Assets::ArtifactType::Shader,
        "shader",
        1u,
        R"(NULLUS_IMPORTED_SHADER_ARTIFACT=1
SOURCE=Assets/Shaders/ArtifactShader.hlsl
SUB_ASSET=shader:ArtifactShader
TARGET_PLATFORM=editor
STAGE_BEGIN
STAGE=Vertex
TARGET=DXIL
ENTRY=VSMain
PROFILE=vs_6_0
STATUS=Succeeded
CACHE_KEY=test-vertex
ARTIFACT_PATH=)" + shaderArtifactResourcePath + R"(
BYTECODE_HEX=01020304
STAGE_END
STAGE_BEGIN
STAGE=Pixel
TARGET=DXIL
ENTRY=PSMain
PROFILE=ps_6_0
STATUS=Succeeded
CACHE_KEY=test-pixel
ARTIFACT_PATH=)" + shaderArtifactResourcePath + R"(
BYTECODE_HEX=05060708
STAGE_END
CBUFFER_BEGIN
NAME=MaterialConstants
STAGE=Pixel
SPACE=2
BINDING=0
BYTE_SIZE=64
MEMBER_BEGIN
NAME=_BaseColor
TYPE=vec4
BYTE_OFFSET=0
BYTE_SIZE=16
ARRAY_SIZE=1
MEMBER_END
CBUFFER_END
PROPERTY_BEGIN
NAME=_BaseColor
TYPE=vec4
KIND=Value
STAGE=Pixel
SPACE=2
BINDING=0
LOCATION=-1
ARRAY_SIZE=1
BYTE_OFFSET=0
BYTE_SIZE=16
PARENT_CBUFFER=MaterialConstants
PROPERTY_END
)");

    const auto materialPath = MaterialArtifactPath(root, "b1c25a2815424d17f20ef9571059b506e1b2894a8d346bf9550c157836d68b46");
    WriteNativeMaterialArtifactFile(
        materialPath,
        "shaderLabMaterialVersion=1\n"
        "shader=" + shaderArtifactResourcePath + "\n"
        "property _BaseColor Color 0.250000 0.500000 0.750000 1.000000\n");

    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    NLS::Core::ResourceManagement::TextureManager textureManager;
    const ScopedShaderManagerAssetPaths shaderAssetPaths(
        (root / "Assets").string() + "/",
        "App/Assets/Engine/");
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::ShaderManager>(shaderManager);
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::TextureManager>(textureManager);
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    auto* loaded = NLS::Render::Resources::Loaders::MaterialLoader::Create(materialPath.string());
    EXPECT_EQ(loaded, nullptr)
        << ".mat files reference .shader sources; ArtifactDB resolves imported shader payloads.";
    shaderManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::TextureManager>();
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, ShaderArtifactPreservesShaderLabPassState)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_shaderlab_pass_state_artifact_" + NLS::Guid::New().ToString());
    const auto shaderArtifactPath = ArtifactPath(
        root,
        NLS::Core::Assets::BuildArtifactStorageFileName("shaderlab-pass-state"));

    auto artifact = MakeStandardPbrShaderArtifact();
    artifact.shaderLabLightMode = "Forward";
    artifact.shaderLabPassState = MakeTransparentShaderLabPassState();
    WriteBinaryFile(shaderArtifactPath, NLS::Render::Assets::SerializeShaderArtifact(artifact));

    const auto loadedArtifact = NLS::Render::Assets::LoadShaderArtifact(shaderArtifactPath);
    ASSERT_TRUE(loadedArtifact.has_value());
    ASSERT_TRUE(loadedArtifact->shaderLabPassState.has_value());
    EXPECT_EQ(loadedArtifact->shaderLabLightMode, "Forward");
    EXPECT_EQ(loadedArtifact->shaderLabPassState->cullMode, NLS::Render::ShaderLab::ShaderLabCullMode::Front);
    EXPECT_FALSE(loadedArtifact->shaderLabPassState->depthWrite);
    EXPECT_EQ(
        loadedArtifact->shaderLabPassState->depthCompare,
        NLS::Render::Settings::EComparaisonAlgorithm::ALWAYS);
    ASSERT_EQ(loadedArtifact->shaderLabPassState->blend.renderTargets.size(), 1u);
    EXPECT_TRUE(loadedArtifact->shaderLabPassState->blend.enabled);
    EXPECT_TRUE(loadedArtifact->shaderLabPassState->blend.renderTargets[0].blendEnable);

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(shaderArtifactPath.string());
    ASSERT_NE(shader, nullptr);
    ASSERT_TRUE(shader->GetShaderLabPassState().has_value());
    EXPECT_EQ(shader->GetShaderLabLightMode(), "Forward");
    EXPECT_EQ(shader->GetShaderLabPassState()->cullMode, NLS::Render::ShaderLab::ShaderLabCullMode::Front);
    EXPECT_FALSE(shader->GetShaderLabPassState()->depthWrite);
    EXPECT_EQ(
        shader->GetShaderLabPassState()->depthCompare,
        NLS::Render::Settings::EComparaisonAlgorithm::ALWAYS);
    ASSERT_EQ(shader->GetShaderLabPassState()->blend.renderTargets.size(), 1u);
    EXPECT_TRUE(shader->GetShaderLabPassState()->blend.renderTargets[0].blendEnable);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, ConvertedMaterialPayloadLoadsDeclaredTextureSamplers)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_material_texture_load_" + NLS::Guid::New().ToString());
    const auto materialPath = MaterialArtifactPath(root, "d4fe16a6a775c5a201978a3430d0e4ca1849af34caee64e82c89f24a34d85553");
    const auto png = TinyPng();
    auto textureArtifact = NLS::Render::Assets::DecodeTextureArtifactFromEncodedImage(
        png.data(),
        png.size(),
        NLS::Render::Assets::TextureArtifactColorSpace::Srgb,
        true);
    ASSERT_TRUE(textureArtifact.has_value());
    const auto textureBytes = NLS::Render::Assets::SerializeTextureArtifact(*textureArtifact);
    const auto texturePath = WriteTextureArtifactBytes(root, textureBytes);

    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    NLS::Core::ResourceManagement::TextureManager textureManager;
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::ShaderManager>(shaderManager);
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::TextureManager>(textureManager);

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    const auto shaderArtifactPath = WriteStandardPbrShaderArtifact(root);
    const auto shaderSourcePath = WriteShaderLabSourceBackedStandardPbrShaderArtifact(root);
    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(shaderArtifactPath.string());
    ASSERT_NE(shader, nullptr);
    shaderManager.RegisterResource(shaderArtifactPath.string(), shader);

    const auto textureResourcePath = texturePath.lexically_normal().generic_string();
    const std::string payload =
        "shaderLabMaterialVersion=1\n"
        "shader=" + shaderSourcePath.generic_string() + "\n"
        "property _BaseMap Texture2D " + textureResourcePath + "\n";
    WriteNativeMaterialArtifactFile(materialPath, payload);

    auto* skippedTextures = NLS::Render::Resources::Loaders::MaterialLoader::Create(
        materialPath.string(),
        {false});
    ASSERT_NE(skippedTextures, nullptr);
    const auto* skippedAlbedoMap = skippedTextures->GetParameterBlock().TryGet("_BaseMap");
    ASSERT_NE(skippedAlbedoMap, nullptr);
    ASSERT_EQ(skippedAlbedoMap->type(), typeid(NLS::Render::Resources::Texture2D*));
    EXPECT_EQ(std::any_cast<NLS::Render::Resources::Texture2D*>(*skippedAlbedoMap), nullptr);
    EXPECT_EQ(skippedTextures->GetTextureResourcePath("_BaseMap"), textureResourcePath);
    EXPECT_FALSE(textureManager.IsResourceRegistered(textureResourcePath));
    EXPECT_TRUE(NLS::Render::Resources::Loaders::MaterialLoader::Destroy(skippedTextures));

    auto* loadedTextures = NLS::Render::Resources::Loaders::MaterialLoader::Create(
        materialPath.string(),
        {true});
    ASSERT_NE(loadedTextures, nullptr);
    const auto* loadedAlbedoMap = loadedTextures->GetParameterBlock().TryGet("_BaseMap");
    ASSERT_NE(loadedAlbedoMap, nullptr);
    ASSERT_EQ(loadedAlbedoMap->type(), typeid(NLS::Render::Resources::Texture2D*));
    EXPECT_NE(std::any_cast<NLS::Render::Resources::Texture2D*>(*loadedAlbedoMap), nullptr);
    EXPECT_EQ(loadedTextures->GetTextureResourcePath("_BaseMap"), textureResourcePath);
    EXPECT_TRUE(textureManager.IsResourceRegistered(textureResourcePath));

    EXPECT_TRUE(NLS::Render::Resources::Loaders::MaterialLoader::Destroy(loadedTextures));
    shaderManager.UnloadResources();
    textureManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::TextureManager>();
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialLoaderKeepsDistinctTextureSlotsWhenTextureLoadingIsDeferred)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_material_texture_slots_deferred_" + NLS::Guid::New().ToString());
    const auto materialPath = MaterialArtifactPath(root, "e8b42e5e7b582f90c696aa7db79ad50b08d22ebe74701862f3aee23d2dedce8c");

    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    NLS::Core::ResourceManagement::TextureManager textureManager;
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::ShaderManager>(shaderManager);
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::TextureManager>(textureManager);

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    const auto shaderArtifactPath = WriteStandardPbrShaderArtifact(root);
    const auto shaderSourcePath = WriteShaderLabSourceBackedStandardPbrShaderArtifact(root);
    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(shaderArtifactPath.string());
    ASSERT_NE(shader, nullptr);
    shaderManager.RegisterResource(shaderArtifactPath.string(), shader);

    const std::string albedoPath = (root / "Textures" / "HeroBaseColor.png").lexically_normal().generic_string();
    const std::string normalPath = (root / "Textures" / "HeroNormal.png").lexically_normal().generic_string();
    const std::string metalRoughPath = (root / "Textures" / "HeroMetalRough.png").lexically_normal().generic_string();
    const std::string payload =
        "shaderLabMaterialVersion=1\n"
        "shader=" + shaderSourcePath.generic_string() + "\n"
        "property _BaseMap Texture2D " + albedoPath + "\n"
        "property _NormalMap Texture2D " + normalPath + "\n"
        "property _MetallicMap Texture2D " + metalRoughPath + "\n"
        "property _RoughnessMap Texture2D " + metalRoughPath + "\n";
    WriteNativeMaterialArtifactFile(materialPath, payload);

    auto* material = NLS::Render::Resources::Loaders::MaterialLoader::Create(
        materialPath.string(),
        {false, true});
    ASSERT_NE(material, nullptr);

    EXPECT_EQ(material->GetTextureResourcePath("_BaseMap"), albedoPath);
    EXPECT_EQ(material->GetTextureResourcePath("_NormalMap"), normalPath);
    EXPECT_EQ(material->GetTextureResourcePath("_MetallicMap"), metalRoughPath);
    EXPECT_EQ(material->GetTextureResourcePath("_RoughnessMap"), metalRoughPath);
    EXPECT_NE(material->GetTextureResourcePath("_BaseMap"), material->GetTextureResourcePath("_NormalMap"));
    EXPECT_FALSE(textureManager.IsResourceRegistered(albedoPath));
    EXPECT_FALSE(textureManager.IsResourceRegistered(normalPath));
    EXPECT_FALSE(textureManager.IsResourceRegistered(metalRoughPath));

    EXPECT_TRUE(NLS::Render::Resources::Loaders::MaterialLoader::Destroy(material));
    shaderManager.UnloadResources();
    textureManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::TextureManager>();
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialLoaderReadsShaderLabKeywords)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_material_keywords_" + NLS::Guid::New().ToString());
    const auto materialPath = MaterialArtifactPath(root, "d7200cd5e3c49f03e794a63a5d6b53f0ac550a47e826ed439bfb2d1f89e00f88");

    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    NLS::Core::ResourceManagement::TextureManager textureManager;
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::ShaderManager>(shaderManager);
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::TextureManager>(textureManager);

    const std::string payload =
        "shaderLabMaterialVersion=1\n"
        "shader=Assets/Shaders/StandardPBR.shader\n"
        "keyword _NORMALMAP\n"
        "keyword _ALPHATEST_ON\n";
    WriteNativeMaterialArtifactFile(materialPath, payload);

    auto* material = NLS::Render::Resources::Loaders::MaterialLoader::Create(
        materialPath.string(),
        {false, false});
    ASSERT_NE(material, nullptr);

    EXPECT_TRUE(material->IsKeywordEnabled("_ALPHATEST_ON"));
    EXPECT_TRUE(material->IsKeywordEnabled("_NORMALMAP"));
    EXPECT_FALSE(material->IsKeywordEnabled("_EMISSION"));
    EXPECT_EQ(material->GetShaderLabKeywordNames(),
        (std::vector<std::string>{"_ALPHATEST_ON", "_NORMALMAP"}));

    EXPECT_TRUE(NLS::Render::Resources::Loaders::MaterialLoader::Destroy(material));
    shaderManager.UnloadResources();
    textureManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::TextureManager>();
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialLoaderAppliesShaderLabTextureSlotSamplerMetadataToNativeSampler)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_material_texture_slot_sampler_" + NLS::Guid::New().ToString());
    const auto materialPath = MaterialArtifactPath(root, "9a52a3d065ae95d00ee6dcf5c04fe549b84f7cc5eca50fa633a37f8290d62e05");

    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    NLS::Core::ResourceManagement::TextureManager textureManager;
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::ShaderManager>(shaderManager);
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::TextureManager>(textureManager);

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    constexpr const char* shaderArtifactHash = "2b40aa9d7e26302abaee46d90172b24a111dda5b6d466fcf2e7a2aff001a0607";
    const auto shaderSourcePath = WriteShaderLabSourceBackedShaderArtifact(
        root,
        std::filesystem::path("Assets") / "Shaders" / "StandardPBR.shader",
        shaderArtifactHash,
        MakeBaseMapWithSamplerShaderArtifact(),
        "StandardPBR Forward");
    const auto shaderArtifactPath = ArtifactPath(root, shaderArtifactHash);
    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(shaderArtifactPath.string());
    ASSERT_NE(shader, nullptr);
    shaderManager.RegisterResource(shaderArtifactPath.string(), shader);

    const std::string texturePath = (root / "Textures" / "HeroBaseColor.png")
        .lexically_normal()
        .generic_string();
    const std::string payload =
        "shaderLabMaterialVersion=1\n"
        "shader=" + shaderSourcePath.generic_string() + "\n"
        "property _BaseMap Texture2D " + texturePath + "\n"
        "textureSlot _BaseMap texture=image/0 resourcePath=" + texturePath +
        " colorSpace=SRgb wrapS=ClampToEdge wrapT=MirrorRepeat minFilter=Nearest magFilter=Nearest\n";
    WriteNativeMaterialArtifactFile(materialPath, payload);

    auto* material = NLS::Render::Resources::Loaders::MaterialLoader::Create(
        materialPath.string(),
        {false, true});
    ASSERT_NE(material, nullptr);

    const auto* sampler = material->GetBindingSet().GetSampler("sampler_BaseMap");
    ASSERT_NE(sampler, nullptr);
    EXPECT_EQ(sampler->wrapU, NLS::Render::RHI::TextureWrap::ClampToEdge);
    EXPECT_EQ(sampler->wrapV, NLS::Render::RHI::TextureWrap::MirrorRepeat);
    EXPECT_EQ(sampler->minFilter, NLS::Render::RHI::TextureFilter::Nearest);
    EXPECT_EQ(sampler->magFilter, NLS::Render::RHI::TextureFilter::Nearest);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::MaterialLoader::Destroy(material));
    shaderManager.UnloadResources();
    textureManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::TextureManager>();
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialManagerCoalescesDuplicateAsyncArtifactRequests)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect async material request state.";
#else
    using NLS::Core::ResourceManagement::MaterialManager;

    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_material_async_coalescing_" + NLS::Guid::New().ToString());
    const auto materialPath = MaterialArtifactPath(root, "03442e14a6314368560968cb6a1528199a45d0fe5d5407533fb7a2a7382c3f01");
    WriteMinimalShaderLabMaterialArtifactFile(materialPath);

    MaterialManager::ClearAsyncArtifactRequestStateForTesting();
    MaterialManager materialManager;
    EXPECT_EQ(MaterialManager::GetPendingAsyncArtifactRequestCountForTesting(), 0u);

    EXPECT_EQ(materialManager.RequestAsyncArtifact(materialPath.string()), nullptr);
    EXPECT_TRUE(materialManager.IsAsyncArtifactLoadPending(materialPath.string()));
    EXPECT_EQ(MaterialManager::GetTotalAsyncArtifactRequestCountForTesting(), 1u);

    EXPECT_EQ(materialManager.RequestAsyncArtifact(materialPath.string()), nullptr);
    EXPECT_EQ(MaterialManager::GetTotalAsyncArtifactRequestCountForTesting(), 1u)
        << "Duplicate material artifact requests must coalesce onto the in-flight load.";

    MaterialManager::ClearAsyncArtifactRequestStateForTesting();
    std::filesystem::remove_all(root);
#endif
}

TEST(AssetMaterialConversionTests, MaterialManagerBoundsPendingAsyncArtifactRequests)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect async material request state.";
#else
    using NLS::Core::ResourceManagement::MaterialManager;

    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_material_async_bound_" + NLS::Guid::New().ToString());
    MaterialManager::ClearAsyncArtifactRequestStateForTesting();
    MaterialManager materialManager;

    for (size_t index = 0u; index < 80u; ++index)
    {
        const auto materialPath = MaterialArtifactPath(
            root,
            IndexedArtifactHash("pending-material", index));
        WriteMinimalShaderLabMaterialArtifactFile(materialPath);
        EXPECT_EQ(materialManager.RequestAsyncArtifact(materialPath.string()), nullptr);
    }

    EXPECT_EQ(MaterialManager::GetMaxPendingAsyncArtifactRequestCountForTesting(), 32u);
    EXPECT_LE(
        MaterialManager::GetPendingAsyncArtifactRequestCountForTesting(),
        MaterialManager::GetMaxPendingAsyncArtifactRequestCountForTesting())
        << "Async material artifact scheduling must be bounded so large prefab drags cannot spawn unbounded std::async work.";
    EXPECT_TRUE(materialManager.IsAsyncArtifactLoadPending(MaterialArtifactPath(root, IndexedArtifactHash("pending-material", 79u)).string()))
        << "Requests beyond the active material cap must remain pending so large prefab finalization retries instead of failing.";

    MaterialManager::ClearAsyncArtifactRequestStateForTesting();
    std::filesystem::remove_all(root);
#endif
}

TEST(AssetMaterialConversionTests, MaterialManagerBoundsTotalQueuedAsyncArtifactRequests)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect async material request state.";
#else
    using NLS::Core::ResourceManagement::MaterialManager;

    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_material_async_total_bound_" + NLS::Guid::New().ToString());
    MaterialManager::ClearAsyncArtifactRequestStateForTesting();
    MaterialManager materialManager;

    for (size_t index = 0u; index < 320u; ++index)
    {
        const auto materialPath = MaterialArtifactPath(
            root,
            IndexedArtifactHash("queued-material", index));
        WriteMinimalShaderLabMaterialArtifactFile(materialPath);
        EXPECT_EQ(materialManager.RequestAsyncArtifact(materialPath.string()), nullptr);
    }

    EXPECT_LE(MaterialManager::GetTotalAsyncArtifactRequestCountForTesting(), 256u)
        << "Large prefab drags must not enqueue an unbounded number of material artifact records.";

    MaterialManager::ClearAsyncArtifactRequestStateForTesting();
    std::filesystem::remove_all(root);
#endif
}

TEST(AssetMaterialConversionTests, MaterialManagerBoundsGlobalActiveAsyncArtifactRequestsAcrossOwners)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect async material request state.";
#else
    using NLS::Core::ResourceManagement::MaterialManager;

    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_material_async_global_active_" + NLS::Guid::New().ToString());
    MaterialManager::ClearAsyncArtifactRequestStateForTesting();

    std::vector<std::unique_ptr<MaterialManager>> materialManagers;
    materialManagers.reserve(80u);
    for (size_t index = 0u; index < 80u; ++index)
    {
        auto materialManager = std::make_unique<MaterialManager>();
        const auto materialPath = MaterialArtifactPath(
            root,
            IndexedArtifactHash("global-material", index));
        WriteMinimalShaderLabMaterialArtifactFile(materialPath);
        EXPECT_EQ(materialManager->RequestAsyncArtifact(materialPath.string()), nullptr);
        materialManagers.push_back(std::move(materialManager));
    }

    EXPECT_EQ(MaterialManager::GetMaxPendingAsyncArtifactRequestCountForTesting(), 32u);
    EXPECT_LE(
        MaterialManager::GetPendingAsyncArtifactRequestCountForTesting(),
        MaterialManager::GetMaxPendingAsyncArtifactRequestCountForTesting())
        << "Material artifact loading must use a global active cap so many preview owners cannot fill the editor background queue.";
    EXPECT_EQ(MaterialManager::GetTotalAsyncArtifactRequestCountForTesting(), 80u);

    MaterialManager::ClearAsyncArtifactRequestStateForTesting();
    std::filesystem::remove_all(root);
#endif
}

TEST(AssetMaterialConversionTests, MaterialManagerCancelAsyncArtifactDoesNotConsumeSharedInterest)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect async material request state.";
#else
    using NLS::Core::ResourceManagement::MaterialManager;

    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_material_async_cancel_shared_" + NLS::Guid::New().ToString());
    const auto materialPath = MaterialArtifactPath(root, "fa123c1b71823847f07518473ec7d4186dc647a432199e87291c6dcc1afe0ca0");
    WriteMinimalShaderLabMaterialArtifactFile(materialPath);

    MaterialManager::ClearAsyncArtifactRequestStateForTesting();
    MaterialManager materialManager;
    EXPECT_EQ(materialManager.RequestAsyncArtifact(materialPath.string(), false), nullptr);
    EXPECT_EQ(materialManager.RequestAsyncArtifact(materialPath.string(), true), nullptr);
    materialManager.CancelAsyncArtifact(materialPath.string());
    materialManager.CancelAsyncArtifact(materialPath.string());
    EXPECT_TRUE(materialManager.IsAsyncArtifactLoadPending(materialPath.string()))
        << "Preview cleanup must not cancel shared/final material interest by repeating path-only cancellation.";

    MaterialManager::ClearAsyncArtifactRequestStateForTesting();
    std::filesystem::remove_all(root);
#endif
}

TEST(AssetMaterialConversionTests, MaterialManagerCancelAsyncArtifactCanReleaseSharedInterest)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect async material request state.";
#else
    using NLS::Core::ResourceManagement::MaterialManager;

    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_material_async_cancel_explicit_shared_" + NLS::Guid::New().ToString());
    MaterialManager::ClearAsyncArtifactRequestStateForTesting();
    MaterialManager materialManager;

    std::filesystem::path queuedMaterialPath;
    for (size_t index = 0u; index < 80u; ++index)
    {
        const auto materialPath = MaterialArtifactPath(
            root,
            IndexedArtifactHash("explicit-shared-material", index));
        WriteMinimalShaderLabMaterialArtifactFile(materialPath);
        EXPECT_EQ(materialManager.RequestAsyncArtifact(materialPath.string(), false), nullptr);
        queuedMaterialPath = materialPath;
    }

    ASSERT_TRUE(materialManager.IsAsyncArtifactLoadPending(queuedMaterialPath.string()));
    materialManager.CancelAsyncArtifact(queuedMaterialPath.string(), false);
    EXPECT_FALSE(materialManager.IsAsyncArtifactLoadPending(queuedMaterialPath.string()))
        << "Scene-load cleanup must be able to release shared material interest that it registered.";

    MaterialManager::ClearAsyncArtifactRequestStateForTesting();
    std::filesystem::remove_all(root);
#endif
}

TEST(AssetMaterialConversionTests, MaterialManagerClearAsyncArtifactStateDrainsWorkersForTesting)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect async material request state.";
#else
    using NLS::Core::ResourceManagement::MaterialManager;

    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_material_async_clear_drains_" + NLS::Guid::New().ToString());
    MaterialManager::ClearAsyncArtifactRequestStateForTesting();
    MaterialManager materialManager;
    for (size_t index = 0u; index < 8u; ++index)
    {
        const auto materialPath = MaterialArtifactPath(
            root,
            IndexedArtifactHash("clear-material", index));
        WriteMinimalShaderLabMaterialArtifactFile(materialPath);
        EXPECT_EQ(materialManager.RequestAsyncArtifact(materialPath.string()), nullptr);
    }

    MaterialManager::ClearAsyncArtifactRequestStateForTesting();
    EXPECT_TRUE(MaterialManager::WaitForAsyncArtifactWorkersForTesting());
    std::filesystem::remove_all(root);
#endif
}

TEST(AssetMaterialConversionTests, MaterialManagerPreviewAsyncArtifactReturnsMaterialBeforeTexturesResolve)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect async material request state.";
#else
    using NLS::Core::ResourceManagement::MaterialManager;

    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_material_preview_async_deferred_textures_" + NLS::Guid::New().ToString());
    const auto projectAssets = root / "Assets";
    const auto materialResourcePath = LibraryArtifactPath("47b24ab4b128645b99328e0a68370de1202b0ba370eafc30e8bb0b0b7cf8b5ae");
    const auto materialPath = root / materialResourcePath;
    const auto textureResourcePath = LibraryArtifactPath("f51fc4f93fdfaeb9d91abfc64a3296734c991105b8782f8a4aa617684c5d109b");
    const auto shaderSourcePath = WriteShaderLabSourceBackedShaderArtifact(
        root,
        std::filesystem::path("Assets") / "Shaders" / "PreviewDeferredTexture.shader",
        "2b40aa9d7e26302abaee46d90172b24a111dda5b6d466fcf2e7a2aff001a0607",
        MakeAlbedoMapShaderArtifact(),
        "PreviewDeferredTexture Forward");

    WriteNativeMaterialArtifactFile(
        materialPath,
        "shaderLabMaterialVersion=1\n"
        "shader=" + shaderSourcePath.generic_string() + "\n"
        "property _BaseMap Texture2D " + textureResourcePath + "\n");

    const ScopedMaterialConversionJobSystem jobSystem;
    ASSERT_TRUE(jobSystem.IsInitialized());

    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    NLS::Core::ResourceManagement::TextureManager textureManager;
    MaterialManager materialManager;
    const ScopedShaderManagerAssetPaths shaderAssetPaths(
        projectAssets.string() + "/",
        "App/Assets/Engine/");
    MaterialManager::ProvideAssetPaths(projectAssets.string() + "/", "App/Assets/Engine/");
    NLS::Core::ResourceManagement::TextureManager::ProvideAssetPaths(
        projectAssets.string() + "/",
        "App/Assets/Engine/");
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::ShaderManager>(shaderManager);
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::TextureManager>(textureManager);

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    MaterialManager::ClearAsyncArtifactRequestStateForTesting();
    EXPECT_EQ(materialManager.RequestAsyncArtifactForPreview(materialPath.string()), nullptr);
    for (size_t attempt = 0u; attempt < 256u && !materialManager.IsResourceRegistered(materialPath.string()); ++attempt)
    {
        materialManager.PumpAsyncLoadsForPaths({materialPath.string()}, 1u);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    auto* material = materialManager.RequestAsyncArtifactForPreview(materialPath.string());
    ASSERT_NE(material, nullptr);
    EXPECT_TRUE(materialManager.IsResourceRegistered(materialPath.string()));
    EXPECT_EQ(material->GetTextureResourcePath("_BaseMap"), textureResourcePath);
    const auto* albedoMap = material->GetParameterBlock().TryGet("_BaseMap");
    ASSERT_NE(albedoMap, nullptr);
    ASSERT_EQ(albedoMap->type(), typeid(NLS::Render::Resources::Texture2D*));
    EXPECT_EQ(std::any_cast<NLS::Render::Resources::Texture2D*>(*albedoMap), nullptr)
        << "Preview code must be able to inspect material texture paths before texture artifacts finish loading.";

    materialManager.UnloadResources();
    shaderManager.UnloadResources();
    textureManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::TextureManager>();
    MaterialManager::ProvideAssetPaths({}, {});
    NLS::Core::ResourceManagement::TextureManager::ProvideAssetPaths({}, {});
    MaterialManager::ClearAsyncArtifactRequestStateForTesting();
    std::filesystem::remove_all(root);
#endif
}

TEST(AssetMaterialConversionTests, MaterialManagerAsyncArtifactRegistersShaderLabMaterialWithShader)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect async material request state.";
#else
    using NLS::Core::ResourceManagement::MaterialManager;

    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_material_async_shaderlab_shader_" + NLS::Guid::New().ToString());
    const auto projectAssets = root / "Assets";
    const auto materialResourcePath =
        LibraryArtifactPath("4b0b69b04c6af9c428efcf5b857c6cf1576804a6a1212995f9b469a4a187d7b8");
    const auto materialPath = root / materialResourcePath;
    const auto shaderSourcePath = WriteShaderLabSourceBackedShaderArtifact(
        root,
        std::filesystem::path("Assets") / "Shaders" / "AsyncShaderLabMaterial.shader",
        "5b670b2d4aab273b6e256f3e94b0e16037a19a1c34be235ed9a658b767c0c4e1",
        MakeStandardPbrShaderArtifact(),
        "AsyncShaderLabMaterial Forward");

    WriteNativeMaterialArtifactFile(
        materialPath,
        "shaderLabMaterialVersion=1\n"
        "shader=" + shaderSourcePath.generic_string() + "\n"
        "surfaceMode=Opaque\n");

    const ScopedMaterialConversionJobSystem jobSystem;
    ASSERT_TRUE(jobSystem.IsInitialized());

    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    NLS::Core::ResourceManagement::TextureManager textureManager;
    MaterialManager materialManager;
    const ScopedShaderManagerAssetPaths shaderAssetPaths(
        projectAssets.string() + "/",
        "App/Assets/Engine/");
    MaterialManager::ProvideAssetPaths(projectAssets.string() + "/", "App/Assets/Engine/");
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::ShaderManager>(shaderManager);
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::TextureManager>(textureManager);

    MaterialManager::ClearAsyncArtifactRequestStateForTesting();
    EXPECT_EQ(materialManager.RequestAsyncArtifact(materialPath.string()), nullptr);
    for (size_t attempt = 0u; attempt < 256u && !materialManager.IsResourceRegistered(materialPath.string()); ++attempt)
    {
        materialManager.PumpAsyncLoadsForPaths({materialPath.string()}, 1u);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    auto* material = materialManager.RequestAsyncArtifact(materialPath.string());
    ASSERT_NE(material, nullptr);
    ASSERT_TRUE(material->IsValid())
        << "Async material artifact completion must not cache a ShaderLab material without its pass shader.";
    auto* forwardShader = material->ResolveShaderForLightMode("Forward");
    ASSERT_NE(forwardShader, nullptr);
    EXPECT_EQ(forwardShader, material->GetShader());
    EXPECT_EQ(forwardShader->GetImportedArtifactSourcePath(), shaderSourcePath.generic_string());

    materialManager.UnloadResources();
    shaderManager.UnloadResources();
    textureManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::TextureManager>();
    MaterialManager::ProvideAssetPaths({}, {});
    MaterialManager::ClearAsyncArtifactRequestStateForTesting();
    std::filesystem::remove_all(root);
#endif
}

TEST(AssetMaterialConversionTests, MaterialManagerAsyncArtifactDoesNotCacheInvalidShaderLabMaterial)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect async material request state.";
#else
    using NLS::Core::ResourceManagement::MaterialManager;

    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_material_async_invalid_shaderlab_" + NLS::Guid::New().ToString());
    const auto materialPath = MaterialArtifactPath(
        root,
        "40c4f8e299d86e7a01eb00b97384200ed589dd94c5fef13de750d27955c63c92");
    WriteNativeMaterialArtifactFile(
        materialPath,
        "shaderLabMaterialVersion=1\n"
        "shader=Assets/Shaders/Missing.shader\n"
        "surfaceMode=Opaque\n");

    const ScopedMaterialConversionJobSystem jobSystem;
    ASSERT_TRUE(jobSystem.IsInitialized());

    MaterialManager materialManager;
    MaterialManager::ClearAsyncArtifactRequestStateForTesting();
    EXPECT_EQ(materialManager.RequestAsyncArtifact(materialPath.string()), nullptr);
    for (size_t attempt = 0u;
        attempt < 256u && !materialManager.IsAsyncArtifactLoadFailed(materialPath.string());
        ++attempt)
    {
        materialManager.PumpAsyncLoadsForPaths({materialPath.string()}, 1u);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_FALSE(materialManager.IsResourceRegistered(materialPath.string()))
        << "Invalid ShaderLab materials must not poison the runtime material cache.";
    EXPECT_TRUE(materialManager.IsAsyncArtifactLoadFailed(materialPath.string()));
    EXPECT_EQ(materialManager.RequestAsyncArtifact(materialPath.string()), nullptr);

    materialManager.UnloadResources();
    MaterialManager::ClearAsyncArtifactRequestStateForTesting();
    std::filesystem::remove_all(root);
#endif
}

TEST(AssetMaterialConversionTests, MaterialReloadClearsPreviousTextureSlotSamplerMetadata)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_material_texture_slot_sampler_reload_" + NLS::Guid::New().ToString());
    const auto materialPath = MaterialArtifactPath(root, "71fdb6f93870cc78d3022f7dff25ef311f3cd7c277d74db3beb7116103bcfd76");

    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    NLS::Core::ResourceManagement::TextureManager textureManager;
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::ShaderManager>(shaderManager);
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::TextureManager>(textureManager);

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    constexpr const char* shaderArtifactHash = "2b40aa9d7e26302abaee46d90172b24a111dda5b6d466fcf2e7a2aff001a0607";
    const auto shaderSourcePath = WriteShaderLabSourceBackedShaderArtifact(
        root,
        std::filesystem::path("Assets") / "Shaders" / "StandardPBR.shader",
        shaderArtifactHash,
        MakeBaseMapWithSamplerShaderArtifact(),
        "StandardPBR Forward");
    const auto shaderArtifactPath = ArtifactPath(root, shaderArtifactHash);
    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(shaderArtifactPath.string());
    ASSERT_NE(shader, nullptr);
    shaderManager.RegisterResource(shaderArtifactPath.string(), shader);

    const std::string texturePath = (root / "Textures" / "HeroBaseColor.png")
        .lexically_normal()
        .generic_string();
    const std::string firstPayload =
        "shaderLabMaterialVersion=1\n"
        "shader=" + shaderSourcePath.generic_string() + "\n"
        "property _BaseMap Texture2D " + texturePath + "\n"
        "textureSlot _BaseMap texture=image/0 resourcePath=" + texturePath +
        " colorSpace=SRgb wrapS=ClampToEdge wrapT=ClampToEdge minFilter=Nearest magFilter=Nearest\n";
    WriteNativeMaterialArtifactFile(materialPath, firstPayload);

    auto* material = NLS::Render::Resources::Loaders::MaterialLoader::Create(
        materialPath.string(),
        {false, true});
    ASSERT_NE(material, nullptr);
    ASSERT_NE(material->GetBindingSet().GetSampler("sampler_BaseMap"), nullptr);
    EXPECT_EQ(
        material->GetBindingSet().GetSampler("sampler_BaseMap")->minFilter,
        NLS::Render::RHI::TextureFilter::Nearest);

    const std::string secondPayload =
        "shaderLabMaterialVersion=1\n"
        "shader=" + shaderSourcePath.generic_string() + "\n"
        "property _BaseMap Texture2D " + texturePath + "\n";
    WriteNativeMaterialArtifactFile(materialPath, secondPayload);
    NLS::Render::Resources::Loaders::MaterialLoader::Reload(*material, materialPath.string(), {false, true});

    EXPECT_EQ(material->GetSamplerOverride("sampler_BaseMap"), nullptr);
    const auto* forwardShader = material->ResolveShaderForLightMode("Forward");
    ASSERT_NE(forwardShader, nullptr);
    const auto reflection = forwardShader->GetReflectionSnapshot();
    ASSERT_NE(reflection, nullptr);
    EXPECT_TRUE(std::any_of(
        reflection->properties.begin(),
        reflection->properties.end(),
        [](const auto& property)
        {
            return property.name == "sampler_BaseMap" &&
                property.kind == NLS::Render::Resources::ShaderResourceKind::Sampler;
        }));

    EXPECT_TRUE(NLS::Render::Resources::Loaders::MaterialLoader::Destroy(material));
    shaderManager.UnloadResources();
    textureManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::TextureManager>();
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialLoaderDoesNotWarnWhenTextureLoadingIsIntentionallyDeferred)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_deferred_material_texture_load_" + NLS::Guid::New().ToString());
    const auto materialPath = MaterialArtifactPath(root, "c833b118a3fef68c5b64bdecc889df3b6fe5d7b138ab1975372579bd45944b8a");
    const auto texturePath = root / "Textures" / "HeroBaseColor.png";
    WriteBinaryFile(texturePath, TinyPng());

    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    NLS::Core::ResourceManagement::TextureManager textureManager;
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::ShaderManager>(shaderManager);
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::TextureManager>(textureManager);

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    const auto shaderArtifactPath = WriteStandardPbrShaderArtifact(root);
    const auto shaderSourcePath = WriteShaderLabSourceBackedStandardPbrShaderArtifact(root);
    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(shaderArtifactPath.string());
    ASSERT_NE(shader, nullptr);
    shaderManager.RegisterResource(shaderArtifactPath.string(), shader);

    const auto textureResourcePath = texturePath.lexically_normal().generic_string();
    const std::string payload =
        "shaderLabMaterialVersion=1\n"
        "shader=" + shaderSourcePath.generic_string() + "\n"
        "property _BaseMap Texture2D " + textureResourcePath + "\n";
    WriteNativeMaterialArtifactFile(materialPath, payload);

    bool sawTextureFailureWarning = false;
    const auto listener = NLS::Debug::Logger::LogEvent +=
        [&sawTextureFailureWarning](const NLS::Debug::LogData& log)
        {
            if (log.logLevel == NLS::Debug::ELogLevel::LOG_WARNING &&
                log.message.find("Material texture failed to load") != std::string::npos)
            {
                sawTextureFailureWarning = true;
            }
        };

    auto* skippedTextures = NLS::Render::Resources::Loaders::MaterialLoader::Create(
        materialPath.string(),
        {false});

    NLS::Debug::Logger::LogEvent -= listener;

    ASSERT_NE(skippedTextures, nullptr);
    EXPECT_FALSE(sawTextureFailureWarning);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::MaterialLoader::Destroy(skippedTextures));
    shaderManager.UnloadResources();
    textureManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::TextureManager>();
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialLoaderCanKeepShaderResolutionCacheOnly)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_material_shader_deferred_" + NLS::Guid::New().ToString());
    const auto projectAssets = root / "Assets";
    const auto materialPath = MaterialArtifactPath(root, "4d908c6dd9a8e406a58f8b148f48c9c71e04a45195541ce5517ee5b0ad6f4aa8");
    const std::string shaderSourcePath = "Assets/Shaders/Deferred.shader";

    WriteNativeMaterialArtifactFile(
        materialPath,
        "shaderLabMaterialVersion=1\n"
        "shader=" + shaderSourcePath + "\n");

    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    const ScopedShaderManagerAssetPaths shaderAssetPaths(
        projectAssets.string() + "/",
        "App/Assets/Engine/");
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::ShaderManager>(shaderManager);

    auto* deferred = NLS::Render::Resources::Loaders::MaterialLoader::Create(
        materialPath.string(),
        {false, false});

    ASSERT_NE(deferred, nullptr);
    EXPECT_EQ(deferred->GetShader(), nullptr);
    EXPECT_FALSE(shaderManager.IsResourceRegistered(shaderSourcePath));

    EXPECT_TRUE(NLS::Render::Resources::Loaders::MaterialLoader::Destroy(deferred));
    shaderManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialLoaderPrefersShaderLabPropertyNamesOverLegacyUniformAliases)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_shaderlab_named_material_" + NLS::Guid::New().ToString());
    const auto shaderSourcePath = WriteShaderLabSourceBackedShaderArtifact(
        root,
        std::filesystem::path("Assets") / "Shaders" / "ShaderLabNamed.shader",
        "2b40aa9d7e26302abaee46d90172b24a111dda5b6d466fcf2e7a2aff001a0607",
        MakeShaderArtifact(
            "Assets/Shaders/ShaderLabNamed.shader",
            "shader:ShaderLabNamed",
            MakeShaderLabNamedMaterialReflection()),
        "ShaderLabNamed Forward");

    const auto materialPath = MaterialArtifactPath(root, "64f3f954e79d850ab72fd9f225a4d647958b0f3d36df33232dbf19719509587d");
    WriteNativeMaterialArtifactFile(
        materialPath,
        "shaderLabMaterialVersion=1\n"
        "shader=" + shaderSourcePath.generic_string() + "\n"
        "property _BaseColor Color 0.125000 0.250000 0.500000 1.000000\n"
        "property _BaseMap Texture2D " +
        LibraryArtifactPath("f51fc4f93fdfaeb9d91abfc64a3296734c991105b8782f8a4aa617684c5d109b") +
        "\n");

    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    NLS::Core::ResourceManagement::TextureManager textureManager;
    const ScopedShaderManagerAssetPaths shaderAssetPaths(
        (root / "Assets").string() + "/",
        "App/Assets/Engine/");
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::ShaderManager>(shaderManager);
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::TextureManager>(textureManager);
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    auto* loaded = NLS::Render::Resources::Loaders::MaterialLoader::Create(materialPath.string(), {false, true});
    ASSERT_NE(loaded, nullptr);
    ASSERT_NE(loaded->GetShader(), nullptr);

    const auto* baseColorValue = loaded->GetParameterBlock().TryGet("_BaseColor");
    ASSERT_NE(baseColorValue, nullptr);
    ASSERT_EQ(baseColorValue->type(), typeid(NLS::Maths::Vector4));
    const auto& baseColor = std::any_cast<const NLS::Maths::Vector4&>(*baseColorValue);
    EXPECT_FLOAT_EQ(baseColor.x, 0.125f);
    EXPECT_FLOAT_EQ(baseColor.y, 0.25f);
    EXPECT_FLOAT_EQ(baseColor.z, 0.5f);
    EXPECT_FLOAT_EQ(baseColor.w, 1.0f);
    EXPECT_EQ(loaded->GetParameterBlock().TryGet("u_Albedo"), nullptr);
    EXPECT_EQ(
        loaded->GetTextureResourcePath("_BaseMap"),
        LibraryArtifactPath("f51fc4f93fdfaeb9d91abfc64a3296734c991105b8782f8a4aa617684c5d109b"));
    EXPECT_TRUE(loaded->GetTextureResourcePath("u_AlbedoMap").empty());

    EXPECT_TRUE(NLS::Render::Resources::Loaders::MaterialLoader::Destroy(loaded));
    shaderManager.UnloadResources();
    textureManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::TextureManager>();
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialPrewarmDoesNotPoisonLaterShaderLoading)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_material_prewarm_reload_" + NLS::Guid::New().ToString());
    const auto projectAssets = root / "Assets";
    const auto shaderResourcePath = LibraryArtifactPath("2b40aa9d7e26302abaee46d90172b24a111dda5b6d466fcf2e7a2aff001a0607");
    const auto shaderArtifactPath = root / shaderResourcePath;
    const auto shaderSourcePath = std::filesystem::path("Assets") / "Shaders" / "Hero.shader";
    const auto materialResourcePath = LibraryArtifactPath("47b24ab4b128645b99328e0a68370de1202b0ba370eafc30e8bb0b0b7cf8b5ae");
    const auto materialPath = root / materialResourcePath;

    WriteNativeArtifactTextFile(
        shaderArtifactPath,
        NLS::Core::Assets::ArtifactType::Shader,
        "shader",
        1u,
        R"(NULLUS_IMPORTED_SHADER_ARTIFACT=1
SOURCE=Assets/Shaders/Hero.shader
SUB_ASSET=shader:Hero
TARGET_PLATFORM=editor
SHADERLAB_LIGHT_MODE=Forward
STAGE_BEGIN
STAGE=Vertex
TARGET=DXIL
ENTRY=VSMain
PROFILE=vs_6_0
STATUS=Succeeded
CACHE_KEY=test-vertex
ARTIFACT_PATH=Library/Artifacts/2b/2b40aa9d7e26302abaee46d90172b24a111dda5b6d466fcf2e7a2aff001a0607
BYTECODE_HEX=01020304
STAGE_END
STAGE_BEGIN
STAGE=Pixel
TARGET=DXIL
ENTRY=PSMain
PROFILE=ps_6_0
STATUS=Succeeded
CACHE_KEY=test-pixel
ARTIFACT_PATH=Library/Artifacts/2b/2b40aa9d7e26302abaee46d90172b24a111dda5b6d466fcf2e7a2aff001a0607
BYTECODE_HEX=05060708
STAGE_END
)");
    NLS::Core::Assets::ArtifactManifest shaderManifest;
    shaderManifest.sourceAssetId = NLS::Core::Assets::AssetId(NLS::Guid::NewDeterministic("Assets/Shaders/Hero.shader"));
    shaderManifest.importerId = "ShaderLabImporter";
    shaderManifest.importerVersion = 1u;
    shaderManifest.targetPlatform = "editor";
    shaderManifest.primarySubAssetKey = "shader:Hero";
    shaderManifest.subAssets.push_back({
        shaderManifest.sourceAssetId,
        "shader:Hero",
        NLS::Core::Assets::ArtifactType::Shader,
        "ShaderLoader",
        "editor",
        shaderResourcePath,
        "2b40aa9d7e26302abaee46d90172b24a111dda5b6d466fcf2e7a2aff001a0607",
        "Hero Forward"
    });
    NLS::Core::Assets::ArtifactDatabase shaderDatabase;
    shaderDatabase.UpsertManifest(
        shaderManifest,
        shaderSourcePath.generic_string(),
        NLS::Core::Assets::ArtifactRecordStatus::UpToDate);
    ASSERT_TRUE(shaderDatabase.Save(root / "Library" / "ArtifactDB"));
    WriteNativeArtifactTextFile(
        materialPath,
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        1u,
        "shaderLabMaterialVersion=1\n"
        "shader=" + shaderSourcePath.generic_string() + "\n" +
        "doubleSided=false\n");

    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    NLS::Core::ResourceManagement::TextureManager textureManager;
    NLS::Core::ResourceManagement::MaterialManager materialManager;
    const ScopedShaderManagerAssetPaths shaderAssetPaths(
        projectAssets.string() + "/",
        "App/Assets/Engine/");
    NLS::Core::ResourceManagement::MaterialManager::ProvideAssetPaths(
        projectAssets.string() + "/",
        "App/Assets/Engine/");
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::ShaderManager>(shaderManager);
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::TextureManager>(textureManager);

    const auto resourcePath = materialResourcePath;
    EXPECT_EQ(materialManager.PrewarmArtifact(resourcePath), nullptr);
    EXPECT_FALSE(materialManager.IsResourceRegistered(resourcePath));
    EXPECT_FALSE(shaderManager.IsResourceRegistered(shaderArtifactPath.string()));

    auto* loaded = materialManager.GetResource(resourcePath, true);
    ASSERT_NE(loaded, nullptr);
    EXPECT_NE(loaded->GetShader(), nullptr);
    EXPECT_TRUE(loaded->HasBackfaceCulling());
    EXPECT_FALSE(loaded->HasFrontfaceCulling());
    const auto registeredShaders = shaderManager.GetResources();
    EXPECT_TRUE(std::any_of(
        registeredShaders.begin(),
        registeredShaders.end(),
        [shader = loaded->GetShader()](const auto& entry)
        {
            return entry.second == shader;
        }));

    materialManager.UnloadResources();
    shaderManager.UnloadResources();
    textureManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::TextureManager>();
    NLS::Core::ResourceManagement::MaterialManager::ProvideAssetPaths({}, {});
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialArtifactCanLoadShaderWhileDeferringTextures)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_material_artifact_deferred_textures_" + NLS::Guid::New().ToString());
    const auto projectAssets = root / "Assets";
    const auto materialResourcePath = LibraryArtifactPath("47b24ab4b128645b99328e0a68370de1202b0ba370eafc30e8bb0b0b7cf8b5ae");
    const auto materialPath = root / materialResourcePath;
    const auto textureResourcePath = LibraryArtifactPath("f51fc4f93fdfaeb9d91abfc64a3296734c991105b8782f8a4aa617684c5d109b");
    const auto texturePath = root / textureResourcePath;
    const auto shaderSourcePath = WriteShaderLabSourceBackedShaderArtifact(
        root,
        std::filesystem::path("Assets") / "Shaders" / "AlbedoMap.shader",
        "2b40aa9d7e26302abaee46d90172b24a111dda5b6d466fcf2e7a2aff001a0607",
        MakeAlbedoMapShaderArtifact(),
        "AlbedoMap Forward");
    const std::string materialPayload =
        "shaderLabMaterialVersion=1\n"
        "shader=" + shaderSourcePath.generic_string() + "\n"
        "property _BaseMap Texture2D " + textureResourcePath + "\n";

    WriteNativeArtifactTextFile(
        materialPath,
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        1u,
        materialPayload);

    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    NLS::Core::ResourceManagement::TextureManager textureManager;
    NLS::Core::ResourceManagement::MaterialManager materialManager;
    const ScopedShaderManagerAssetPaths shaderAssetPaths(
        projectAssets.string() + "/",
        "App/Assets/Engine/");
    NLS::Core::ResourceManagement::MaterialManager::ProvideAssetPaths(
        projectAssets.string() + "/",
        "App/Assets/Engine/");
    NLS::Core::ResourceManagement::TextureManager::ProvideAssetPaths(
        projectAssets.string() + "/",
        "App/Assets/Engine/");
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::ShaderManager>(shaderManager);
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::TextureManager>(textureManager);

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    const auto resourcePath = materialResourcePath;
    auto* loaded = materialManager.LoadArtifactWithoutTextures(resourcePath);

    ASSERT_NE(loaded, nullptr);
    EXPECT_TRUE(materialManager.IsResourceRegistered(resourcePath));
    EXPECT_NE(loaded->GetShader(), nullptr);
    EXPECT_EQ(loaded->GetTextureResourcePath("_BaseMap"), textureResourcePath);
    EXPECT_FALSE(textureManager.IsResourceRegistered(textureResourcePath));
    const auto* albedoMap = loaded->GetParameterBlock().TryGet("_BaseMap");
    ASSERT_NE(albedoMap, nullptr);
    ASSERT_EQ(albedoMap->type(), typeid(NLS::Render::Resources::Texture2D*));
    EXPECT_EQ(std::any_cast<NLS::Render::Resources::Texture2D*>(*albedoMap), nullptr);

    materialManager.UnloadResources();
    shaderManager.UnloadResources();
    textureManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::TextureManager>();
    NLS::Core::ResourceManagement::MaterialManager::ProvideAssetPaths({}, {});
    NLS::Core::ResourceManagement::TextureManager::ProvideAssetPaths({}, {});
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialLoaderReusesEquivalentCachedTextureArtifact)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_material_equivalent_cached_texture_" + NLS::Guid::New().ToString());
    const auto projectAssets = root / "Assets";
    const auto shaderSourcePath = WriteShaderLabSourceBackedShaderArtifact(
        root,
        std::filesystem::path("Assets") / "Shaders" / "StandardPBR.shader",
        "2b40aa9d7e26302abaee46d90172b24a111dda5b6d466fcf2e7a2aff001a0607",
        MakeAlbedoMapShaderArtifact(),
        "StandardPBR Forward");
    const auto shaderArtifactPath = ArtifactPath(root, "2b40aa9d7e26302abaee46d90172b24a111dda5b6d466fcf2e7a2aff001a0607");
    const auto textureResourcePath = LibraryArtifactPath("f51fc4f93fdfaeb9d91abfc64a3296734c991105b8782f8a4aa617684c5d109b");

    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    NLS::Core::ResourceManagement::TextureManager textureManager;
    const ScopedShaderManagerAssetPaths shaderAssetPaths(
        projectAssets.string() + "/",
        "App/Assets/Engine/");
    NLS::Core::ResourceManagement::TextureManager::ProvideAssetPaths(
        projectAssets.string() + "/",
        "App/Assets/Engine/");
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::ShaderManager>(shaderManager);
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::TextureManager>(textureManager);

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(shaderArtifactPath.string());
    ASSERT_NE(shader, nullptr);
    shaderManager.RegisterResource(shaderArtifactPath.string(), shader);

    const auto absoluteTexturePath =
        NLS::Core::ResourceManagement::TextureManager::ResolveResourcePath(textureResourcePath);
    auto* cachedTexture = NLS::Render::Resources::Loaders::TextureLoader::CreatePixel(32u, 48u, 64u, 255u);
    ASSERT_NE(cachedTexture, nullptr);
    cachedTexture->path = absoluteTexturePath;
    textureManager.RegisterResource(absoluteTexturePath, cachedTexture);

    const std::string payload =
        "shaderLabMaterialVersion=1\n"
        "shader=" + shaderSourcePath.generic_string() + "\n"
        "property _BaseMap Texture2D " + textureResourcePath + "\n";

    auto* loaded = NLS::Render::Resources::Loaders::MaterialLoader::CreateFromSerializedPayload(
        MaterialArtifactPath(root, "47b24ab4b128645b99328e0a68370de1202b0ba370eafc30e8bb0b0b7cf8b5ae").string(),
        payload,
        {true, true});

    ASSERT_NE(loaded, nullptr);
    const auto* albedoMap = loaded->GetParameterBlock().TryGet("_BaseMap");
    ASSERT_NE(albedoMap, nullptr);
    ASSERT_EQ(albedoMap->type(), typeid(NLS::Render::Resources::Texture2D*));
    EXPECT_EQ(std::any_cast<NLS::Render::Resources::Texture2D*>(*albedoMap), cachedTexture)
        << "MaterialLoader must reuse an equivalent cached texture artifact instead of decoding/uploading a duplicate.";
    EXPECT_FALSE(textureManager.IsResourceRegistered(textureResourcePath));

    EXPECT_TRUE(NLS::Render::Resources::Loaders::MaterialLoader::Destroy(loaded));
    shaderManager.UnloadResources();
    textureManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::TextureManager>();
    NLS::Core::ResourceManagement::TextureManager::ProvideAssetPaths({}, {});
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialLoaderReusesEquivalentCachedTextureArtifactIndexAcrossManyMaterials)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect texture artifact lookup index rebuilds.";
#else
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_material_equivalent_cached_texture_index_" + NLS::Guid::New().ToString());
    const auto projectAssets = root / "Assets";
    const auto shaderSourcePath = WriteShaderLabSourceBackedShaderArtifact(
        root,
        std::filesystem::path("Assets") / "Shaders" / "StandardPBR.shader",
        "2b40aa9d7e26302abaee46d90172b24a111dda5b6d466fcf2e7a2aff001a0607",
        MakeAlbedoMapShaderArtifact(),
        "StandardPBR Forward");
    const auto shaderArtifactPath = ArtifactPath(root, "2b40aa9d7e26302abaee46d90172b24a111dda5b6d466fcf2e7a2aff001a0607");
    const auto textureResourcePath = LibraryArtifactPath("f51fc4f93fdfaeb9d91abfc64a3296734c991105b8782f8a4aa617684c5d109b");

    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    NLS::Core::ResourceManagement::TextureManager textureManager;
    const ScopedShaderManagerAssetPaths shaderAssetPaths(
        projectAssets.string() + "/",
        "App/Assets/Engine/");
    NLS::Core::ResourceManagement::TextureManager::ProvideAssetPaths(
        projectAssets.string() + "/",
        "App/Assets/Engine/");
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::ShaderManager>(shaderManager);
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::TextureManager>(textureManager);

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(shaderArtifactPath.string());
    ASSERT_NE(shader, nullptr);
    shaderManager.RegisterResource(shaderArtifactPath.string(), shader);

    const auto absoluteTexturePath =
        NLS::Core::ResourceManagement::TextureManager::ResolveResourcePath(textureResourcePath);
    auto* cachedTexture = NLS::Render::Resources::Loaders::TextureLoader::CreatePixel(32u, 48u, 64u, 255u);
    ASSERT_NE(cachedTexture, nullptr);
    cachedTexture->path = absoluteTexturePath;
    textureManager.RegisterResource(absoluteTexturePath, cachedTexture);
    textureManager.ClearArtifactLookupIndexForTesting();

    constexpr size_t kMaterialCount = 32u;
    std::vector<NLS::Render::Resources::Material*> loadedMaterials;
    loadedMaterials.reserve(kMaterialCount);

    const std::string payload =
        "shaderLabMaterialVersion=1\n"
        "shader=" + shaderSourcePath.generic_string() + "\n"
        "property _BaseMap Texture2D " + textureResourcePath + "\n";

    for (size_t index = 0u; index < kMaterialCount; ++index)
    {
        auto* loaded = NLS::Render::Resources::Loaders::MaterialLoader::CreateFromSerializedPayload(
            MaterialArtifactPath(root, IndexedArtifactHash("cached-texture-material", index)).string(),
            payload,
            {true, true});
        ASSERT_NE(loaded, nullptr);
        loadedMaterials.push_back(loaded);

        const auto* albedoMap = loaded->GetParameterBlock().TryGet("_BaseMap");
        ASSERT_NE(albedoMap, nullptr);
        ASSERT_EQ(albedoMap->type(), typeid(NLS::Render::Resources::Texture2D*));
        EXPECT_EQ(std::any_cast<NLS::Render::Resources::Texture2D*>(*albedoMap), cachedTexture);
    }

    EXPECT_EQ(textureManager.GetArtifactLookupIndexRebuildCountForTesting(), 1u)
        << "Repeated equivalent artifact lookups should reuse TextureManager's normalized lookup index.";
    EXPECT_FALSE(textureManager.IsResourceRegistered(textureResourcePath));

    for (auto*& loaded : loadedMaterials)
        EXPECT_TRUE(NLS::Render::Resources::Loaders::MaterialLoader::Destroy(loaded));
    shaderManager.UnloadResources();
    textureManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::TextureManager>();
    NLS::Core::ResourceManagement::TextureManager::ProvideAssetPaths({}, {});
    std::filesystem::remove_all(root);
#endif
}

TEST(AssetMaterialConversionTests, TextureLoaderReadsImportedTextureArtifactPayload)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_imported_texture_artifact_load_" + NLS::Guid::New().ToString());
    const auto png = TinyPng();
    auto artifact = NLS::Render::Assets::DecodeTextureArtifactFromEncodedImage(
        png.data(),
        png.size(),
        NLS::Render::Assets::TextureArtifactColorSpace::Srgb,
        true);
    ASSERT_TRUE(artifact.has_value());
    const auto textureBytes = NLS::Render::Assets::SerializeTextureArtifact(*artifact);
    const auto texturePath = WriteTextureArtifactBytes(root, textureBytes);

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    auto* texture = NLS::Render::Resources::Loaders::TextureLoader::Create(
        texturePath.string(),
        NLS::Render::Settings::ETextureFilteringMode::NEAREST,
        NLS::Render::Settings::ETextureFilteringMode::NEAREST,
        false);

    ASSERT_NE(texture, nullptr);
    EXPECT_EQ(texture->width, 1u);
    EXPECT_EQ(texture->height, 1u);
    EXPECT_EQ(texture->path, texturePath.string());

    EXPECT_TRUE(NLS::Render::Resources::Loaders::TextureLoader::Destroy(texture));
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, TextureLoaderCreateRejectsSourceImagesButExplicitEditorApiLoadsThem)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_runtime_reject_source_texture_" + NLS::Guid::New().ToString());
    const auto texturePath = root / "Textures" / "HeroBaseColor.png";
    WriteBinaryFile(texturePath, TinyPng());

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    auto* runtimeTexture = NLS::Render::Resources::Loaders::TextureLoader::Create(
        texturePath.string(),
        NLS::Render::Settings::ETextureFilteringMode::NEAREST,
        NLS::Render::Settings::ETextureFilteringMode::NEAREST,
        false);
    EXPECT_EQ(runtimeTexture, nullptr);

    auto* editorTexture = NLS::Render::Resources::Loaders::TextureLoader::CreateFromImageFile(
        texturePath.string(),
        NLS::Render::Settings::ETextureFilteringMode::NEAREST,
        NLS::Render::Settings::ETextureFilteringMode::NEAREST,
        false);
    ASSERT_NE(editorTexture, nullptr);
    EXPECT_EQ(editorTexture->width, 1u);
    EXPECT_EQ(editorTexture->height, 1u);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::TextureLoader::Destroy(editorTexture));
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, TextureLoaderReadsImportedRgb16TextureArtifactPayload)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_imported_rgb16_texture_artifact_load_" + NLS::Guid::New().ToString());
    const auto png = TinyRgb16Png();
    auto artifact = NLS::Render::Assets::DecodeTextureArtifactFromEncodedImage(
        png.data(),
        png.size(),
        NLS::Render::Assets::TextureArtifactColorSpace::Linear,
        true);
    ASSERT_TRUE(artifact.has_value());
    const auto textureBytes = NLS::Render::Assets::SerializeTextureArtifact(*artifact);
    const auto texturePath = WriteTextureArtifactBytes(root, textureBytes);

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    auto* texture = NLS::Render::Resources::Loaders::TextureLoader::Create(
        texturePath.string(),
        NLS::Render::Settings::ETextureFilteringMode::NEAREST,
        NLS::Render::Settings::ETextureFilteringMode::NEAREST,
        false);

    ASSERT_NE(texture, nullptr);
    EXPECT_EQ(texture->width, 1u);
    EXPECT_EQ(texture->height, 1u);
    EXPECT_EQ(texture->path, texturePath.string());

    EXPECT_TRUE(NLS::Render::Resources::Loaders::TextureLoader::Destroy(texture));
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, TextureLoaderReadsNativeTextureArtifactWithoutEncodedPayload)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_native_texture_artifact_load_" + NLS::Guid::New().ToString());
    NLS::Render::Assets::TextureArtifactData artifact;
    artifact.width = 2u;
    artifact.height = 2u;
    artifact.format = NLS::Render::RHI::TextureFormat::RGBA8;
    artifact.colorSpace = NLS::Render::Assets::TextureArtifactColorSpace::Srgb;
    artifact.mips.push_back({
        0u,
        2u,
        2u,
        8u,
        16u,
        {
            255u, 0u, 0u, 255u,
            0u, 255u, 0u, 255u,
            0u, 0u, 255u, 255u,
            255u, 255u, 255u, 255u
        }
    });
    artifact.mips.push_back({1u, 1u, 1u, 4u, 4u, {128u, 128u, 128u, 255u}});
    const auto bytes = NLS::Render::Assets::SerializeTextureArtifact(artifact);
    const auto texturePath = WriteTextureArtifactBytes(root, bytes);

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    auto* texture = NLS::Render::Resources::Loaders::TextureLoader::Create(
        texturePath.string(),
        NLS::Render::Settings::ETextureFilteringMode::NEAREST,
        NLS::Render::Settings::ETextureFilteringMode::NEAREST,
        false);

    ASSERT_NE(texture, nullptr);
    EXPECT_EQ(texture->width, 2u);
    EXPECT_EQ(texture->height, 2u);
    EXPECT_TRUE(texture->isMimapped);
    EXPECT_EQ(texture->path, texturePath.string());

    EXPECT_TRUE(NLS::Render::Resources::Loaders::TextureLoader::Destroy(texture));
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, TextureLoaderReadsNativeTextureArtifactWithoutDriverService)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_native_texture_artifact_headless_load_" + NLS::Guid::New().ToString());
    const auto artifact = MakeDescriptorBackedTextureArtifact(
        NLS::Render::RHI::TextureFormat::RGBA8,
        NLS::Render::Assets::TextureArtifactColorSpace::Srgb,
        2u,
        2u,
        2u);
    const auto textureBytes = NLS::Render::Assets::SerializeTextureArtifact(artifact);
    const auto texturePath = WriteTextureArtifactBytes(root, textureBytes);

    const ScopedNoDriverService noDriverService;

    auto* texture = NLS::Render::Resources::Loaders::TextureLoader::Create(
        texturePath.string(),
        NLS::Render::Settings::ETextureFilteringMode::NEAREST,
        NLS::Render::Settings::ETextureFilteringMode::NEAREST,
        false);

    ASSERT_NE(texture, nullptr);
    EXPECT_EQ(texture->width, 2u);
    EXPECT_EQ(texture->height, 2u);
    EXPECT_TRUE(texture->isMimapped);
    EXPECT_EQ(texture->path, texturePath.string());
    EXPECT_EQ(texture->GetTextureHandle(), nullptr);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::TextureLoader::Destroy(texture));
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, TextureLoaderPropagatesCompressedArtifactMetadataToRhi)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_compressed_texture_artifact_load_" + NLS::Guid::New().ToString());
    const auto artifact = MakeDescriptorBackedTextureArtifact(
        NLS::Render::RHI::TextureFormat::BC7,
        NLS::Render::Assets::TextureArtifactColorSpace::Srgb,
        8u,
        8u,
        2u);
    const auto textureBytes = NLS::Render::Assets::SerializeTextureArtifact(artifact);
    const auto texturePath = WriteTextureArtifactBytes(root, textureBytes);

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    auto explicitDevice = std::make_shared<TextureLoaderTestDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    auto* texture = NLS::Render::Resources::Loaders::TextureLoader::Create(
        texturePath.string(),
        NLS::Render::Settings::ETextureFilteringMode::LINEAR,
        NLS::Render::Settings::ETextureFilteringMode::LINEAR,
        false);

    ASSERT_NE(texture, nullptr);
    EXPECT_EQ(texture->width, 8u);
    EXPECT_EQ(texture->height, 8u);
    EXPECT_TRUE(texture->isMimapped);
    EXPECT_EQ(texture->path, texturePath.string());

    EXPECT_EQ(explicitDevice->lastTextureDesc.format, NLS::Render::RHI::TextureFormat::BC7);
    EXPECT_EQ(explicitDevice->lastTextureDesc.colorSpace, NLS::Render::RHI::TextureColorSpace::SRGB);
    EXPECT_EQ(explicitDevice->lastTextureDesc.mipLevels, 2u);
    EXPECT_EQ(explicitDevice->lastTextureUploadDesc.dataSize, 80u);
    EXPECT_EQ(explicitDevice->lastTextureUploadDesc.rowPitch, 32u);
    EXPECT_EQ(explicitDevice->lastTextureUploadDesc.slicePitch, 64u);

    const auto view = texture->GetOrCreateExplicitTextureView("CompressedTextureArtifactView");
    ASSERT_NE(view, nullptr);
    EXPECT_EQ(view->GetDesc().format, NLS::Render::RHI::TextureFormat::BC7);
    EXPECT_EQ(view->GetDesc().colorSpace, NLS::Render::RHI::TextureColorSpace::SRGB);
    EXPECT_EQ(view->GetDesc().subresourceRange.mipLevelCount, 2u);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::TextureLoader::Destroy(texture));
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, TextureLoaderWarnsWhenCompressedArtifactBackendIsUnsupported)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_unsupported_compressed_texture_artifact_" + NLS::Guid::New().ToString());
    const auto artifact = MakeDescriptorBackedTextureArtifact(
        NLS::Render::RHI::TextureFormat::BC1,
        NLS::Render::Assets::TextureArtifactColorSpace::Srgb,
        4u,
        4u,
        1u);
    const auto textureBytes = NLS::Render::Assets::SerializeTextureArtifact(artifact);
    const auto texturePath = WriteTextureArtifactBytes(root, textureBytes);

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    bool sawUnsupportedCompressedWarning = false;
    const auto listener = NLS::Debug::Logger::LogEvent +=
        [&sawUnsupportedCompressedWarning](const NLS::Debug::LogData& log)
        {
            if (log.logLevel == NLS::Debug::ELogLevel::LOG_WARNING &&
                log.message.find("unsupported compressed texture artifact") != std::string::npos)
            {
                sawUnsupportedCompressedWarning = true;
            }
        };

    auto* texture = NLS::Render::Resources::Loaders::TextureLoader::Create(
        texturePath.string(),
        NLS::Render::Settings::ETextureFilteringMode::NEAREST,
        NLS::Render::Settings::ETextureFilteringMode::NEAREST,
        false);

    NLS::Debug::Logger::LogEvent -= listener;

    EXPECT_EQ(texture, nullptr);
    EXPECT_TRUE(sawUnsupportedCompressedWarning);
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, TextureArtifactSerializesNativeRgba8MipChain)
{
    NLS::Render::Assets::TextureArtifactData artifact;
    artifact.width = 2u;
    artifact.height = 2u;
    artifact.format = NLS::Render::RHI::TextureFormat::RGBA8;
    artifact.colorSpace = NLS::Render::Assets::TextureArtifactColorSpace::Srgb;
    artifact.mips.push_back({
        0u,
        2u,
        2u,
        8u,
        16u,
        {
            255u, 0u, 0u, 255u,
            0u, 255u, 0u, 255u,
            0u, 0u, 255u, 255u,
            255u, 255u, 255u, 255u
        }
    });
    artifact.mips.push_back({
        1u,
        1u,
        1u,
        4u,
        4u,
        {128u, 128u, 128u, 255u}
    });

    const auto bytes = NLS::Render::Assets::SerializeTextureArtifact(artifact);
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], static_cast<uint8_t>('N'));
    EXPECT_EQ(bytes[1], static_cast<uint8_t>('L'));
    EXPECT_EQ(bytes[2], static_cast<uint8_t>('S'));
    EXPECT_EQ(bytes[3], static_cast<uint8_t>('A'));

    const auto decoded = NLS::Render::Assets::DeserializeTextureArtifact(bytes);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->width, 2u);
    EXPECT_EQ(decoded->height, 2u);
    EXPECT_EQ(decoded->format, NLS::Render::RHI::TextureFormat::RGBA8);
    EXPECT_EQ(decoded->colorSpace, NLS::Render::Assets::TextureArtifactColorSpace::Srgb);
    ASSERT_EQ(decoded->mips.size(), 2u);
    EXPECT_EQ(decoded->mips[0].width, 2u);
    EXPECT_EQ(decoded->mips[0].height, 2u);
    EXPECT_EQ(decoded->mips[0].rowPitch, 8u);
    EXPECT_EQ(decoded->mips[0].slicePitch, 16u);
    EXPECT_EQ(decoded->mips[0].pixels.size(), 16u);
    EXPECT_EQ(decoded->mips[1].width, 1u);
    EXPECT_EQ(decoded->mips[1].height, 1u);
    EXPECT_EQ(decoded->mips[1].pixels, (std::vector<uint8_t>{128u, 128u, 128u, 255u}));
}

TEST(AssetMaterialConversionTests, TextureArtifactWritesCurrentSchema4Container)
{
    NLS::Render::Assets::TextureArtifactData artifact;
    artifact.width = 2u;
    artifact.height = 2u;
    artifact.format = NLS::Render::RHI::TextureFormat::RGBA8;
    artifact.colorSpace = NLS::Render::Assets::TextureArtifactColorSpace::Srgb;
    artifact.mips.push_back({
        0u,
        2u,
        2u,
        8u,
        16u,
        {
            255u, 0u, 0u, 255u,
            0u, 255u, 0u, 255u,
            0u, 0u, 255u, 255u,
            255u, 255u, 255u, 255u
        }
    });

    const auto bytes = NLS::Render::Assets::SerializeTextureArtifact(artifact);
    const auto container = NLS::Core::Assets::ReadNativeArtifactContainer(
        bytes,
        NLS::Core::Assets::ArtifactType::Texture,
        4u);
    ASSERT_TRUE(container.has_value());

    const auto decoded = NLS::Render::Assets::DeserializeTextureArtifact(bytes);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->width, 2u);
    EXPECT_EQ(decoded->height, 2u);
}

TEST(AssetMaterialConversionTests, TextureArtifactSerializesBcAndRgba16fDescriptorBackedPayloads)
{
    struct FormatCase
    {
        NLS::Render::RHI::TextureFormat format;
        NLS::Render::Assets::TextureArtifactColorSpace colorSpace;
    };

    const std::array<FormatCase, 5u> cases{{
        {NLS::Render::RHI::TextureFormat::BC1, NLS::Render::Assets::TextureArtifactColorSpace::Srgb},
        {NLS::Render::RHI::TextureFormat::BC3, NLS::Render::Assets::TextureArtifactColorSpace::Srgb},
        {NLS::Render::RHI::TextureFormat::BC5, NLS::Render::Assets::TextureArtifactColorSpace::Linear},
        {NLS::Render::RHI::TextureFormat::BC7, NLS::Render::Assets::TextureArtifactColorSpace::Srgb},
        {NLS::Render::RHI::TextureFormat::RGBA16F, NLS::Render::Assets::TextureArtifactColorSpace::Linear}
    }};

    for (const auto& formatCase : cases)
    {
        SCOPED_TRACE(static_cast<int>(formatCase.format));
        const auto artifact = MakeDescriptorBackedTextureArtifact(
            formatCase.format,
            formatCase.colorSpace,
            8u,
            8u,
            2u);

        const auto bytes = NLS::Render::Assets::SerializeTextureArtifact(artifact);
        ASSERT_FALSE(bytes.empty());

        const auto decoded = NLS::Render::Assets::DeserializeTextureArtifact(bytes);
        ASSERT_TRUE(decoded.has_value());
        EXPECT_EQ(decoded->width, 8u);
        EXPECT_EQ(decoded->height, 8u);
        EXPECT_EQ(decoded->format, formatCase.format);
        EXPECT_EQ(decoded->colorSpace, formatCase.colorSpace);
        ASSERT_EQ(decoded->mips.size(), artifact.mips.size());
        ASSERT_EQ(decoded->subresources.size(), artifact.mips.size());

        for (size_t mipIndex = 0u; mipIndex < artifact.mips.size(); ++mipIndex)
        {
            const auto& expectedMip = artifact.mips[mipIndex];
            const auto& actualMip = decoded->mips[mipIndex];
            const auto& actualSubresource = decoded->subresources[mipIndex];

            EXPECT_EQ(actualMip.level, expectedMip.level);
            EXPECT_EQ(actualMip.width, expectedMip.width);
            EXPECT_EQ(actualMip.height, expectedMip.height);
            EXPECT_EQ(actualMip.rowPitch, NLS::Render::RHI::CalculateTextureRowPitch(formatCase.format, expectedMip.width));
            EXPECT_EQ(actualMip.slicePitch, NLS::Render::RHI::CalculateTextureSlicePitch(formatCase.format, expectedMip.width, expectedMip.height, 1u));
            EXPECT_EQ(actualMip.pixels, expectedMip.pixels);

            EXPECT_EQ(actualSubresource.level, expectedMip.level);
            EXPECT_EQ(actualSubresource.arrayLayer, 0u);
            EXPECT_EQ(actualSubresource.width, expectedMip.width);
            EXPECT_EQ(actualSubresource.height, expectedMip.height);
            EXPECT_EQ(actualSubresource.depth, 1u);
            EXPECT_EQ(actualSubresource.rowPitch, actualMip.rowPitch);
            EXPECT_EQ(actualSubresource.slicePitch, actualMip.slicePitch);
            EXPECT_EQ(actualSubresource.dataSize, expectedMip.pixels.size());
        }
    }
}

TEST(AssetMaterialConversionTests, TextureArtifactPreservesBuildMetadataAndCubeFaceOrdering)
{
    NLS::Render::Assets::TextureArtifactData artifact;
    artifact.width = 4u;
    artifact.height = 4u;
    artifact.dimension = NLS::Render::RHI::TextureDimension::TextureCube;
    artifact.arrayLayers = 6u;
    artifact.format = NLS::Render::RHI::TextureFormat::RGBA8;
    artifact.colorSpace = NLS::Render::Assets::TextureArtifactColorSpace::Srgb;
    artifact.targetPlatform = "win64-dx12";
    artifact.buildIdentity = "texture-build-identity";
    artifact.encoderId = "rgba8-passthrough";
    artifact.encoderVersion = 7u;

    const std::array<NLS::Render::Assets::TextureArtifactCubeFace, 6u> faces{{
        NLS::Render::Assets::TextureArtifactCubeFace::PositiveX,
        NLS::Render::Assets::TextureArtifactCubeFace::NegativeX,
        NLS::Render::Assets::TextureArtifactCubeFace::PositiveY,
        NLS::Render::Assets::TextureArtifactCubeFace::NegativeY,
        NLS::Render::Assets::TextureArtifactCubeFace::PositiveZ,
        NLS::Render::Assets::TextureArtifactCubeFace::NegativeZ
    }};

    for (uint32_t faceIndex = 0u; faceIndex < faces.size(); ++faceIndex)
    {
        NLS::Render::Assets::TextureArtifactMip mip;
        mip.level = 0u;
        mip.width = artifact.width;
        mip.height = artifact.height;
        mip.rowPitch = NLS::Render::RHI::CalculateTextureRowPitch(artifact.format, mip.width);
        mip.slicePitch = NLS::Render::RHI::CalculateTextureSlicePitch(artifact.format, mip.width, mip.height, 1u);
        mip.pixels.assign(mip.slicePitch, static_cast<uint8_t>(faceIndex + 1u));
        artifact.mips.push_back(std::move(mip));

        artifact.subresources.push_back({
            0u,
            faceIndex,
            artifact.width,
            artifact.height,
            1u,
            faces[faceIndex],
            NLS::Render::RHI::CalculateTextureRowPitch(artifact.format, artifact.width),
            NLS::Render::RHI::CalculateTextureSlicePitch(artifact.format, artifact.width, artifact.height, 1u),
            0u,
            0u
        });
    }

    const auto bytes = NLS::Render::Assets::SerializeTextureArtifact(artifact);
    ASSERT_FALSE(bytes.empty());

    const auto decoded = NLS::Render::Assets::DeserializeTextureArtifact(bytes);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->dimension, NLS::Render::RHI::TextureDimension::TextureCube);
    EXPECT_EQ(decoded->arrayLayers, 6u);
    EXPECT_EQ(decoded->targetPlatform, artifact.targetPlatform);
    EXPECT_EQ(decoded->buildIdentity, artifact.buildIdentity);
    EXPECT_EQ(decoded->encoderId, artifact.encoderId);
    EXPECT_EQ(decoded->encoderVersion, artifact.encoderVersion);
    EXPECT_EQ(decoded->mips.size(), faces.size());
    ASSERT_EQ(decoded->subresources.size(), faces.size());
    for (size_t faceIndex = 0u; faceIndex < faces.size(); ++faceIndex)
    {
        EXPECT_EQ(decoded->subresources[faceIndex].arrayLayer, faceIndex);
        EXPECT_EQ(decoded->subresources[faceIndex].face, faces[faceIndex]);
        ASSERT_EQ(decoded->mips[faceIndex].pixels.size(), decoded->mips[faceIndex].slicePitch);
        EXPECT_EQ(decoded->mips[faceIndex].pixels.front(), static_cast<uint8_t>(faceIndex + 1u));
    }
}

TEST(AssetMaterialConversionTests, TextureArtifactRejectsMalformedCubeFaceOrdering)
{
    NLS::Render::Assets::TextureArtifactData artifact;
    artifact.width = 4u;
    artifact.height = 4u;
    artifact.dimension = NLS::Render::RHI::TextureDimension::TextureCube;
    artifact.arrayLayers = 6u;
    artifact.format = NLS::Render::RHI::TextureFormat::RGBA8;
    artifact.colorSpace = NLS::Render::Assets::TextureArtifactColorSpace::Srgb;

    const std::array<NLS::Render::Assets::TextureArtifactCubeFace, 6u> faces{{
        NLS::Render::Assets::TextureArtifactCubeFace::PositiveX,
        NLS::Render::Assets::TextureArtifactCubeFace::PositiveX,
        NLS::Render::Assets::TextureArtifactCubeFace::PositiveY,
        NLS::Render::Assets::TextureArtifactCubeFace::NegativeY,
        NLS::Render::Assets::TextureArtifactCubeFace::PositiveZ,
        NLS::Render::Assets::TextureArtifactCubeFace::NegativeZ
    }};

    for (uint32_t faceIndex = 0u; faceIndex < faces.size(); ++faceIndex)
    {
        NLS::Render::Assets::TextureArtifactMip mip;
        mip.level = 0u;
        mip.width = artifact.width;
        mip.height = artifact.height;
        mip.rowPitch = NLS::Render::RHI::CalculateTextureRowPitch(artifact.format, mip.width);
        mip.slicePitch = NLS::Render::RHI::CalculateTextureSlicePitch(artifact.format, mip.width, mip.height, 1u);
        mip.pixels.assign(mip.slicePitch, static_cast<uint8_t>(faceIndex + 1u));
        artifact.mips.push_back(std::move(mip));

        artifact.subresources.push_back({
            0u,
            faceIndex,
            artifact.width,
            artifact.height,
            1u,
            faces[faceIndex],
            NLS::Render::RHI::CalculateTextureRowPitch(artifact.format, artifact.width),
            NLS::Render::RHI::CalculateTextureSlicePitch(artifact.format, artifact.width, artifact.height, 1u),
            0u,
            0u
        });
    }

    EXPECT_TRUE(NLS::Render::Assets::SerializeTextureArtifact(artifact).empty());
}

TEST(AssetMaterialConversionTests, TextureArtifactRejectsNonContiguousMipLevels)
{
    NLS::Render::Assets::TextureArtifactData artifact;
    artifact.width = 4u;
    artifact.height = 4u;
    artifact.format = NLS::Render::RHI::TextureFormat::RGBA8;

    for (const uint32_t level : {0u, 2u})
    {
        NLS::Render::Assets::TextureArtifactMip mip;
        mip.level = level;
        mip.width = level == 0u ? 4u : 1u;
        mip.height = level == 0u ? 4u : 1u;
        mip.rowPitch = NLS::Render::RHI::CalculateTextureRowPitch(artifact.format, mip.width);
        mip.slicePitch = NLS::Render::RHI::CalculateTextureSlicePitch(artifact.format, mip.width, mip.height, 1u);
        mip.pixels.assign(mip.slicePitch, 255u);
        artifact.mips.push_back(std::move(mip));
    }

    EXPECT_TRUE(NLS::Render::Assets::SerializeTextureArtifact(artifact).empty());
}

TEST(AssetMaterialConversionTests, TextureArtifactDecodesRgb16PngAsRgba8)
{
    const auto png = TinyRgb16Png();
    const auto decoded = NLS::Render::Assets::DecodeTextureArtifactFromEncodedImage(
        png.data(),
        png.size(),
        NLS::Render::Assets::TextureArtifactColorSpace::Srgb,
        false);

    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->width, 1u);
    EXPECT_EQ(decoded->height, 1u);
    EXPECT_EQ(decoded->format, NLS::Render::RHI::TextureFormat::RGBA8);
    ASSERT_EQ(decoded->mips.size(), 1u);
    EXPECT_EQ(decoded->mips[0].pixels, (std::vector<uint8_t>{0x12u, 0x56u, 0x9Au, 0xFFu}));
}

TEST(AssetMaterialConversionTests, TextureArtifactDeserializesLegacySchema3Payload2)
{
    NLS::Core::Assets::NativeArtifactMetadata metadata;
    metadata.artifactType = NLS::Core::Assets::ArtifactType::Texture;
    metadata.schemaName = "texture";
    metadata.schemaVersion = 3u;

    const auto bytes = NLS::Core::Assets::WriteNativeArtifactContainer(
        std::move(metadata),
        MakeLegacyTextureArtifactPayloadV2());

    const auto decoded = NLS::Render::Assets::DeserializeTextureArtifact(bytes);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->width, 2u);
    EXPECT_EQ(decoded->height, 2u);
    EXPECT_EQ(decoded->format, NLS::Render::RHI::TextureFormat::RGBA8);
    EXPECT_EQ(decoded->colorSpace, NLS::Render::Assets::TextureArtifactColorSpace::Srgb);
    ASSERT_EQ(decoded->mips.size(), 2u);
    EXPECT_EQ(decoded->mips[0].slicePitch, 16u);
    EXPECT_EQ(decoded->mips[1].pixels, (std::vector<uint8_t>{128u, 128u, 128u, 255u}));
}

TEST(AssetMaterialConversionTests, TextureMipGeneratorBuildsFullChainForColorTextures)
{
    std::vector<uint8_t> pixels(static_cast<size_t>(1024u) * 1024u * 4u, 0u);
    for (uint32_t y = 0u; y < 1024u; ++y)
    {
        for (uint32_t x = 0u; x < 1024u; ++x)
        {
            const size_t index = (static_cast<size_t>(y) * 1024u + x) * 4u;
            pixels[index + 0u] = static_cast<uint8_t>(x & 0xFFu);
            pixels[index + 1u] = static_cast<uint8_t>(y & 0xFFu);
            pixels[index + 2u] = static_cast<uint8_t>((x + y) & 0xFFu);
            pixels[index + 3u] = 255u;
        }
    }

    NLS::Render::Assets::TextureMipGeneratorSettings settings;
    settings.intent = NLS::Render::Assets::TextureMipIntent::Color;
    settings.colorSpace = NLS::Render::Assets::TextureArtifactColorSpace::Srgb;
    settings.mipmapEnabled = true;

    const auto artifact = NLS::Render::Assets::GenerateTextureMipChain(
        1024u,
        1024u,
        std::move(pixels),
        settings);

    ASSERT_TRUE(artifact.has_value());
    EXPECT_EQ(artifact->width, 1024u);
    EXPECT_EQ(artifact->height, 1024u);
    EXPECT_EQ(artifact->format, NLS::Render::RHI::TextureFormat::RGBA8);
    ASSERT_EQ(artifact->mips.size(), 11u);
    EXPECT_EQ(artifact->mips.front().width, 1024u);
    EXPECT_EQ(artifact->mips.back().width, 1u);
    EXPECT_EQ(artifact->mips.back().height, 1u);
}

TEST(AssetMaterialConversionTests, TextureMipGeneratorKeepsUiTexturesSingleMip)
{
    std::vector<uint8_t> pixels(4u * 4u * 4u, 255u);

    NLS::Render::Assets::TextureMipGeneratorSettings settings;
    settings.intent = NLS::Render::Assets::TextureMipIntent::UI;
    settings.colorSpace = NLS::Render::Assets::TextureArtifactColorSpace::Srgb;
    settings.mipmapEnabled = false;

    const auto artifact = NLS::Render::Assets::GenerateTextureMipChain(
        4u,
        4u,
        std::move(pixels),
        settings);

    ASSERT_TRUE(artifact.has_value());
    ASSERT_EQ(artifact->mips.size(), 1u);
    EXPECT_EQ(artifact->mips[0].width, 4u);
    EXPECT_EQ(artifact->mips[0].height, 4u);
}

TEST(AssetMaterialConversionTests, TextureMipGeneratorKeepsTinyTexturesSingleMip)
{
    std::vector<uint8_t> pixels{17u, 34u, 51u, 255u};

    NLS::Render::Assets::TextureMipGeneratorSettings settings;
    settings.intent = NLS::Render::Assets::TextureMipIntent::Color;
    settings.colorSpace = NLS::Render::Assets::TextureArtifactColorSpace::Srgb;
    settings.mipmapEnabled = true;

    const auto artifact = NLS::Render::Assets::GenerateTextureMipChain(
        1u,
        1u,
        std::move(pixels),
        settings);

    ASSERT_TRUE(artifact.has_value());
    EXPECT_EQ(artifact->format, NLS::Render::RHI::TextureFormat::RGBA8);
    ASSERT_EQ(artifact->mips.size(), 1u);
    EXPECT_EQ(artifact->mips[0].rowPitch, 4u);
    EXPECT_EQ(artifact->mips[0].slicePitch, 4u);
    EXPECT_EQ(artifact->mips[0].pixels, (std::vector<uint8_t>{17u, 34u, 51u, 255u}));
}

TEST(AssetMaterialConversionTests, TextureMipGeneratorFiltersSrgbColorMipsInLinearSpace)
{
    std::vector<uint8_t> pixels{
        0u, 0u, 0u, 255u,
        255u, 255u, 255u, 255u,
        255u, 255u, 255u, 255u,
        255u, 255u, 255u, 255u
    };

    NLS::Render::Assets::TextureMipGeneratorSettings settings;
    settings.intent = NLS::Render::Assets::TextureMipIntent::Color;
    settings.colorSpace = NLS::Render::Assets::TextureArtifactColorSpace::Srgb;
    settings.mipmapEnabled = true;

    const auto artifact = NLS::Render::Assets::GenerateTextureMipChain(
        2u,
        2u,
        std::move(pixels),
        settings);

    ASSERT_TRUE(artifact.has_value());
    ASSERT_EQ(artifact->mips.size(), 2u);
    ASSERT_EQ(artifact->mips[1].pixels.size(), 4u);
    EXPECT_GT(artifact->mips[1].pixels[0], 220u);
    EXPECT_LT(artifact->mips[1].pixels[0], 230u);
    EXPECT_EQ(artifact->mips[1].pixels[0], artifact->mips[1].pixels[1]);
    EXPECT_EQ(artifact->mips[1].pixels[1], artifact->mips[1].pixels[2]);
    EXPECT_EQ(artifact->mips[1].pixels[3], 255u);
}

TEST(AssetMaterialConversionTests, TextureMipGeneratorRenormalizesNormalMapMips)
{
    std::vector<uint8_t> pixels;
    const auto n0 = EncodeNormalVector(0.0f, 0.0f, 1.0f);
    const auto n1 = EncodeNormalVector(0.6f, 0.0f, 0.8f);
    const auto n2 = EncodeNormalVector(0.0f, 0.6f, 0.8f);
    const auto n3 = EncodeNormalVector(0.6f, 0.6f, 0.529150f);
    pixels.insert(pixels.end(), n0.begin(), n0.end());
    pixels.insert(pixels.end(), n1.begin(), n1.end());
    pixels.insert(pixels.end(), n2.begin(), n2.end());
    pixels.insert(pixels.end(), n3.begin(), n3.end());

    NLS::Render::Assets::TextureMipGeneratorSettings settings;
    settings.intent = NLS::Render::Assets::TextureMipIntent::Normal;
    settings.colorSpace = NLS::Render::Assets::TextureArtifactColorSpace::Linear;
    settings.mipmapEnabled = true;

    const auto artifact = NLS::Render::Assets::GenerateTextureMipChain(
        2u,
        2u,
        std::move(pixels),
        settings);

    ASSERT_TRUE(artifact.has_value());
    ASSERT_EQ(artifact->mips.size(), 2u);
    const auto decodedNormal = DecodeNormalVector(artifact->mips[1].pixels);
    const float length = std::sqrt(
        decodedNormal[0] * decodedNormal[0] +
        decodedNormal[1] * decodedNormal[1] +
        decodedNormal[2] * decodedNormal[2]);
    EXPECT_NEAR(length, 1.0f, 0.05f);
    EXPECT_GT(decodedNormal[2], 0.5f);
}

TEST(AssetMaterialConversionTests, TextureMipGeneratorPreservesRgba16fHdrMips)
{
    std::vector<uint8_t> pixels;
    const std::array<std::array<float, 4>, 4> texels = {{
        {{2.0f, 0.5f, 0.25f, 1.0f}},
        {{4.0f, 1.5f, 0.5f, 1.0f}},
        {{6.0f, 2.5f, 0.75f, 1.0f}},
        {{8.0f, 3.5f, 1.0f, 1.0f}}
    }};
    for (const auto& texel : texels)
    {
        for (const float component : texel)
            AppendHalfFloatForTest(pixels, component);
    }
    const auto basePixels = pixels;

    NLS::Render::Assets::TextureMipGeneratorSettings settings;
    settings.intent = NLS::Render::Assets::TextureMipIntent::HDR;
    settings.colorSpace = NLS::Render::Assets::TextureArtifactColorSpace::Linear;
    settings.format = NLS::Render::RHI::TextureFormat::RGBA16F;
    settings.mipmapEnabled = true;

    const auto artifact = NLS::Render::Assets::GenerateTextureMipChain(
        2u,
        2u,
        std::move(pixels),
        settings);

    ASSERT_TRUE(artifact.has_value());
    EXPECT_EQ(artifact->format, NLS::Render::RHI::TextureFormat::RGBA16F);
    ASSERT_EQ(artifact->mips.size(), 2u);
    EXPECT_EQ(artifact->mips[0].rowPitch, 16u);
    EXPECT_EQ(artifact->mips[0].slicePitch, 32u);
    EXPECT_EQ(artifact->mips[0].pixels, basePixels);
    EXPECT_EQ(artifact->mips[1].rowPitch, 8u);
    EXPECT_EQ(artifact->mips[1].slicePitch, 8u);
    EXPECT_NEAR(ReadHalfFloatForTest(artifact->mips[1].pixels, 0u), 5.0f, 0.01f);
    EXPECT_NEAR(ReadHalfFloatForTest(artifact->mips[1].pixels, 1u), 2.0f, 0.01f);
    EXPECT_GT(ReadHalfFloatForTest(artifact->mips[1].pixels, 0u), 1.0f);
}

TEST(AssetMaterialConversionTests, ImageFileLoaderDecodesRgb16Png)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_rgb16_png_image_load_" + NLS::Guid::New().ToString());
    const auto imagePath = root / "Render_Main_A.png";
    WriteBinaryFile(imagePath, TinyRgb16Png());

    const NLS::Image image(imagePath.string(), false);
    const bool loaded = image.GetData() != nullptr;
    EXPECT_TRUE(loaded);
    if (loaded)
    {
        EXPECT_EQ(image.GetWidth(), 1);
        EXPECT_EQ(image.GetHeight(), 1);
        EXPECT_EQ(image.GetChannels(), 3);
        EXPECT_EQ(image.GetData()[0], 0x12u);
        EXPECT_EQ(image.GetData()[1], 0x56u);
        EXPECT_EQ(image.GetData()[2], 0x9Au);
    }

    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, ImageMemoryLoaderDecodesRgb16Png)
{
    const auto png = TinyRgb16Png();

    const NLS::Image image(png.data(), png.size(), false);
    const bool loaded = image.GetData() != nullptr;
    EXPECT_TRUE(loaded);
    if (loaded)
    {
        EXPECT_EQ(image.GetWidth(), 1);
        EXPECT_EQ(image.GetHeight(), 1);
        EXPECT_EQ(image.GetChannels(), 3);
        EXPECT_EQ(image.GetData()[0], 0x12u);
        EXPECT_EQ(image.GetData()[1], 0x56u);
        EXPECT_EQ(image.GetData()[2], 0x9Au);
    }
}

TEST(AssetMaterialConversionTests, ImageSetDataKeepsOwnedPixelMemoryAliveUntilDestruction)
{
    std::array<uint8_t, 4u> pixel { 0x11u, 0x22u, 0x33u, 0x44u };

    {
        NLS::Image image(1, 1, 4);
        image.SetData(pixel.data());

        ASSERT_NE(image.GetData(), nullptr);
        EXPECT_EQ(image.GetData()[0], 0x11u);
        EXPECT_EQ(image.GetData()[1], 0x22u);
        EXPECT_EQ(image.GetData()[2], 0x33u);
        EXPECT_EQ(image.GetData()[3], 0x44u);
    }
}

TEST(AssetMaterialConversionTests, ImageCopyAndMovePreserveDecodedPixels)
{
    std::array<uint8_t, 4u> pixel { 0x51u, 0x52u, 0x53u, 0x54u };

    NLS::Image source(1, 1, 4);
    source.SetData(pixel.data());

    NLS::Image copied(source);
    ASSERT_NE(copied.GetData(), nullptr);
    EXPECT_EQ(copied.GetData()[0], 0x51u);
    EXPECT_EQ(copied.GetData()[1], 0x52u);
    EXPECT_EQ(copied.GetData()[2], 0x53u);
    EXPECT_EQ(copied.GetData()[3], 0x54u);

    NLS::Image moved(std::move(source));
    ASSERT_NE(moved.GetData(), nullptr);
    EXPECT_EQ(moved.GetData()[0], 0x51u);
    EXPECT_EQ(moved.GetData()[1], 0x52u);
    EXPECT_EQ(moved.GetData()[2], 0x53u);
    EXPECT_EQ(moved.GetData()[3], 0x54u);
}

TEST(AssetMaterialConversionTests, FbxAndObjChannelsMapOrDiagnoseParserExposedData)
{
    NLS::Render::Assets::ImportedScene scene;
    scene.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("e2010101-0101-4101-8101-010101010101"));
    scene.textures.push_back({"fbx/texture/diffuse", "Diffuse", "Diffuse.png", "image/png"});
    scene.textures.push_back({"fbx/texture/normal", "Normal", "Normal.png", "image/png"});
    scene.textures.push_back({"fbx/texture/metallic", "Metallic", "Metallic.png", "image/png"});
    scene.textures.push_back({"fbx/texture/roughness", "Roughness", "Roughness.png", "image/png"});
    scene.textures.push_back({"fbx/texture/opacity", "Opacity", "Opacity.png", "image/png"});
    scene.textures.push_back({"fbx/texture/emissive", "Emissive", "Emissive.png", "image/png"});
    scene.textures.push_back({"fbx/texture/specular", "Specular", "Specular.png", "image/png"});

    NLS::Render::Assets::ImportedSceneNamedRecord fbx;
    fbx.sourceKey = "fbx/material/HeroSurface";
    fbx.name = "HeroSurface";
    fbx.materialChannels.push_back({"diffuse", "fbx/texture/diffuse", {1.0, 0.8, 0.6}, false, 0.0});
    fbx.materialChannels.push_back({"normal", "fbx/texture/normal", {}, false, 0.0});
    fbx.materialChannels.push_back({"roughness", "fbx/texture/roughness", {}, true, 0.35});
    fbx.materialChannels.push_back({"metallic", "fbx/texture/metallic", {}, true, 0.45});
    fbx.materialChannels.push_back({"occlusion", {}, {}, true, 0.9});
    fbx.materialChannels.push_back({"opacity", "fbx/texture/opacity", {}, true, 0.6});
    fbx.materialChannels.push_back({"emissive", "fbx/texture/emissive", {0.1, 0.2, 0.3}, false, 0.0});
    fbx.materialChannels.push_back({"specular", "fbx/texture/specular", {0.7, 0.8, 0.9}, false, 0.0});
    fbx.materialChannels.push_back({"doubleSided", {}, {}, true, 1.0});

    const auto convertedFbx = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        fbx,
        MaterialSourceModel::FbxParserMaterial);
    EXPECT_EQ(convertedFbx.workflow, "parser-fbx");
    EXPECT_EQ(convertedFbx.alphaMode, MaterialAlphaMode::Blend);
    EXPECT_TRUE(convertedFbx.doubleSided);
    EXPECT_NE(FindSlot(convertedFbx, "BaseColor"), nullptr);
    EXPECT_NE(FindSlot(convertedFbx, "Normal"), nullptr);
    EXPECT_NE(FindSlot(convertedFbx, "Metallic"), nullptr);
    EXPECT_NE(FindSlot(convertedFbx, "Roughness"), nullptr);
    EXPECT_NE(FindSlot(convertedFbx, "Opacity"), nullptr);
    EXPECT_NE(FindSlot(convertedFbx, "Emissive"), nullptr);
    EXPECT_NE(FindSlot(convertedFbx, "Specular"), nullptr);
    EXPECT_DOUBLE_EQ(FindFactor(convertedFbx, "Roughness")->scalar, 0.35);
    EXPECT_DOUBLE_EQ(FindFactor(convertedFbx, "Metallic")->scalar, 0.45);
    EXPECT_DOUBLE_EQ(FindFactor(convertedFbx, "OcclusionStrength")->scalar, 0.9);
    EXPECT_DOUBLE_EQ(FindFactor(convertedFbx, "Alpha")->scalar, 0.6);
    EXPECT_EQ(FindFactor(convertedFbx, "Emissive")->values.size(), 3u);
    EXPECT_EQ(FindFactor(convertedFbx, "Specular")->values.size(), 3u);
    EXPECT_NE(convertedFbx.serializedPayload.find("_MetallicMap"), std::string::npos);
    EXPECT_NE(convertedFbx.serializedPayload.find("Metallic.png"), std::string::npos);
    EXPECT_NE(convertedFbx.serializedPayload.find("_RoughnessMap"), std::string::npos);
    EXPECT_NE(convertedFbx.serializedPayload.find("Roughness.png"), std::string::npos);
    EXPECT_NE(convertedFbx.serializedPayload.find("_OpacityMap"), std::string::npos);
    EXPECT_NE(convertedFbx.serializedPayload.find("Opacity.png"), std::string::npos);
    EXPECT_NE(convertedFbx.serializedPayload.find("_EmissiveMap"), std::string::npos);
    EXPECT_NE(convertedFbx.serializedPayload.find("Emissive.png"), std::string::npos);
    EXPECT_NE(convertedFbx.serializedPayload.find("_SpecularMap"), std::string::npos);
    EXPECT_NE(convertedFbx.serializedPayload.find("Specular.png"), std::string::npos);
    EXPECT_NE(
        convertedFbx.serializedPayload.find("property _EmissiveColor Color 0.100000 0.200000 0.300000 1.000000"),
        std::string::npos);
    EXPECT_NE(
        convertedFbx.serializedPayload.find("property _SpecularColor Color 0.700000 0.800000 0.900000 1.000000"),
        std::string::npos);

    NLS::Render::Assets::ImportedSceneNamedRecord obj;
    obj.sourceKey = "mtl/material/BodyPaint";
    obj.name = "BodyPaint";
    obj.materialChannels.push_back({"diffuse", "fbx/texture/diffuse", {0.7, 0.6, 0.5}, false, 0.0});
    obj.materialChannels.push_back({"shininess", {}, {}, true, 64.0});
    obj.materialChannels.push_back({"illumination", {}, {}, true, 7.0});

    const auto convertedObj = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        obj,
        MaterialSourceModel::ObjMtl);
    EXPECT_EQ(convertedObj.workflow, "mtl");
    EXPECT_NE(FindSlot(convertedObj, "BaseColor"), nullptr);
    EXPECT_NE(FindFactor(convertedObj, "SpecularPower"), nullptr);
    EXPECT_EQ(FindFactor(convertedObj, "Roughness"), nullptr)
        << "OBJ shininess should remain legacy specular metadata and must not trigger the FBX-only PBR fallback.";
    EXPECT_TRUE(HasDiagnosticCode(convertedObj, "material-illumination-model-unsupported"));
}

TEST(AssetMaterialConversionTests, FbxMetallicTextureDefaultsMissingFactorToOne)
{
    NLS::Render::Assets::ImportedScene scene;
    scene.textures.push_back({"fbx/texture/metallic", "Metallic", "Metallic.png", "image/png"});

    NLS::Render::Assets::ImportedSceneNamedRecord fbx;
    fbx.sourceKey = "fbx/material/TexturedMetal";
    fbx.name = "TexturedMetal";
    fbx.materialChannels.push_back({"metallic", "fbx/texture/metallic", {}, true, 0.0});

    const auto converted = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        fbx,
        MaterialSourceModel::FbxParserMaterial);

    ASSERT_NE(FindSlot(converted, "Metallic"), nullptr);
    const auto* metallic = FindFactor(converted, "Metallic");
    ASSERT_NE(metallic, nullptr);
    EXPECT_DOUBLE_EQ(metallic->scalar, 1.0)
        << "FBX metalness maps need an identity multiplier when Assimp exposes no authored metallic factor.";
    EXPECT_NE(
        converted.serializedPayload.find("property _Metallic Float 1.000000"),
        std::string::npos);
}

TEST(AssetMaterialConversionTests, FbxPbrTexturesIgnoreUntexturedLegacyWhiteSpecular)
{
    NLS::Render::Assets::ImportedScene scene;
    scene.textures.push_back({"fbx/texture/metallic", "Metallic", "Metallic.png", "image/png"});
    scene.textures.push_back({"fbx/texture/roughness", "Roughness", "Roughness.png", "image/png"});

    NLS::Render::Assets::ImportedSceneNamedRecord fbx;
    fbx.sourceKey = "fbx/material/PbrSurface";
    fbx.name = "PbrSurface";
    fbx.materialChannels.push_back({"metallic", "fbx/texture/metallic", {}, false, 0.0});
    fbx.materialChannels.push_back({"roughness", "fbx/texture/roughness", {}, false, 0.0});
    fbx.materialChannels.push_back({"specular", {}, {1.0, 1.0, 1.0}, false, 0.0});

    const auto converted = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        fbx,
        MaterialSourceModel::FbxParserMaterial);

    EXPECT_EQ(FindFactor(converted, "Specular"), nullptr)
        << "An untextured legacy FBX white specular default must not become an additive PBR color term.";
    EXPECT_NE(
        converted.serializedPayload.find("property _SpecularColor Color 0.000000 0.000000 0.000000 1.000000"),
        std::string::npos);
}

TEST(AssetMaterialConversionTests, FbxPbrTexturesPreserveAuthoredSpecularMapAndTint)
{
    NLS::Render::Assets::ImportedScene scene;
    scene.textures.push_back({"fbx/texture/metallic", "Metallic", "Metallic.png", "image/png"});
    scene.textures.push_back({"fbx/texture/specular", "Specular", "Specular.png", "image/png"});

    NLS::Render::Assets::ImportedSceneNamedRecord mappedFbx;
    mappedFbx.sourceKey = "fbx/material/MappedSpecular";
    mappedFbx.name = "MappedSpecular";
    mappedFbx.materialChannels.push_back({"metallic", "fbx/texture/metallic", {}, true, 0.0});
    mappedFbx.materialChannels.push_back({"specular", "fbx/texture/specular", {1.0, 1.0, 1.0}, false, 0.0});

    const auto mapped = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        mappedFbx,
        MaterialSourceModel::FbxParserMaterial);
    EXPECT_NE(FindSlot(mapped, "Specular"), nullptr);
    EXPECT_NE(FindFactor(mapped, "Specular"), nullptr)
        << "A white factor paired with an authored FBX specular map is not a legacy default.";

    NLS::Render::Assets::ImportedSceneNamedRecord tintedFbx;
    tintedFbx.sourceKey = "fbx/material/TintedSpecular";
    tintedFbx.name = "TintedSpecular";
    tintedFbx.materialChannels.push_back({"metallic", "fbx/texture/metallic", {}, true, 0.0});
    tintedFbx.materialChannels.push_back({"specular", {}, {0.25, 0.5, 0.75}, false, 0.0});

    const auto tinted = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        tintedFbx,
        MaterialSourceModel::FbxParserMaterial);
    const auto* tintedSpecular = FindFactor(tinted, "Specular");
    ASSERT_NE(tintedSpecular, nullptr)
        << "Only the identity-white FBX compatibility default may be filtered.";
    EXPECT_EQ(tintedSpecular->values, (std::vector<double>{0.25, 0.5, 0.75}));
}

TEST(AssetMaterialConversionTests, FbxShininessFallbackGeneratesPbrRoughness)
{
    NLS::Render::Assets::ImportedScene scene;
    scene.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("e2010202-0202-4202-8202-020202020202"));
    scene.textures.push_back({"fbx/texture/roughness", "Roughness", "Roughness.png", "image/png"});

    NLS::Render::Assets::ImportedSceneNamedRecord shininessOnly;
    shininessOnly.sourceKey = "fbx/material/Glossy";
    shininessOnly.name = "Glossy";
    shininessOnly.materialChannels.push_back({"diffuse", {}, {0.8, 0.7, 0.6}, false, 0.0});
    shininessOnly.materialChannels.push_back({"shininess", {}, {}, true, 64.0});

    const auto convertedShininessOnly = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        shininessOnly,
        MaterialSourceModel::FbxParserMaterial);
    const auto* fallbackRoughness = FindFactor(convertedShininessOnly, "Roughness");
    ASSERT_NE(fallbackRoughness, nullptr)
        << "FBX shininess must feed the PBR roughness uniform consumed by StandardPBR and DeferredGBuffer.";
    EXPECT_NEAR(fallbackRoughness->scalar, std::sqrt(2.0 / (64.0 + 2.0)), 1e-6);
    EXPECT_NE(
        convertedShininessOnly.serializedPayload.find("property _Roughness Float 0.174078"),
        std::string::npos);
    EXPECT_NE(FindFactor(convertedShininessOnly, "SpecularPower"), nullptr);

    shininessOnly.materialChannels.push_back({"roughness", {}, {}, true, 0.35});
    const auto convertedExplicitRoughness = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        shininessOnly,
        MaterialSourceModel::FbxParserMaterial);
    const auto* explicitRoughness = FindFactor(convertedExplicitRoughness, "Roughness");
    ASSERT_NE(explicitRoughness, nullptr);
    EXPECT_DOUBLE_EQ(explicitRoughness->scalar, 0.35)
        << "Authored roughness must take precedence over the shininess fallback.";

    NLS::Render::Assets::ImportedSceneNamedRecord roughnessTexture;
    roughnessTexture.sourceKey = "fbx/material/TexturedGloss";
    roughnessTexture.name = "TexturedGloss";
    roughnessTexture.materialChannels.push_back({"diffuse", {}, {0.8, 0.7, 0.6}, false, 0.0});
    roughnessTexture.materialChannels.push_back({"roughness", "fbx/texture/roughness", {}, false, 0.0});
    roughnessTexture.materialChannels.push_back({"shininess", {}, {}, true, 64.0});

    const auto convertedRoughnessTexture = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        roughnessTexture,
        MaterialSourceModel::FbxParserMaterial);
    EXPECT_NE(FindSlot(convertedRoughnessTexture, "Roughness"), nullptr);
    EXPECT_EQ(FindFactor(convertedRoughnessTexture, "Roughness"), nullptr)
        << "A roughness texture already supplies the PBR roughness signal; shininess must not multiply it.";
    EXPECT_NE(
        convertedRoughnessTexture.serializedPayload.find("property _Roughness Float 1.000000"),
        std::string::npos);
    EXPECT_NE(convertedRoughnessTexture.serializedPayload.find("_RoughnessMap"), std::string::npos);
}

TEST(AssetMaterialConversionTests, FbxInvalidRoughnessScalarDoesNotPollutePbrUniform)
{
    NLS::Render::Assets::ImportedScene scene;
    scene.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("e2010505-0505-4505-8505-050505050505"));
    scene.textures.push_back({"fbx/texture/roughness", "Roughness", "Roughness.png", "image/png"});

    NLS::Render::Assets::ImportedSceneNamedRecord textured;
    textured.sourceKey = "fbx/material/InvalidTexturedRoughness";
    textured.name = "InvalidTexturedRoughness";
    textured.materialChannels.push_back({"diffuse", {}, {0.5, 0.5, 0.5}, false, 0.0});
    textured.materialChannels.push_back({"roughness", "fbx/texture/roughness", {}, true, -2.2});

    const auto convertedTextured = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        textured,
        MaterialSourceModel::FbxParserMaterial);
    EXPECT_NE(FindSlot(convertedTextured, "Roughness"), nullptr);
    EXPECT_EQ(FindFactor(convertedTextured, "Roughness"), nullptr)
        << "Parser sentinel roughness values must not multiply an authored roughness texture into invalid PBR space.";
    EXPECT_TRUE(HasDiagnosticCode(convertedTextured, "material-invalid-roughness-scalar"));
    EXPECT_EQ(CountDiagnosticCode(convertedTextured, "material-invalid-roughness-scalar"), 1u);
    EXPECT_NE(
        convertedTextured.serializedPayload.find("property _Roughness Float 1.000000"),
        std::string::npos);
    EXPECT_EQ(convertedTextured.serializedPayload.find("-2.200000"), std::string::npos);

    NLS::Render::Assets::ImportedSceneNamedRecord shininessFallback;
    shininessFallback.sourceKey = "fbx/material/InvalidRoughnessWithShininess";
    shininessFallback.name = "InvalidRoughnessWithShininess";
    shininessFallback.materialChannels.push_back({"diffuse", {}, {0.5, 0.5, 0.5}, false, 0.0});
    shininessFallback.materialChannels.push_back({"roughness", {}, {}, true, -2.2});
    shininessFallback.materialChannels.push_back({"shininess", {}, {}, true, 64.0});

    const auto convertedFallback = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        shininessFallback,
        MaterialSourceModel::FbxParserMaterial);
    const auto* fallbackRoughness = FindFactor(convertedFallback, "Roughness");
    ASSERT_NE(fallbackRoughness, nullptr)
        << "Invalid imported roughness must not suppress the FBX shininess fallback.";
    EXPECT_NEAR(fallbackRoughness->scalar, std::sqrt(2.0 / (64.0 + 2.0)), 1e-6);
    EXPECT_TRUE(HasDiagnosticCode(convertedFallback, "material-invalid-roughness-scalar"));
    EXPECT_EQ(CountDiagnosticCode(convertedFallback, "material-invalid-roughness-scalar"), 1u);
    EXPECT_EQ(convertedFallback.serializedPayload.find("-2.200000"), std::string::npos);

    NLS::Render::Assets::ImportedSceneNamedRecord missingTextureFallback;
    missingTextureFallback.sourceKey = "fbx/material/InvalidRoughnessMissingTexture";
    missingTextureFallback.name = "InvalidRoughnessMissingTexture";
    missingTextureFallback.materialChannels.push_back({"diffuse", {}, {0.5, 0.5, 0.5}, false, 0.0});
    missingTextureFallback.materialChannels.push_back({"roughness", "fbx/texture/missing", {}, true, -2.2});
    missingTextureFallback.materialChannels.push_back({"shininess", {}, {}, true, 64.0});

    const auto convertedMissingTextureFallback = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        missingTextureFallback,
        MaterialSourceModel::FbxParserMaterial);
    EXPECT_EQ(FindSlot(convertedMissingTextureFallback, "Roughness"), nullptr);
    const auto* missingTextureFallbackRoughness = FindFactor(convertedMissingTextureFallback, "Roughness");
    ASSERT_NE(missingTextureFallbackRoughness, nullptr)
        << "A roughness texture key that failed to resolve must not suppress shininess fallback.";
    EXPECT_NEAR(missingTextureFallbackRoughness->scalar, std::sqrt(2.0 / (64.0 + 2.0)), 1e-6);
    EXPECT_TRUE(HasDiagnosticCode(convertedMissingTextureFallback, "material-missing-texture"));
    EXPECT_TRUE(HasDiagnosticCode(convertedMissingTextureFallback, "material-invalid-roughness-scalar"));
    EXPECT_EQ(CountDiagnosticCode(convertedMissingTextureFallback, "material-invalid-roughness-scalar"), 1u);
    EXPECT_EQ(convertedMissingTextureFallback.serializedPayload.find("-2.200000"), std::string::npos);

    const std::array<double, 3> invalidScalars {
        1.2,
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::quiet_NaN()
    };
    for (size_t index = 0u; index < invalidScalars.size(); ++index)
    {
        NLS::Render::Assets::ImportedSceneNamedRecord invalidScalar;
        invalidScalar.sourceKey = "fbx/material/InvalidScalar" + std::to_string(index);
        invalidScalar.name = "InvalidScalar" + std::to_string(index);
        invalidScalar.materialChannels.push_back({"diffuse", {}, {0.5, 0.5, 0.5}, false, 0.0});
        invalidScalar.materialChannels.push_back({"roughness", {}, {}, true, invalidScalars[index]});

        const auto convertedInvalidScalar = NLS::Render::Assets::ConvertImportedSceneMaterial(
            scene,
            invalidScalar,
            MaterialSourceModel::FbxParserMaterial);
        EXPECT_EQ(FindFactor(convertedInvalidScalar, "Roughness"), nullptr);
        EXPECT_TRUE(HasDiagnosticCode(convertedInvalidScalar, "material-invalid-roughness-scalar"));
        EXPECT_EQ(CountDiagnosticCode(convertedInvalidScalar, "material-invalid-roughness-scalar"), 1u);
        EXPECT_NE(
            convertedInvalidScalar.serializedPayload.find("property _Roughness Float 1.000000"),
            std::string::npos);
        EXPECT_EQ(convertedInvalidScalar.serializedPayload.find("nan"), std::string::npos);
        EXPECT_EQ(convertedInvalidScalar.serializedPayload.find("inf"), std::string::npos);
    }
}

TEST(AssetMaterialConversionTests, FbxTexturedDiffuseDoesNotDarkenBaseColorTexture)
{
    NLS::Render::Assets::ImportedScene scene;
    scene.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("e2010606-0606-4606-8606-060606060606"));
    scene.textures.push_back({"fbx/texture/basecolor", "HeroBaseColor", "HeroBaseColor.png", "image/png"});

    NLS::Render::Assets::ImportedSceneNamedRecord textured;
    textured.sourceKey = "fbx/material/TexturedNeutralDiffuse";
    textured.name = "TexturedNeutralDiffuse";
    NLS::Render::Assets::ImportedSceneMaterialChannel neutralDiffuse;
    neutralDiffuse.name = "diffuse";
    neutralDiffuse.textureKey = "fbx/texture/basecolor";
    neutralDiffuse.values = {0.5, 0.5, 0.5};
    textured.materialChannels.push_back(std::move(neutralDiffuse));

    const auto convertedTextured = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        textured,
        MaterialSourceModel::FbxParserMaterial);
    ASSERT_NE(FindSlot(convertedTextured, "BaseColor"), nullptr);
    EXPECT_EQ(FindFactor(convertedTextured, "BaseColor"), nullptr)
        << "Nullus treats neutral textured FBX diffuse factors as a compatibility tint so common FBX exports do not halve the sampled texture.";
    EXPECT_NE(
        convertedTextured.serializedPayload.find("property _BaseColor Color 1.000000 1.000000 1.000000 1.000000"),
        std::string::npos);
    EXPECT_TRUE(HasDiagnosticCode(convertedTextured, "material-ignored-fbx-textured-neutral-diffuse-tint"));

    NLS::Render::Assets::ImportedSceneNamedRecord textureless;
    textureless.sourceKey = "fbx/material/TexturelessDiffuse";
    textureless.name = "TexturelessDiffuse";
    textureless.materialChannels.push_back({"diffuse", {}, {0.25, 0.5, 0.75}, false, 0.0});

    const auto convertedTextureless = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        textureless,
        MaterialSourceModel::FbxParserMaterial);
    const auto* preservedDiffuse = FindFactor(convertedTextureless, "BaseColor");
    ASSERT_NE(preservedDiffuse, nullptr);
    ASSERT_EQ(preservedDiffuse->values.size(), 3u);
    EXPECT_DOUBLE_EQ(preservedDiffuse->values[0], 0.25);
    EXPECT_DOUBLE_EQ(preservedDiffuse->values[1], 0.5);
    EXPECT_DOUBLE_EQ(preservedDiffuse->values[2], 0.75);
    EXPECT_NE(
        convertedTextureless.serializedPayload.find("property _BaseColor Color 0.250000 0.500000 0.750000 1.000000"),
        std::string::npos);

    NLS::Render::Assets::ImportedSceneNamedRecord obj;
    obj.sourceKey = "mtl/material/TexturedTint";
    obj.name = "TexturedTint";
    obj.materialChannels.push_back({"diffuse", "fbx/texture/basecolor", {0.5, 0.5, 0.5}, false, 0.0});

    const auto convertedObj = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        obj,
        MaterialSourceModel::ObjMtl);
    EXPECT_NE(FindFactor(convertedObj, "BaseColor"), nullptr)
        << "The FBX compatibility-tint guard must not change OBJ MTL tint compatibility.";
}

TEST(AssetMaterialConversionTests, FbxTexturedAuthoredDiffuseTintIsPreserved)
{
    NLS::Render::Assets::ImportedScene scene;
    scene.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("e2010808-0808-4808-8808-080808080808"));
    scene.textures.push_back({"fbx/texture/basecolor", "TintedBaseColor", "TintedBaseColor.png", "image/png"});

    NLS::Render::Assets::ImportedSceneNamedRecord tinted;
    tinted.sourceKey = "fbx/material/TexturedAuthoredTint";
    tinted.name = "TexturedAuthoredTint";
    tinted.materialChannels.push_back({"diffuse", "fbx/texture/basecolor", {0.8, 0.7, 0.6}, false, 0.0});

    const auto convertedTinted = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        tinted,
        MaterialSourceModel::FbxParserMaterial);

    ASSERT_NE(FindSlot(convertedTinted, "BaseColor"), nullptr);
    const auto* preservedTint = FindFactor(convertedTinted, "BaseColor");
    ASSERT_NE(preservedTint, nullptr)
        << "The FBX compatibility-tint guard must not discard clearly authored textured diffuse tints.";
    ASSERT_EQ(preservedTint->values.size(), 3u);
    EXPECT_DOUBLE_EQ(preservedTint->values[0], 0.8);
    EXPECT_DOUBLE_EQ(preservedTint->values[1], 0.7);
    EXPECT_DOUBLE_EQ(preservedTint->values[2], 0.6);
    EXPECT_NE(
        convertedTinted.serializedPayload.find("property _BaseColor Color 0.800000 0.700000 0.600000 1.000000"),
        std::string::npos);
    EXPECT_FALSE(HasDiagnosticCode(convertedTinted, "material-ignored-fbx-textured-neutral-diffuse-tint"));
}

TEST(AssetMaterialConversionTests, FbxTexturedNeutralDiffuseTintIsIgnoredByCompatibilityPolicy)
{
    NLS::Render::Assets::ImportedScene scene;
    scene.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("e2010909-0909-4909-8909-090909090909"));
    scene.textures.push_back({"fbx/texture/basecolor", "HalfTintBaseColor", "HalfTintBaseColor.png", "image/png"});

    NLS::Render::Assets::ImportedSceneNamedRecord tinted;
    tinted.sourceKey = "fbx/material/TexturedAuthoredHalfTint";
    tinted.name = "TexturedAuthoredHalfTint";
    tinted.materialChannels.push_back({"diffuse", "fbx/texture/basecolor", {0.5, 0.5, 0.5}, false, 0.0});

    const auto convertedTinted = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        tinted,
        MaterialSourceModel::FbxParserMaterial);

    ASSERT_NE(FindSlot(convertedTinted, "BaseColor"), nullptr);
    EXPECT_EQ(FindFactor(convertedTinted, "BaseColor"), nullptr)
        << "Nullus intentionally ignores neutral textured FBX diffuse factors by default to match glTF brightness for common FBX exports.";
    EXPECT_NE(
        convertedTinted.serializedPayload.find("property _BaseColor Color 1.000000 1.000000 1.000000 1.000000"),
        std::string::npos);
    EXPECT_TRUE(HasDiagnosticCode(convertedTinted, "material-ignored-fbx-textured-neutral-diffuse-tint"));
}

TEST(AssetMaterialConversionTests, FbxTexturedNeutralDiffuseCompatibilityPolicyCoversNonHalfGray)
{
    NLS::Render::Assets::ImportedScene scene;
    scene.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("e2010909-0909-4909-8909-090909090911"));
    scene.textures.push_back({"fbx/texture/basecolor", "GrayTintBaseColor", "GrayTintBaseColor.png", "image/png"});

    NLS::Render::Assets::ImportedSceneNamedRecord tinted;
    tinted.sourceKey = "fbx/material/TexturedAuthoredGrayTint";
    tinted.name = "TexturedAuthoredGrayTint";
    tinted.materialChannels.push_back({"diffuse", "fbx/texture/basecolor", {0.8, 0.8, 0.8}, false, 0.0});

    const auto convertedTinted = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        tinted,
        MaterialSourceModel::FbxParserMaterial);

    ASSERT_NE(FindSlot(convertedTinted, "BaseColor"), nullptr);
    EXPECT_EQ(FindFactor(convertedTinted, "BaseColor"), nullptr);
    EXPECT_TRUE(HasDiagnosticCode(convertedTinted, "material-ignored-fbx-textured-neutral-diffuse-tint"));
}

TEST(AssetMaterialConversionTests, FbxTexturedNeutralDiffuseTintCanBePreservedByPolicyOverride)
{
    NLS::Render::Assets::ImportedScene scene;
    scene.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("e2010909-0909-4909-8909-090909090910"));
    scene.textures.push_back({"fbx/texture/basecolor", "HalfTintBaseColor", "HalfTintBaseColor.png", "image/png"});

    NLS::Render::Assets::ImportedSceneNamedRecord tinted;
    tinted.sourceKey = "fbx/material/TexturedAuthoredHalfTint";
    tinted.name = "TexturedAuthoredHalfTint";
    tinted.materialChannels.push_back({"diffuse", "fbx/texture/basecolor", {0.5, 0.5, 0.5}, false, 0.0});

    NLS::Render::Assets::MaterialConversionContext context;
    context.ignoreFbxTexturedNeutralDiffuseTint = false;

    const auto convertedTinted = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        tinted,
        MaterialSourceModel::FbxParserMaterial,
        context);

    ASSERT_NE(FindSlot(convertedTinted, "BaseColor"), nullptr);
    const auto* preservedTint = FindFactor(convertedTinted, "BaseColor");
    ASSERT_NE(preservedTint, nullptr)
        << "Projects that rely on authored grayscale FBX tints can disable Nullus' compatibility policy.";
    ASSERT_EQ(preservedTint->values.size(), 3u);
    EXPECT_DOUBLE_EQ(preservedTint->values[0], 0.5);
    EXPECT_DOUBLE_EQ(preservedTint->values[1], 0.5);
    EXPECT_DOUBLE_EQ(preservedTint->values[2], 0.5);
    EXPECT_NE(
        convertedTinted.serializedPayload.find("property _BaseColor Color 0.500000 0.500000 0.500000 1.000000"),
        std::string::npos);
    EXPECT_FALSE(HasDiagnosticCode(convertedTinted, "material-ignored-fbx-textured-neutral-diffuse-tint"));
}

TEST(AssetMaterialConversionTests, FbxOpacityTextureWithDecalNameSerializesAsDecalSurface)
{
    NLS::Render::Assets::ImportedScene scene;
    scene.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("e2010707-0707-4707-8707-070707070707"));
    scene.textures.push_back({"fbx/texture/albedo", "DirtDecalBaseColor", "DirtDecalBaseColor.png", "image/png"});
    scene.textures.push_back({"fbx/texture/opacity", "DirtDecalOpacity", "DirtDecalOpacity.png", "image/png"});

    NLS::Render::Assets::ImportedSceneNamedRecord decal;
    decal.sourceKey = "fbx/material/dirt_decal";
    decal.name = "dirt_decal";
    decal.materialChannels.push_back({"diffuse", "fbx/texture/albedo", {1.0, 1.0, 1.0}, false, 0.0});
    decal.materialChannels.push_back({"opacity", "fbx/texture/opacity", {}, false, 0.0});

    const auto convertedDecal = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        decal,
        MaterialSourceModel::FbxParserMaterial);

    ASSERT_NE(FindSlot(convertedDecal, "Opacity"), nullptr);
    EXPECT_EQ(convertedDecal.alphaMode, MaterialAlphaMode::Blend);
    EXPECT_NE(convertedDecal.serializedPayload.find("alphaMode=Blend"), std::string::npos);
    EXPECT_NE(convertedDecal.serializedPayload.find("surfaceMode=Decal"), std::string::npos);
    EXPECT_NE(convertedDecal.serializedPayload.find("depthWrite=false"), std::string::npos);
    EXPECT_NE(convertedDecal.serializedPayload.find("_OpacityMap"), std::string::npos);

    NLS::Render::Assets::ImportedSceneNamedRecord transparent;
    transparent.sourceKey = "fbx/material/window_mask";
    transparent.name = "window_mask";
    transparent.materialChannels.push_back({"opacity", "fbx/texture/opacity", {}, false, 0.0});

    const auto convertedTransparent = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        transparent,
        MaterialSourceModel::FbxParserMaterial);

    EXPECT_EQ(convertedTransparent.alphaMode, MaterialAlphaMode::Blend);
    EXPECT_NE(convertedTransparent.serializedPayload.find("surfaceMode=Transparent"), std::string::npos);
    EXPECT_EQ(convertedTransparent.serializedPayload.find("surfaceMode=Decal"), std::string::npos);
}

TEST(AssetMaterialConversionTests, FbxDecalNamedBaseColorAlphaTextureSerializesAsDecalSurface)
{
    NLS::Render::Assets::ImportedScene scene;
    scene.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("e2010707-0707-4707-8707-070707070708"));
    scene.textures.push_back({"parser/texture/72", "dirt_decal_01", "textures/dirt_decal_01.png", "image/png"});
    scene.textures.push_back({"parser/texture/73", "stone_wall", "textures/stone_wall.png", "image/png"});

    NLS::Render::Assets::ImportedSceneNamedRecord decal;
    decal.sourceKey = "parser/material/21";
    decal.name = "dirt_decal";
    decal.materialChannels.push_back({"diffuse", "parser/texture/72", {1.0, 1.0, 1.0}, false, 0.0});

    NLS::Render::Assets::MaterialConversionContext context;
    context.sourceTextureAlphaEvidence["parser/texture/72"] = true;

    const auto convertedDecal = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        decal,
        MaterialSourceModel::FbxParserMaterial,
        context);

    ASSERT_NE(FindSlot(convertedDecal, "BaseColor"), nullptr);
    EXPECT_EQ(FindSlot(convertedDecal, "Opacity"), nullptr)
        << "Sponza FBX exposes dirt_decal alpha through the base-color texture, not a separate opacity slot.";
    EXPECT_EQ(convertedDecal.alphaMode, MaterialAlphaMode::Blend);
    EXPECT_NE(convertedDecal.serializedPayload.find("alphaMode=Blend"), std::string::npos);
    EXPECT_NE(convertedDecal.serializedPayload.find("surfaceMode=Decal"), std::string::npos);
    EXPECT_NE(convertedDecal.serializedPayload.find("depthWrite=false"), std::string::npos);
    EXPECT_TRUE(HasDiagnosticCode(convertedDecal, "material-inferred-fbx-decal-basecolor-alpha"));

    NLS::Render::Assets::ImportedSceneNamedRecord opaque;
    opaque.sourceKey = "parser/material/22";
    opaque.name = "stone_wall";
    opaque.materialChannels.push_back({"diffuse", "parser/texture/73", {1.0, 1.0, 1.0}, false, 0.0});

    const auto convertedOpaque = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        opaque,
        MaterialSourceModel::FbxParserMaterial);

    EXPECT_EQ(convertedOpaque.alphaMode, MaterialAlphaMode::Opaque);
    EXPECT_NE(convertedOpaque.serializedPayload.find("surfaceMode=Opaque"), std::string::npos);
    EXPECT_EQ(convertedOpaque.serializedPayload.find("surfaceMode=Decal"), std::string::npos);
}

TEST(AssetMaterialConversionTests, FbxDecalNamedBaseColorTextureWithoutAlphaEvidenceRemainsOpaque)
{
    NLS::Render::Assets::ImportedScene scene;
    scene.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("e2010707-0707-4707-8707-070707070709"));
    scene.textures.push_back({"parser/texture/74", "painted_label", "textures/painted_label.png", "image/png"});

    NLS::Render::Assets::ImportedSceneNamedRecord decalNamedOpaque;
    decalNamedOpaque.sourceKey = "parser/material/23";
    decalNamedOpaque.name = "painted_decal_label";
    decalNamedOpaque.materialChannels.push_back({"diffuse", "parser/texture/74", {1.0, 1.0, 1.0}, false, 0.0});

    NLS::Render::Assets::MaterialConversionContext context;
    context.sourceTextureAlphaEvidence["parser/texture/74"] = false;

    const auto converted = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        decalNamedOpaque,
        MaterialSourceModel::FbxParserMaterial,
        context);

    ASSERT_NE(FindSlot(converted, "BaseColor"), nullptr);
    EXPECT_EQ(FindSlot(converted, "Opacity"), nullptr);
    EXPECT_EQ(converted.alphaMode, MaterialAlphaMode::Opaque)
        << "Decal identity alone is not enough; RGB or fully opaque base-color textures must keep opaque depth semantics.";
    EXPECT_NE(converted.serializedPayload.find("surfaceMode=Opaque"), std::string::npos);
    EXPECT_EQ(converted.serializedPayload.find("surfaceMode=Decal"), std::string::npos);
    EXPECT_FALSE(HasDiagnosticCode(converted, "material-inferred-fbx-decal-basecolor-alpha"));
}

TEST(AssetMaterialConversionTests, ParserBumpChannelsOnlyPromoteExplicitNormalMapIdentities)
{
    NLS::Render::Assets::ImportedScene scene;
    scene.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("e2010303-0303-4303-8303-030303030303"));
    scene.textures.push_back({"texture/fbx-normal", "CurtainNormal", "textures/curtain_fabric_Normal.png", "image/png"});
    scene.textures.push_back({"texture/obj-normal", "StoneNormalMap", "textures/StoneNormalMap.tga", "image/tga"});
    scene.textures.push_back({"texture/height", "CurtainHeight", "textures/curtain_height.png", "image/png"});
    scene.textures.push_back({"texture/false-positive", "AbnormalDetail", "textures/abnormal_detail.png", "image/png"});

    const auto convertBump = [&scene](const MaterialSourceModel sourceModel, const std::string& textureKey)
    {
        NLS::Render::Assets::ImportedSceneNamedRecord material;
        material.sourceKey = "material/bump";
        material.name = "BumpMaterial";
        material.materialChannels.push_back({"bump", textureKey, {}, false, 0.0});
        return NLS::Render::Assets::ConvertImportedSceneMaterial(scene, material, sourceModel);
    };

    for (const auto sourceModel : {MaterialSourceModel::FbxParserMaterial, MaterialSourceModel::ObjMtl})
    {
        const auto underscoredNormal = convertBump(sourceModel, "texture/fbx-normal");
        const auto* normalSlot = FindSlot(underscoredNormal, "Normal");
        ASSERT_NE(normalSlot, nullptr);
        EXPECT_EQ(normalSlot->colorSpace, MaterialTextureColorSpace::Linear);
        EXPECT_EQ(CountSlots(underscoredNormal, "Normal"), 1u);
        EXPECT_TRUE(HasDiagnosticCode(underscoredNormal, "material-inferred-normal-map-from-bump-channel"));
        EXPECT_NE(underscoredNormal.serializedPayload.find("_NormalMap"), std::string::npos);
        EXPECT_NE(underscoredNormal.serializedPayload.find("keyword _NORMALMAP"), std::string::npos);

        const auto camelCaseNormal = convertBump(sourceModel, "texture/obj-normal");
        EXPECT_NE(FindSlot(camelCaseNormal, "Normal"), nullptr);

        for (const auto* rejectedTexture : {"texture/height", "texture/false-positive"})
        {
            const auto rejected = convertBump(sourceModel, rejectedTexture);
            EXPECT_EQ(FindSlot(rejected, "Normal"), nullptr);
            EXPECT_TRUE(HasDiagnosticCode(rejected, "material-ignored-bump-height-map"));
            EXPECT_EQ(rejected.serializedPayload.find("keyword _NORMALMAP"), std::string::npos);
        }
    }
}

TEST(AssetMaterialConversionTests, ExplicitNormalSemanticsWinOverBumpInferenceAcrossFormats)
{
    NLS::Render::Assets::ImportedScene scene;
    scene.textures.push_back({"texture/explicit", "AuthoredNormal", "AuthoredNormal.png", "image/png"});
    scene.textures.push_back({"texture/inferred", "FallbackNormal", "Fallback_Normal.png", "image/png"});

    NLS::Render::Assets::ImportedSceneNamedRecord fbx;
    fbx.sourceKey = "fbx/material/normal-priority";
    fbx.materialChannels.push_back({"normal", "texture/explicit", {}, false, 0.0});
    fbx.materialChannels.push_back({"bump", "texture/inferred", {}, false, 0.0});
    const auto convertedFbx = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        fbx,
        MaterialSourceModel::FbxParserMaterial);
    ASSERT_EQ(CountSlots(convertedFbx, "Normal"), 1u);
    EXPECT_EQ(FindSlot(convertedFbx, "Normal")->textureKey, "texture/explicit");
    EXPECT_FALSE(HasDiagnosticCode(convertedFbx, "material-inferred-normal-map-from-bump-channel"));

    NLS::Render::Assets::ImportedSceneNamedRecord gltf;
    gltf.sourceKey = "material/0";
    gltf.normalTextureKey = "texture/explicit";
    const auto convertedGltf = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        gltf,
        MaterialSourceModel::GltfPbrMetallicRoughness);
    ASSERT_EQ(CountSlots(convertedGltf, "Normal"), 1u);
    EXPECT_EQ(FindSlot(convertedGltf, "Normal")->textureKey, "texture/explicit");
    EXPECT_NE(convertedGltf.serializedPayload.find("keyword _NORMALMAP"), std::string::npos);
}

TEST(AssetMaterialConversionTests, ParserScansAllBumpCandidatesForAnExplicitNormalMapIdentity)
{
    NLS::Render::Assets::ImportedScene scene;
    scene.textures.push_back({"texture/height", "WallHeight", "textures/wall_height.png", "image/png"});
    scene.textures.push_back({"texture/normal", "WallNormalMap", "textures/wall_normal_map.png", "image/png"});

    NLS::Render::Assets::ImportedSceneNamedRecord material;
    material.sourceKey = "fbx/material/multiple-bump-candidates";
    material.materialChannels.push_back({"normal", {}, {}, false, 0.0});
    material.materialChannels.push_back({"bump", "texture/height", {}, false, 0.0});
    material.materialChannels.push_back({"bump", "texture/normal", {}, false, 0.0});

    const auto converted = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        material,
        MaterialSourceModel::FbxParserMaterial);

    ASSERT_EQ(CountSlots(converted, "Normal"), 1u);
    EXPECT_EQ(FindSlot(converted, "Normal")->textureKey, "texture/normal");
    EXPECT_TRUE(HasDiagnosticCode(converted, "material-ignored-bump-height-map"));
    EXPECT_TRUE(HasDiagnosticCode(converted, "material-inferred-normal-map-from-bump-channel"));
}

TEST(AssetMaterialConversionTests, PbrShadersSampleNormalMapsWhenEnabled)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto read = [](const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return std::string(
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>());
    };

    const auto standardPbr = read(root / "App/Assets/Engine/Shaders/StandardPBR.hlsl");
    const auto deferredGBuffer = read(root / "App/Assets/Engine/Shaders/DeferredGBuffer.hlsl");
    const auto sharedPbrSurface = read(
        root / "App/Assets/Engine/Shaders/NullusShaderLibrary/StandardPBRSurface.hlsl");
    const auto standard = read(root / "App/Assets/Engine/Shaders/Standard.hlsl");

    ASSERT_FALSE(standard.empty());
    ASSERT_FALSE(standardPbr.empty());
    ASSERT_FALSE(deferredGBuffer.empty());
    ASSERT_FALSE(sharedPbrSurface.empty());

    EXPECT_NE(standard.find("DecodeNormalMapSample"), std::string::npos);
    EXPECT_NE(standard.find("sqrt(saturate(1.0f - dot(xy, xy)))"), std::string::npos);
    EXPECT_NE(sharedPbrSurface.find("NLSDecodeStandardPbrNormalSample"), std::string::npos);
    EXPECT_NE(sharedPbrSurface.find("sqrt(saturate(1.0f - dot(xy, xy)))"), std::string::npos);
    EXPECT_NE(sharedPbrSurface.find("NLSApplyStandardPbrNormalMap"), std::string::npos);
    for (const auto* shader : {&standardPbr, &deferredGBuffer})
    {
        EXPECT_NE(
            shader->find("#include \"NullusShaderLibrary/StandardPBRSurface.hlsl\""),
            std::string::npos);
        EXPECT_NE(shader->find("u_NormalMap.Sample"), std::string::npos);
        EXPECT_NE(shader->find("NLSApplyStandardPbrNormalMap("), std::string::npos);
        EXPECT_NE(shader->find("u_EnableNormalMapping > 0.5f"), std::string::npos);
    }
}

TEST(AssetMaterialConversionTests, PbrShadersOrientBackFaceTangentFramesBeforeNormalMapping)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR) /
        "App" / "Assets" / "Engine" / "Shaders";
    const auto read = [](const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return std::string(
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>());
    };

    const auto commonTypes = read(root / "CommonTypes.hlsli");
    const auto builtIn = read(root / "StandardPBR.hlsl");
    const auto deferredGBuffer = read(root / "DeferredGBuffer.hlsl");
    const auto shaderLab = read(root / "ShaderLab" / "StandardPBR.shader");
    const auto sharedPbrSurface = read(root / "NullusShaderLibrary" / "StandardPBRSurface.hlsl");
    ASSERT_FALSE(commonTypes.empty());
    ASSERT_FALSE(builtIn.empty());
    ASSERT_FALSE(deferredGBuffer.empty());
    ASSERT_FALSE(shaderLab.empty());
    ASSERT_FALSE(sharedPbrSurface.empty());

    EXPECT_NE(
        commonTypes.find("NLSTangentFrame NLSOrientTangentFrameForFace("),
        std::string::npos);
    EXPECT_NE(
        commonTypes.find("const float faceSign = isFrontFace ? 1.0f : -1.0f"),
        std::string::npos);
    EXPECT_NE(commonTypes.find("frame.normalWS *= faceSign"), std::string::npos);
    EXPECT_NE(commonTypes.find("frame.bitangentWS *= faceSign"), std::string::npos);
    EXPECT_EQ(commonTypes.find("frame.tangentWS *= faceSign"), std::string::npos)
        << "Back-face orientation must preserve the tangent while flipping normal and bitangent.";

    const auto orientFrame = sharedPbrSurface.find("NLSOrientTangentFrameForFace(");
    const auto applyNormalMap = sharedPbrSurface.find("NLSApplyTangentNormal(");
    ASSERT_NE(orientFrame, std::string::npos);
    ASSERT_NE(applyNormalMap, std::string::npos);
    EXPECT_LT(orientFrame, applyNormalMap)
        << "The shared helper must orient the tangent frame before applying the normal map.";

    for (const auto* pbrSource : {&builtIn, &deferredGBuffer, &shaderLab})
    {
        EXPECT_NE(pbrSource->find("SV_IsFrontFace"), std::string::npos);
        EXPECT_NE(
            pbrSource->find("#include \"NullusShaderLibrary/StandardPBRSurface.hlsl\""),
            std::string::npos);
        EXPECT_NE(pbrSource->find("NLSApplyStandardPbrNormalMap("), std::string::npos);
    }

    EXPECT_NE(builtIn.find("if (u_EnableNormalMapping > 0.5f)"), std::string::npos);
    EXPECT_NE(builtIn.find("shadingNormalWS = geometryNormalWS"), std::string::npos);

    const auto deferredSafeGeometryNormal = deferredGBuffer.find(
        "const float3 interpolatedGeometryNormalWS = NLSSafeNormalize(input.NormalWS, float3(0.0f, 0.0f, 1.0f));");
    const auto deferredOrientGeometry = deferredGBuffer.find(
        "NLSOrientGeometryNormal(interpolatedGeometryNormalWS, isFrontFace)");
    const auto deferredNoMapShading = deferredGBuffer.find("shadingNormalWS = geometryNormalWS");
    const auto deferredNormalMapBranch = deferredGBuffer.find("if (u_EnableNormalMapping > 0.5f)");
    const auto deferredConstrainShading = deferredGBuffer.find(
        "NLSConstrainShadingNormalToGeometryHemisphere(",
        deferredNormalMapBranch);
    const auto deferredMappedNormalCall = deferredGBuffer.find(
        "NLSApplyStandardPbrNormalMap(",
        deferredNormalMapBranch);
    EXPECT_NE(deferredSafeGeometryNormal, std::string::npos);
    EXPECT_NE(deferredOrientGeometry, std::string::npos);
    EXPECT_NE(deferredNoMapShading, std::string::npos);
    EXPECT_NE(deferredNormalMapBranch, std::string::npos);
    EXPECT_NE(deferredConstrainShading, std::string::npos);
    EXPECT_NE(deferredMappedNormalCall, std::string::npos);
    EXPECT_LT(deferredSafeGeometryNormal, deferredOrientGeometry)
        << "Deferred geometry normals must be safely normalized before face orientation.";
    EXPECT_LT(deferredOrientGeometry, deferredNoMapShading)
        << "The no-normal-map path must preserve the exact oriented geometry normal.";
    EXPECT_LT(deferredNormalMapBranch, deferredMappedNormalCall)
        << "Deferred must only call the TBN normal-map helper inside the enabled branch.";
    EXPECT_LT(deferredNormalMapBranch, deferredConstrainShading)
        << "Only mapped shading normals are constrained to the oriented geometry hemisphere.";

    EXPECT_NE(shaderLab.find("#if defined(_NORMALMAP)"), std::string::npos);
    EXPECT_NE(shaderLab.find("shadingNormalWS = geometryNormalWS"), std::string::npos);
    const auto shaderLabGBuffer = shaderLab.find("Name \"GBuffer\"");
    const auto shaderLabGBufferOrient = shaderLab.find(
        "NLSOrientGeometryNormal(interpolatedGeometryNormalWS, isFrontFace)",
        shaderLabGBuffer);
    const auto shaderLabGBufferConstrain = shaderLab.find(
        "NLSConstrainShadingNormalToGeometryHemisphere(",
        shaderLabGBufferOrient);
    EXPECT_NE(shaderLabGBuffer, std::string::npos);
    EXPECT_NE(shaderLabGBufferOrient, std::string::npos);
    EXPECT_NE(shaderLabGBufferConstrain, std::string::npos);
    EXPECT_LT(shaderLabGBufferOrient, shaderLabGBufferConstrain)
        << "ShaderLab GBuffer normal maps must preserve the oriented geometry hemisphere.";

    EXPECT_NE(shaderLab.find("#pragma shader_feature _ALPHATEST_ON"), std::string::npos);
    EXPECT_NE(shaderLab.find("#pragma multi_compile _ _NORMALMAP"), std::string::npos);
    EXPECT_NE(shaderLab.find("#pragma multi_compile _ MAIN_LIGHT_SHADOWS"), std::string::npos);
}

TEST(AssetMaterialConversionTests, PbrShadersGuardDegenerateNormalMapInputs)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto read = [](const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return std::string(
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>());
    };

    const auto commonTypes = read(root / "App/Assets/Engine/Shaders/CommonTypes.hlsli");
    const auto standardPbr = read(root / "App/Assets/Engine/Shaders/StandardPBR.hlsl");
    const auto deferredGBuffer = read(root / "App/Assets/Engine/Shaders/DeferredGBuffer.hlsl");
    const auto deferredLighting = read(root / "App/Assets/Engine/Shaders/DeferredLighting.hlsl");
    const auto pbrNormals = read(
        root / "App/Assets/Engine/Shaders/NullusShaderLibrary/PBRNormals.hlsl");
    const auto sharedPbrSurface = read(
        root / "App/Assets/Engine/Shaders/NullusShaderLibrary/StandardPBRSurface.hlsl");
    const auto lightGridCommon = read(root / "App/Assets/Engine/Shaders/LightGridCommon.hlsli");
    const auto standard = read(root / "App/Assets/Engine/Shaders/Standard.hlsl");

    ASSERT_FALSE(commonTypes.empty());
    ASSERT_FALSE(lightGridCommon.empty());
    ASSERT_FALSE(standard.empty());
    ASSERT_FALSE(standardPbr.empty());
    ASSERT_FALSE(deferredGBuffer.empty());
    ASSERT_FALSE(deferredLighting.empty());
    ASSERT_FALSE(pbrNormals.empty());
    ASSERT_FALSE(sharedPbrSurface.empty());

    EXPECT_NE(commonTypes.find("NLSIsFinite3"), std::string::npos);
    EXPECT_NE(commonTypes.find("NLSSafeNormalize"), std::string::npos);
    EXPECT_NE(commonTypes.find("NLSTransformNormalDirection"), std::string::npos);
    EXPECT_NE(commonTypes.find("cross(row1, row2)"), std::string::npos);
    EXPECT_NE(commonTypes.find("NLSSafePerpendicular"), std::string::npos);
    EXPECT_NE(commonTypes.find("NLSBuildSafeTangentFrame"), std::string::npos);
    EXPECT_NE(commonTypes.find("NLSApplyTangentNormal"), std::string::npos);
    EXPECT_NE(commonTypes.find("NLSNormalizeFallback"), std::string::npos);
    EXPECT_NE(commonTypes.find("cross(frame.normalWS, frame.tangentWS)"), std::string::npos);
    EXPECT_NE(lightGridCommon.find("NLSSafeLightingNormalize"), std::string::npos);
    EXPECT_NE(lightGridCommon.find("NLSSafeLightingPerpendicular"), std::string::npos);
    EXPECT_EQ(lightGridCommon.find("normalize("), std::string::npos);

    EXPECT_NE(standard.find("NLSBuildSafeTangentFrame("), std::string::npos);
    EXPECT_NE(standard.find("NLSApplyTangentNormal(tangentNormal, tangentFrame)"), std::string::npos);
    EXPECT_NE(sharedPbrSurface.find("NLSTransformNormalDirection(model, normalOS)"), std::string::npos);
    EXPECT_NE(sharedPbrSurface.find("NLSBuildSafeTangentFrame("), std::string::npos);
    EXPECT_NE(sharedPbrSurface.find("NLSDecodeStandardPbrNormalSample"), std::string::npos);
    EXPECT_NE(sharedPbrSurface.find("NLSApplyTangentNormal("), std::string::npos);
    EXPECT_NE(sharedPbrSurface.find("NLSPackStandardPbrGBuffer("), std::string::npos);
    EXPECT_NE(sharedPbrSurface.find("NLSPackOctNormalToUnorm("), std::string::npos);
    for (const auto* shader : {&standardPbr, &deferredGBuffer})
    {
        EXPECT_NE(
            shader->find("#include \"NullusShaderLibrary/StandardPBRSurface.hlsl\""),
            std::string::npos);
        EXPECT_NE(shader->find("NLSBuildStandardPbrTangentFrame("), std::string::npos);
        EXPECT_NE(shader->find("NLSApplyStandardPbrNormalMap("), std::string::npos);
    }
    EXPECT_NE(deferredGBuffer.find("NLSPackStandardPbrGBuffer("), std::string::npos);
    EXPECT_NE(
        deferredGBuffer.find("u_ObjectFlags & NLS_OBJECT_FLAG_RECEIVE_SHADOWS"),
        std::string::npos);
    EXPECT_NE(
        deferredLighting.find("NLSUnpackOctNormalFromUnorm("),
        std::string::npos);
    EXPECT_EQ(deferredLighting.find("NLSDeferredSafeNormalize"), std::string::npos);
    EXPECT_NE(pbrNormals.find("NLSSafeNormalize(shadingNormalWS"), std::string::npos);
    EXPECT_NE(
        deferredLighting.find("NLSConstrainShadingNormalToGeometryHemisphere("),
        std::string::npos);
    EXPECT_NE(
        deferredLighting.find("const bool receiveShadows = materialSample.a >= 0.5f"),
        std::string::npos);
    const auto countSamples = [](const std::string& source, const std::string_view texture)
    {
        size_t count = 0u;
        for (size_t offset = 0u;
             (offset = source.find(texture, offset)) != std::string::npos;
             offset += texture.size())
        {
            ++count;
        }
        return count;
    };
    EXPECT_EQ(countSamples(deferredLighting, "u_GBufferAlbedo.Sample("), 1u);
    EXPECT_EQ(countSamples(deferredLighting, "u_GBufferNormal.Sample("), 1u);
    EXPECT_EQ(countSamples(deferredLighting, "u_GBufferMaterial.Sample("), 1u);
    EXPECT_EQ(
        deferredLighting.find("normalize(normalSample.rgb * 2.0f - 1.0f)"),
        std::string::npos);
}

TEST(AssetMaterialConversionTests, PbrDirectLightingUsesEnergyConservingCookTorranceBrdf)
{
    const auto path = std::filesystem::path(NLS_ROOT_DIR) /
        "App" / "Assets" / "Engine" / "Shaders" / "LightGridCommon.hlsli";
    std::ifstream input(path, std::ios::binary);
    const std::string shader {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(shader.empty());
    const std::string requiredTokens[] = {
        "NLS_PBR_MIN_PERCEPTUAL_ROUGHNESS",
        "NLS_PBR_MIN_DISTRIBUTION_DENOMINATOR",
        "NLSDistributionGGX",
        "NLSGeometrySchlickGGX",
        "NLSGeometrySmith",
        "NLSFresnelSchlick",
        "NLSEvaluateCookTorranceDirect",
        "lerp(dielectricF0, safeAlbedo, safeMetallic)",
        "(1.0f.xxx - fresnel) * (1.0f - safeMetallic)",
        "diffuse + specular",
        "radiance * ndotl"
    };
    for (const auto& token : requiredTokens)
        EXPECT_NE(shader.find(token), std::string::npos) << token;

    EXPECT_EQ(shader.find("specularHint"), std::string::npos)
        << "The heuristic highlight must not remain beside the Cook-Torrance implementation.";

    size_t evaluatorOccurrences = 0u;
    size_t position = 0u;
    while ((position = shader.find("NLSEvaluateCookTorranceDirect(", position)) != std::string::npos)
    {
        ++evaluatorOccurrences;
        position += 1u;
    }
    EXPECT_GE(evaluatorOccurrences, 3u)
        << "The evaluator definition plus forward-clustered and deferred-scene call sites must share one BRDF.";
}

TEST(AssetMaterialConversionTests, PbrDirectLightingPreservesArtistLightIntensityUnits)
{
    const auto path = std::filesystem::path(NLS_ROOT_DIR) /
        "App" / "Assets" / "Engine" / "Shaders" / "LightGridCommon.hlsli";
    std::ifstream input(path, std::ios::binary);
    const std::string shader {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(shader.empty());
    EXPECT_NE(shader.find("NLS_PBR_ARTIST_LIGHT_INTENSITY_TO_RADIANCE"), std::string::npos);
    EXPECT_NE(
        shader.find("safeIntensity * NLS_PBR_ARTIST_LIGHT_INTENSITY_TO_RADIANCE"),
        std::string::npos)
        << "Cook-Torrance's 1/pi diffuse normalization must be paired with the existing "
           "artist-authored light intensity convention so point lights do not become three times dimmer.";
}

TEST(AssetMaterialConversionTests, PbrDiffuseLightingDoesNotDisappearWhenMappedNormalFacesAwayFromView)
{
    const auto path = std::filesystem::path(NLS_ROOT_DIR) /
        "App" / "Assets" / "Engine" / "Shaders" / "LightGridCommon.hlsli";
    std::ifstream input(path, std::ios::binary);
    const std::string shader {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(shader.empty());
    EXPECT_EQ(shader.find("NLS_PBR_MIN_NDOTV"), std::string::npos)
        << "A negative mapped NdotV must not be promoted to an epsilon that still evaluates GGX specular.";
    EXPECT_EQ(shader.find("ndotv <= 0.0f || ndotl <= 0.0f"), std::string::npos)
        << "NdotV only participates in the specular visibility term. Rejecting the entire BRDF on NdotV "
           "creates a hard camera-following black region when a normal map tilts the shading normal away.";
    EXPECT_NE(shader.find("float3 specular = 0.0f.xxx"), std::string::npos);
    EXPECT_NE(shader.find("if (ndotv > 0.0f)"), std::string::npos)
        << "Back-facing mapped normals may keep diffuse lighting, but must not evaluate the GGX specular lobe.";
}

TEST(AssetMaterialConversionTests, PbrLdrOutputsToneMapHdrHighlightsInsteadOfClippingFireflies)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR) /
        "App" / "Assets" / "Engine" / "Shaders";
    const auto read = [](const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return std::string(
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>());
    };

    const auto common = read(root / "LightGridCommon.hlsli");
    const auto builtIn = read(root / "StandardPBR.hlsl");
    const auto shaderLab = read(root / "ShaderLab" / "StandardPBR.shader");
    const auto deferred = read(root / "DeferredLighting.hlsl");
    ASSERT_FALSE(common.empty());
    ASSERT_FALSE(builtIn.empty());
    ASSERT_FALSE(shaderLab.empty());
    ASSERT_FALSE(deferred.empty());

    EXPECT_NE(common.find("float3 NLSToneMapACES("), std::string::npos);
    EXPECT_NE(common.find("peakChannel - 1.0f"), std::string::npos)
        << "The firefly shoulder must preserve the normal 0..1 lighting range.";
    EXPECT_NE(common.find("compressedPeak"), std::string::npos)
        << "Finite firefly peaks need a hue-preserving soft shoulder before the ACES fit; ACES alone "
           "approaches display white too quickly for isolated GGX samples.";
    EXPECT_EQ(common.find("hdrColor /= 1.0f + peakChannel"), std::string::npos)
        << "A global Reinhard compression darkens ordinary material colors before ACES.";
    EXPECT_NE(common.find("hdrColor * (a * hdrColor + b)"), std::string::npos);
    for (const auto* outputShader : {&builtIn, &shaderLab, &deferred})
    {
        EXPECT_NE(outputShader->find("NLSToneMapACES("), std::string::npos)
            << "Every PBR path that writes HDR lighting into an LDR target must use the shared output transform.";
    }
    EXPECT_NE(deferred.find("NLSToneMapACES(skyboxColor.rgb)"), std::string::npos);
    EXPECT_NE(deferred.find("NLSToneMapACES(EvalProceduralSky(skyDirection))"), std::string::npos)
        << "Deferred geometry and sky branches share one LDR target and must use the same output transform.";
    EXPECT_EQ(common.find("clamp(lighting"), std::string::npos)
        << "Fireflies must be compressed at the output transform, not by clipping BRDF energy.";
}

TEST(AssetMaterialConversionTests, PbrDirectLightingFiltersSubpixelNormalVarianceIntoRoughness)
{
    const auto path = std::filesystem::path(NLS_ROOT_DIR) /
        "App" / "Assets" / "Engine" / "Shaders" / "LightGridCommon.hlsli";
    std::ifstream input(path, std::ios::binary);
    const std::string shader {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(shader.empty());
    const std::string requiredTokens[] = {
        "NLSFilterPerceptualRoughness",
        "ddx(safeNormalWS)",
        "ddy(safeNormalWS)",
        "roughnessSquared + normalVariance",
        "filteredRoughness"
    };
    for (const auto& token : requiredTokens)
        EXPECT_NE(shader.find(token), std::string::npos) << token;

    size_t filterOccurrences = 0u;
    size_t position = 0u;
    while ((position = shader.find("NLSFilterPerceptualRoughness(", position)) != std::string::npos)
    {
        ++filterOccurrences;
        position += 1u;
    }
    EXPECT_GE(filterOccurrences, 3u)
        << "The filter definition plus forward and deferred PBR paths must share specular anti-aliasing.";
    EXPECT_EQ(shader.find("clamp(lighting"), std::string::npos)
        << "Fireflies must be addressed by filtering the BRDF inputs, not by clipping output luminance.";
}

TEST(AssetMaterialConversionTests, BuiltInPbrShadersUseAuthoredMetallicRoughnessChannels)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR) /
        "App" / "Assets" / "Engine" / "Shaders";
    const auto read = [](const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return std::string(
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>());
    };

    const auto standardPbr = read(root / "StandardPBR.hlsl");
    const auto deferredGBuffer = read(root / "DeferredGBuffer.hlsl");
    ASSERT_FALSE(standardPbr.empty());
    ASSERT_FALSE(deferredGBuffer.empty());

    for (const auto* shader : {&standardPbr, &deferredGBuffer})
    {
        EXPECT_NE(shader->find("float4 u_MetallicMapChannel"), std::string::npos);
        EXPECT_NE(shader->find("float4 u_RoughnessMapChannel"), std::string::npos);
        EXPECT_NE(shader->find("dot(u_MetallicMap.Sample(u_LinearWrapSampler, texCoord), u_MetallicMapChannel)"), std::string::npos);
        EXPECT_NE(shader->find("dot(u_RoughnessMap.Sample(u_LinearWrapSampler, texCoord), u_RoughnessMapChannel)"), std::string::npos);
        EXPECT_EQ(shader->find("u_MetallicMap.Sample(u_LinearWrapSampler, texCoord).r"), std::string::npos);
        EXPECT_EQ(shader->find("u_RoughnessMap.Sample(u_LinearWrapSampler, texCoord).r"), std::string::npos);
    }
}

TEST(AssetMaterialConversionTests, PbrForwardShadersDoNotAddLegacySpecularMapsAsEmissiveLight)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR) /
        "App" / "Assets" / "Engine" / "Shaders";
    const auto read = [](const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return std::string(
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>());
    };

    const auto builtIn = read(root / "StandardPBR.hlsl");
    const auto shaderLab = read(root / "ShaderLab" / "StandardPBR.shader");
    ASSERT_FALSE(builtIn.empty());
    ASSERT_FALSE(shaderLab.empty());

    for (const auto* shader : {&builtIn, &shaderLab})
    {
        EXPECT_EQ(shader->find("lighting + emissive + specular"), std::string::npos)
            << "A legacy specular texture is an F0 input, not unshadowed emitted light.";
        EXPECT_NE(shader->find("lighting + emissive"), std::string::npos);
    }
}

TEST(AssetMaterialConversionTests, SafeTangentFrameFallbacksKeepMappedNormalsFinite)
{
    const TestVec3 nanVec {
        std::numeric_limits<double>::quiet_NaN(),
        0.0,
        0.0
    };
    const TestVec3 infVec {
        std::numeric_limits<double>::infinity(),
        0.0,
        0.0
    };
    const std::array<TestTangentFrame, 5> frames {
        TestBuildSafeTangentFrame({0.0, 0.0, 1.0}, {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}),
        TestBuildSafeTangentFrame({0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}),
        TestBuildSafeTangentFrame(nanVec, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}),
        TestBuildSafeTangentFrame({0.0, 0.0, 1.0}, infVec, {0.0, 1.0, 0.0}),
        TestBuildSafeTangentFrame({0.0, 0.0, 1.0}, {1.0, 0.0, 0.0}, {2.0, 0.0, 0.0})
    };
    const std::array<TestVec3, 4> tangentNormals {
        TestVec3{0.0, 0.0, 1.0},
        TestVec3{0.0, 0.0, 0.0},
        nanVec,
        infVec
    };

    for (const auto& frame : frames)
    {
        EXPECT_TRUE(TestIsFinite3(frame.normal));
        EXPECT_TRUE(TestIsFinite3(frame.tangent));
        EXPECT_TRUE(TestIsFinite3(frame.bitangent));
        EXPECT_NEAR(Dot(frame.normal, frame.normal), 1.0, 1e-9);
        EXPECT_NEAR(Dot(frame.tangent, frame.tangent), 1.0, 1e-9);
        EXPECT_NEAR(Dot(frame.bitangent, frame.bitangent), 1.0, 1e-9);
        EXPECT_NEAR(Dot(frame.normal, frame.tangent), 0.0, 1e-9);
        EXPECT_NEAR(Dot(frame.normal, frame.bitangent), 0.0, 1e-9);
        EXPECT_NEAR(Dot(frame.tangent, frame.bitangent), 0.0, 1e-9);

        for (const auto tangentNormal : tangentNormals)
        {
            const auto mapped = TestApplyTangentNormal(tangentNormal, frame);
            EXPECT_TRUE(TestIsFinite3(mapped));
            EXPECT_NEAR(Dot(mapped, mapped), 1.0, 1e-9);
        }
    }
}

TEST(AssetMaterialConversionTests, MissingAndUnsupportedTexturesProduceDiagnosticsWithColorSpacePolicy)
{
    NLS::Render::Assets::ImportedScene scene;
    scene.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("e3010101-0101-4101-8101-010101010101"));
    scene.textures.push_back({"image/normal", "PackedNormal", "PackedNormal.gif", "image/gif"});

    NLS::Render::Assets::ImportedSceneNamedRecord material;
    material.sourceKey = "material/broken";
    material.name = "Broken";
    material.baseColorTextureKey = "image/missing";
    material.normalTextureKey = "image/normal";

    const auto converted = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        material,
        MaterialSourceModel::GltfPbrMetallicRoughness);

    EXPECT_TRUE(HasDiagnosticCode(converted, "material-missing-texture"));
    EXPECT_TRUE(HasDiagnosticCode(converted, "material-unsupported-texture-encoding"));
    const auto* normal = FindSlot(converted, "Normal");
    ASSERT_NE(normal, nullptr);
    EXPECT_EQ(normal->colorSpace, MaterialTextureColorSpace::Linear);

    const auto payloads = NLS::Render::Assets::BuildMaterialArtifactPayloads({converted});
    ASSERT_EQ(payloads.size(), 1u);
    EXPECT_EQ(payloads.front().subAssetKey, "material:material/broken");
    EXPECT_EQ(payloads.front().artifactType, NLS::Core::Assets::ArtifactType::Material);
}

TEST(AssetMaterialConversionTests, MaterialArtifactPayloadPathsAreSafeForArtifactWriter)
{
    const auto assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("e4010101-0101-4101-8101-010101010101"));
    NLS::Render::Assets::ConvertedMaterialArtifact material;
    material.subAssetKey = "material:material/body";
    material.displayName = "Body";
    material.serializedPayload = "MATERIAL=material:material/body\n";

    const auto payloads = NLS::Render::Assets::BuildMaterialArtifactPayloads({material});
    ASSERT_EQ(payloads.size(), 1u);
    EXPECT_EQ(payloads.front().relativePath.generic_string().find(':'), std::string::npos);

    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_material_payload_" + NLS::Guid::New().ToString());
    NLS::Core::Assets::ArtifactWriteRequest request;
    request.sourceAssetId = assetId;
    request.importerId = "scene-model";
    request.targetPlatform = "editor-windows";
    request.primarySubAssetKey = material.subAssetKey;
    request.artifacts = payloads;

    const NLS::Core::Assets::ArtifactWriter writer(root / "staging", root / "committed");
    const auto result = writer.WriteAndCommit(request, nullptr);
    std::filesystem::remove_all(root);

    ASSERT_TRUE(result.committed);
    ASSERT_EQ(result.manifest.subAssets.size(), 1u);
    EXPECT_EQ(result.manifest.subAssets.front().subAssetKey, material.subAssetKey);
    EXPECT_FALSE(std::filesystem::path(result.manifest.subAssets.front().artifactPath).filename().has_extension());
    EXPECT_EQ(result.manifest.subAssets.front().artifactPath.find(".mat"), std::string::npos);
}

TEST(AssetMaterialConversionTests, MaterialArtifactPayloadPathsPreserveDistinctSubAssetKeys)
{
    NLS::Render::Assets::ConvertedMaterialArtifact colonMaterial;
    colonMaterial.subAssetKey = "material:body";
    colonMaterial.serializedPayload = "MATERIAL=material:body\n";

    NLS::Render::Assets::ConvertedMaterialArtifact slashMaterial;
    slashMaterial.subAssetKey = "material/body";
    slashMaterial.serializedPayload = "MATERIAL=material/body\n";

    const auto payloads = NLS::Render::Assets::BuildMaterialArtifactPayloads({
        colonMaterial,
        slashMaterial
    });

    ASSERT_EQ(payloads.size(), 2u);
    EXPECT_NE(payloads[0].relativePath, payloads[1].relativePath);
    EXPECT_EQ(payloads[0].relativePath.parent_path().generic_string(), "materials");
    EXPECT_EQ(payloads[1].relativePath.parent_path().generic_string(), "materials");
    EXPECT_FALSE(payloads[0].relativePath.filename().has_extension());
    EXPECT_FALSE(payloads[1].relativePath.filename().has_extension());
}
