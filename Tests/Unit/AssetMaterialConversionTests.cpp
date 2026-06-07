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
#include "Rendering/Settings/DriverSettings.h"
#include "Core/ResourceManagement/ShaderManager.h"
#include "Core/ResourceManagement/MaterialManager.h"
#include "Core/ResourceManagement/TextureManager.h"
#include "Core/ServiceLocator.h"
#include "Assets/NativeArtifactContainer.h"
#include "Debug/Logger.h"
#include "Guid.h"
#include "Image.h"
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
#include <string>
#include <string_view>
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

NLS::Render::Resources::ShaderReflection MakeAlbedoMapShaderReflection()
{
    NLS::Render::Resources::ShaderReflection reflection;
    reflection.properties = {
        {
            "u_AlbedoMap",
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
        "u_NormalMap",
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
        "u_MetallicMap",
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
        "u_RoughnessMap",
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
            "Library/Artifacts/shader-guid/shader.nshader"
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
            "Library/Artifacts/shader-guid/shader.nshader"
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

NLS::Render::Assets::ShaderArtifact MakeStandardPbrShaderArtifact()
{
    return MakeShaderArtifact(
        "App/Assets/Engine/Shaders/StandardPBR.hlsl",
        "shader:StandardPBR",
        MakeStandardPbrShaderReflection());
}

std::filesystem::path WriteStandardPbrShaderArtifact(const std::filesystem::path& root)
{
    const auto shaderArtifactPath = root / "Library" / "Artifacts" / "shader-guid" / "shader.nshader";
    WriteBinaryFile(shaderArtifactPath, NLS::Render::Assets::SerializeShaderArtifact(MakeStandardPbrShaderArtifact()));
    return shaderArtifactPath;
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
    EXPECT_NE(
        material.serializedPayload.find("value=\"Models/Hero/textures/BaseColor.png\""),
        std::string::npos);
    EXPECT_NE(
        material.serializedPayload.find("resourcePath=\"Models/Hero/textures/BaseColor.png\""),
        std::string::npos);
    EXPECT_EQ(material.serializedPayload.find("value=\"image/0\""), std::string::npos);
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

TEST(AssetMaterialConversionTests, ConvertedMaterialPayloadUsesRuntimeMaterialXmlSchema)
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

    EXPECT_NE(material.serializedPayload.find("<root>"), std::string::npos);
    EXPECT_NE(material.serializedPayload.find("<shader>:Shaders/StandardPBR.hlsl</shader>"), std::string::npos);
    EXPECT_NE(material.serializedPayload.find("<blendable>true</blendable>"), std::string::npos);
    EXPECT_NE(material.serializedPayload.find("<surfaceMode>Transparent</surfaceMode>"), std::string::npos);
    EXPECT_NE(material.serializedPayload.find("<backfaceCulling>false</backfaceCulling>"), std::string::npos);
    EXPECT_NE(material.serializedPayload.find("<depthWriting>false</depthWriting>"), std::string::npos);
    EXPECT_NE(material.serializedPayload.find("<alphaMode>Blend</alphaMode>"), std::string::npos);
    EXPECT_NE(material.serializedPayload.find("<alphaCutoff>0.400000</alphaCutoff>"), std::string::npos);
    EXPECT_NE(
        material.serializedPayload.find("<uniform name=\"u_Albedo\" type=\"vec4\" value=\"0.800000 0.700000 0.600000 0.500000\"/>"),
        std::string::npos);
    EXPECT_NE(
        material.serializedPayload.find("<uniform name=\"u_Metallic\" type=\"float\" value=\"0.250000\"/>"),
        std::string::npos);
    EXPECT_NE(
        material.serializedPayload.find("<uniform name=\"u_Roughness\" type=\"float\" value=\"0.750000\"/>"),
        std::string::npos);
    EXPECT_EQ(material.serializedPayload.find("MATERIAL="), std::string::npos);
    EXPECT_EQ(material.serializedPayload.find("TEXTURE_SLOT="), std::string::npos);
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
    EXPECT_NE(material.serializedPayload.find("<name>dirt_decal</name>"), std::string::npos);
    EXPECT_NE(material.serializedPayload.find("<surfaceMode>Decal</surfaceMode>"), std::string::npos);
    EXPECT_NE(material.serializedPayload.find("<blendable>true</blendable>"), std::string::npos);
    EXPECT_NE(material.serializedPayload.find("<depthWriting>false</depthWriting>"), std::string::npos);
    EXPECT_EQ(material.serializedPayload.find("<surfaceMode>Transparent</surfaceMode>"), std::string::npos);
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
    EXPECT_NE(material.serializedPayload.find("<surfaceMode>Decal</surfaceMode>"), std::string::npos);
    EXPECT_EQ(material.serializedPayload.find("<surfaceMode>Transparent</surfaceMode>"), std::string::npos);
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
    EXPECT_NE(material.serializedPayload.find("<surfaceMode>Decal</surfaceMode>"), std::string::npos);
    EXPECT_EQ(material.serializedPayload.find("<surfaceMode>Transparent</surfaceMode>"), std::string::npos);
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
    EXPECT_NE(material.serializedPayload.find("<surfaceMode>Transparent</surfaceMode>"), std::string::npos);
    EXPECT_EQ(material.serializedPayload.find("<surfaceMode>Decal</surfaceMode>"), std::string::npos);
    EXPECT_NE(material.serializedPayload.find("<blendable>true</blendable>"), std::string::npos);
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
    EXPECT_NE(material.serializedPayload.find("<alphaMode>Mask</alphaMode>"), std::string::npos);
    EXPECT_NE(material.serializedPayload.find("<alphaCutoff>0.350000</alphaCutoff>"), std::string::npos);
    EXPECT_NE(material.serializedPayload.find("<surfaceMode>Opaque</surfaceMode>"), std::string::npos);
    EXPECT_NE(material.serializedPayload.find("<blendable>false</blendable>"), std::string::npos);
    EXPECT_EQ(material.serializedPayload.find("<surfaceMode>AlphaTest</surfaceMode>"), std::string::npos);
}

TEST(AssetMaterialConversionTests, MaterialLoaderAppliesExplicitDecalSurfaceMode)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_decal_material_" + NLS::Guid::New().ToString());
    const auto shaderArtifactPath = WriteStandardPbrShaderArtifact(root);
    static NLS::Core::ResourceManagement::ShaderManager shaderManager;
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::ShaderManager>(shaderManager);
    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(shaderArtifactPath.string());
    ASSERT_NE(shader, nullptr);
    shaderManager.RegisterResource(":Shaders/StandardPBR.hlsl", shader);

    const auto materialPath = root / "Decal.nmat";
    {
        std::ofstream output(materialPath, std::ios::binary | std::ios::trunc);
        output <<
            "<root>\n"
            "  <shader>:Shaders/StandardPBR.hlsl</shader>\n"
            "  <surfaceMode>Decal</surfaceMode>\n"
            "  <blendable>true</blendable>\n"
            "  <backfaceCulling>false</backfaceCulling>\n"
            "  <frontfaceCulling>false</frontfaceCulling>\n"
            "  <depthTest>true</depthTest>\n"
            "  <depthWriting>false</depthWriting>\n"
            "  <colorWriting>true</colorWriting>\n"
            "  <gpuInstances>1</gpuInstances>\n"
            "</root>\n";
    }

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

TEST(AssetMaterialConversionTests, MaterialLoaderInfersLegacyDecalSurfaceModeFromSerializedName)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_legacy_decal_material_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(root);

    const auto materialPath = root / "LegacyDirtDecal.nmat";
    {
        std::ofstream output(materialPath, std::ios::binary | std::ios::trunc);
        output <<
            "<root>\n"
            "  <shader>?</shader>\n"
            "  <name>dirt_decal</name>\n"
            "  <sourceSubAsset>material:material/21</sourceSubAsset>\n"
            "  <blendable>true</blendable>\n"
            "</root>\n";
    }

    auto* loaded = NLS::Render::Resources::Loaders::MaterialLoader::Create(materialPath.string());
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->GetSurfaceMode(), NLS::Render::Resources::MaterialSurfaceMode::Decal);
    EXPECT_TRUE(loaded->IsDecal());
    EXPECT_TRUE(loaded->IsBlendable());

    EXPECT_TRUE(NLS::Render::Resources::Loaders::MaterialLoader::Destroy(loaded));
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialLoaderKeepsLegacyNonDecalBlendAsTransparent)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_legacy_transparent_material_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(root);

    const auto materialPath = root / "LegacyGlass.nmat";
    {
        std::ofstream output(materialPath, std::ios::binary | std::ios::trunc);
        output <<
            "<root>\n"
            "  <shader>?</shader>\n"
            "  <name>HeroGlass</name>\n"
            "  <blendable>true</blendable>\n"
            "</root>\n";
    }

    auto* loaded = NLS::Render::Resources::Loaders::MaterialLoader::Create(materialPath.string());
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->GetSurfaceMode(), NLS::Render::Resources::MaterialSurfaceMode::Transparent);
    EXPECT_FALSE(loaded->IsDecal());
    EXPECT_TRUE(loaded->IsBlendable());

    EXPECT_TRUE(NLS::Render::Resources::Loaders::MaterialLoader::Destroy(loaded));
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialLoaderIgnoresDecalTokenInSourceWhenNameIsNonDecal)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_legacy_source_decal_transparent_material_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(root);

    const auto materialPath = root / "LegacyGlassFromDecalFolder.nmat";
    {
        std::ofstream output(materialPath, std::ios::binary | std::ios::trunc);
        output <<
            "<root>\n"
            "  <shader>?</shader>\n"
            "  <name>HeroGlass</name>\n"
            "  <sourceSubAsset>material:decal_folder/hero_glass</sourceSubAsset>\n"
            "  <blendable>true</blendable>\n"
            "</root>\n";
    }

    auto* loaded = NLS::Render::Resources::Loaders::MaterialLoader::Create(materialPath.string());
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->GetSurfaceMode(), NLS::Render::Resources::MaterialSurfaceMode::Transparent);
    EXPECT_FALSE(loaded->IsDecal());
    EXPECT_TRUE(loaded->IsBlendable());

    EXPECT_TRUE(NLS::Render::Resources::Loaders::MaterialLoader::Destroy(loaded));
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialLoaderUsesSourceSubAssetForLegacyDecalWhenNameMissing)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_legacy_source_decal_material_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(root);

    const auto materialPath = root / "LegacySourceDecal.nmat";
    {
        std::ofstream output(materialPath, std::ios::binary | std::ios::trunc);
        output <<
            "<root>\n"
            "  <shader>?</shader>\n"
            "  <sourceSubAsset>material:dirt_decal</sourceSubAsset>\n"
            "  <blendable>true</blendable>\n"
            "</root>\n";
    }

    auto* loaded = NLS::Render::Resources::Loaders::MaterialLoader::Create(materialPath.string());
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->GetSurfaceMode(), NLS::Render::Resources::MaterialSurfaceMode::Decal);
    EXPECT_TRUE(loaded->IsDecal());
    EXPECT_TRUE(loaded->IsBlendable());

    EXPECT_TRUE(NLS::Render::Resources::Loaders::MaterialLoader::Destroy(loaded));
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialLoaderExplicitSurfaceModeOverridesLegacyDecalInference)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_explicit_transparent_decal_name_material_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(root);

    const auto materialPath = root / "ExplicitTransparentDirtDecal.nmat";
    {
        std::ofstream output(materialPath, std::ios::binary | std::ios::trunc);
        output <<
            "<root>\n"
            "  <shader>?</shader>\n"
            "  <surfaceMode>Transparent</surfaceMode>\n"
            "  <name>dirt_decal</name>\n"
            "  <blendable>true</blendable>\n"
            "</root>\n";
    }

    auto* loaded = NLS::Render::Resources::Loaders::MaterialLoader::Create(materialPath.string());
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->GetSurfaceMode(), NLS::Render::Resources::MaterialSurfaceMode::Transparent);
    EXPECT_FALSE(loaded->IsDecal());
    EXPECT_TRUE(loaded->IsBlendable());

    EXPECT_TRUE(NLS::Render::Resources::Loaders::MaterialLoader::Destroy(loaded));
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialLoaderExplicitOpaqueSurfaceModeOverridesBlendableFlag)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_explicit_opaque_blendable_material_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(root);

    const auto materialPath = root / "ExplicitOpaqueBlendable.nmat";
    {
        std::ofstream output(materialPath, std::ios::binary | std::ios::trunc);
        output <<
            "<root>\n"
            "  <shader>?</shader>\n"
            "  <surfaceMode>Opaque</surfaceMode>\n"
            "  <name>dirt_decal</name>\n"
            "  <blendable>true</blendable>\n"
            "</root>\n";
    }

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

    const auto materialPath = root / "AlphaTest.nmat";
    {
        std::ofstream output(materialPath, std::ios::binary | std::ios::trunc);
        output <<
            "<root>\n"
            "  <shader>?</shader>\n"
            "  <surfaceMode>AlphaTest</surfaceMode>\n"
            "  <blendable>false</blendable>\n"
            "</root>\n";
    }

    auto* loaded = NLS::Render::Resources::Loaders::MaterialLoader::Create(materialPath.string());
    EXPECT_EQ(loaded, nullptr);

    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialLoaderRejectsInvalidExplicitSurfaceMode)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_invalid_surface_mode_material_" + NLS::Guid::New().ToString());
    const auto shaderArtifactPath = WriteStandardPbrShaderArtifact(root);
    static NLS::Core::ResourceManagement::ShaderManager shaderManager;
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::ShaderManager>(shaderManager);
    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(shaderArtifactPath.string());
    ASSERT_NE(shader, nullptr);
    shaderManager.RegisterResource(":Shaders/StandardPBR.hlsl", shader);

    const auto materialPath = root / "InvalidSurfaceMode.nmat";
    {
        std::ofstream output(materialPath, std::ios::binary | std::ios::trunc);
        output <<
            "<root>\n"
            "  <shader>:Shaders/StandardPBR.hlsl</shader>\n"
            "  <surfaceMode>Decaal</surfaceMode>\n"
            "  <blendable>true</blendable>\n"
            "</root>\n";
    }

    auto* loaded = NLS::Render::Resources::Loaders::MaterialLoader::Create(materialPath.string());
    EXPECT_EQ(loaded, nullptr);

    shaderManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialConversionCanReferenceShaderArtifactHandle)
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
    context.shaderResourcePath = "Library/Artifacts/shader-guid/shader.nshader";
    const auto material = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        scene.materials.front(),
        MaterialSourceModel::GltfPbrMetallicRoughness,
        context);

    EXPECT_NE(
        material.serializedPayload.find("<shader>Library/Artifacts/shader-guid/shader.nshader</shader>"),
        std::string::npos);
    EXPECT_EQ(material.serializedPayload.find("<shader>:Shaders/StandardPBR.hlsl</shader>"), std::string::npos);
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

    EXPECT_NE(material.serializedPayload.find("<backfaceCulling>false</backfaceCulling>"), std::string::npos);
    EXPECT_NE(material.serializedPayload.find("<frontfaceCulling>false</frontfaceCulling>"), std::string::npos);
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
    EXPECT_NE(payload.find("<backfaceCulling>false</backfaceCulling>"), std::string::npos);
}

TEST(AssetMaterialConversionTests, DefaultWhiteTextureRecoversRhiHandleAfterHeadlessMaterialLoad)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_default_white_texture_headless_recover_" + NLS::Guid::New().ToString());
    const auto shaderArtifactPath = root / "Library" / "Artifacts" / "shader-guid" / "shader.nshader";
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
    const auto shaderArtifactPath = WriteStandardPbrShaderArtifact(root);

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

    EXPECT_NE(shader->GetUniformInfo("u_Albedo"), nullptr);
    EXPECT_FALSE(shader->GetReflection().constantBuffers.empty());

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

    const auto converted = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        scene.materials.front(),
        MaterialSourceModel::GltfPbrMetallicRoughness);

    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_converted_material_" + NLS::Guid::New().ToString());
    const auto shaderArtifactPath = WriteStandardPbrShaderArtifact(root);
    static NLS::Core::ResourceManagement::ShaderManager shaderManager;
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::ShaderManager>(shaderManager);
    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(shaderArtifactPath.string());
    ASSERT_NE(shader, nullptr);
    shaderManager.RegisterResource(":Shaders/StandardPBR.hlsl", shader);

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    const auto materialPath = root / "Hero.nmat";
    {
        std::ofstream output(materialPath, std::ios::binary | std::ios::trunc);
        output << converted.serializedPayload;
    }

    auto* loaded = NLS::Render::Resources::Loaders::MaterialLoader::Create(materialPath.string());
    ASSERT_NE(loaded, nullptr);
    ASSERT_EQ(loaded->GetShader(), shader);

    const auto* albedoValue = loaded->GetParameterBlock().TryGet("u_Albedo");
    ASSERT_NE(albedoValue, nullptr);
    ASSERT_EQ(albedoValue->type(), typeid(NLS::Maths::Vector4));
    const auto& albedo = std::any_cast<const NLS::Maths::Vector4&>(*albedoValue);
    EXPECT_FLOAT_EQ(albedo.x, 0.2f);
    EXPECT_FLOAT_EQ(albedo.y, 0.4f);
    EXPECT_FLOAT_EQ(albedo.z, 0.6f);
    EXPECT_FLOAT_EQ(albedo.w, 0.8f);
    EXPECT_FLOAT_EQ(loaded->Get<float>("u_Metallic"), 0.3f);
    EXPECT_FLOAT_EQ(loaded->Get<float>("u_Roughness"), 0.7f);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::MaterialLoader::Destroy(loaded));
    shaderManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialLoaderResolvesShaderArtifactPayloadWithoutRuntimeSourceCompile)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_shader_artifact_material_" + NLS::Guid::New().ToString());
    const auto shaderArtifactPath = root / "Library" / "Artifacts" / "shader-guid" / "shader.nshader";
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
ARTIFACT_PATH=Library/Artifacts/shader-guid/shader.nshader
BYTECODE_HEX=01020304
STAGE_END
STAGE_BEGIN
STAGE=Pixel
TARGET=DXIL
ENTRY=PSMain
PROFILE=ps_6_0
STATUS=Succeeded
CACHE_KEY=test-pixel
ARTIFACT_PATH=Library/Artifacts/shader-guid/shader.nshader
BYTECODE_HEX=05060708
STAGE_END
CBUFFER_BEGIN
NAME=MaterialConstants
STAGE=Pixel
SPACE=2
BINDING=0
BYTE_SIZE=64
MEMBER_BEGIN
NAME=u_Albedo
TYPE=vec4
BYTE_OFFSET=0
BYTE_SIZE=16
ARRAY_SIZE=1
MEMBER_END
CBUFFER_END
PROPERTY_BEGIN
NAME=u_Albedo
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

    const auto materialPath = root / "Assets" / "Materials" / "ArtifactMaterial.nmat";
    WriteTextFile(
        materialPath,
        "<root>\n"
        "  <shader>Library/Artifacts/shader-guid/shader.nshader</shader>\n"
        "  <uniform name=\"u_Albedo\" type=\"vec4\" value=\"0.250000 0.500000 0.750000 1.000000\"/>\n"
        "</root>\n");

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
    ASSERT_NE(loaded, nullptr);
    ASSERT_NE(loaded->GetShader(), nullptr);
    const auto* albedoValue = loaded->GetParameterBlock().TryGet("u_Albedo");
    ASSERT_NE(albedoValue, nullptr);
    ASSERT_EQ(albedoValue->type(), typeid(NLS::Maths::Vector4));
    const auto& albedo = std::any_cast<const NLS::Maths::Vector4&>(*albedoValue);
    EXPECT_FLOAT_EQ(albedo.x, 0.25f);
    EXPECT_FLOAT_EQ(albedo.y, 0.5f);
    EXPECT_FLOAT_EQ(albedo.z, 0.75f);
    EXPECT_FLOAT_EQ(albedo.w, 1.0f);

    const auto* vertex = loaded->GetShader()->FindCompiledArtifact(
        NLS::Render::ShaderCompiler::ShaderStage::Vertex,
        NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL);
    ASSERT_NE(vertex, nullptr);
    EXPECT_EQ(vertex->entryPoint, "VSMain");
    EXPECT_EQ(vertex->output.bytecode, std::vector<uint8_t>({1u, 2u, 3u, 4u}));

    const auto* pixel = loaded->GetShader()->FindCompiledArtifact(
        NLS::Render::ShaderCompiler::ShaderStage::Pixel,
        NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL);
    ASSERT_NE(pixel, nullptr);
    EXPECT_EQ(pixel->entryPoint, "PSMain");
    EXPECT_EQ(pixel->output.bytecode, std::vector<uint8_t>({5u, 6u, 7u, 8u}));

    EXPECT_TRUE(NLS::Render::Resources::Loaders::MaterialLoader::Destroy(loaded));
    shaderManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::TextureManager>();
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, ConvertedMaterialPayloadLoadsDeclaredTextureSamplers)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_material_texture_load_" + NLS::Guid::New().ToString());
    const auto materialPath = root / "Materials" / "Hero.nmat";
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
    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(shaderArtifactPath.string());
    ASSERT_NE(shader, nullptr);
    shaderManager.RegisterResource(":Shaders/StandardPBR.hlsl", shader);

    const auto textureResourcePath = texturePath.lexically_normal().generic_string();
    const std::string payload =
        "<root>\n"
        "  <shader>:Shaders/StandardPBR.hlsl</shader>\n"
        "  <uniform name=\"u_AlbedoMap\" type=\"sampler2D\" value=\"" + textureResourcePath + "\"/>\n"
        "</root>\n";
    WriteBinaryFile(materialPath, std::vector<uint8_t>(payload.begin(), payload.end()));

    auto* skippedTextures = NLS::Render::Resources::Loaders::MaterialLoader::Create(
        materialPath.string(),
        {false});
    ASSERT_NE(skippedTextures, nullptr);
    const auto* skippedAlbedoMap = skippedTextures->GetParameterBlock().TryGet("u_AlbedoMap");
    ASSERT_NE(skippedAlbedoMap, nullptr);
    ASSERT_EQ(skippedAlbedoMap->type(), typeid(NLS::Render::Resources::Texture2D*));
    EXPECT_EQ(std::any_cast<NLS::Render::Resources::Texture2D*>(*skippedAlbedoMap), nullptr);
    EXPECT_EQ(skippedTextures->GetTextureResourcePath("u_AlbedoMap"), textureResourcePath);
    EXPECT_FALSE(textureManager.IsResourceRegistered(textureResourcePath));
    EXPECT_TRUE(NLS::Render::Resources::Loaders::MaterialLoader::Destroy(skippedTextures));

    auto* loadedTextures = NLS::Render::Resources::Loaders::MaterialLoader::Create(
        materialPath.string(),
        {true});
    ASSERT_NE(loadedTextures, nullptr);
    const auto* loadedAlbedoMap = loadedTextures->GetParameterBlock().TryGet("u_AlbedoMap");
    ASSERT_NE(loadedAlbedoMap, nullptr);
    ASSERT_EQ(loadedAlbedoMap->type(), typeid(NLS::Render::Resources::Texture2D*));
    EXPECT_NE(std::any_cast<NLS::Render::Resources::Texture2D*>(*loadedAlbedoMap), nullptr);
    EXPECT_EQ(loadedTextures->GetTextureResourcePath("u_AlbedoMap"), textureResourcePath);
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
    const auto materialPath = root / "Materials" / "Hero.nmat";

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
    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(shaderArtifactPath.string());
    ASSERT_NE(shader, nullptr);
    shaderManager.RegisterResource(":Shaders/StandardPBR.hlsl", shader);

    const std::string albedoPath = (root / "Textures" / "HeroBaseColor.ntex").lexically_normal().generic_string();
    const std::string normalPath = (root / "Textures" / "HeroNormal.ntex").lexically_normal().generic_string();
    const std::string metalRoughPath = (root / "Textures" / "HeroMetalRough.ntex").lexically_normal().generic_string();
    const std::string payload =
        "<root>\n"
        "  <shader>:Shaders/StandardPBR.hlsl</shader>\n"
        "  <uniform name=\"u_AlbedoMap\" type=\"sampler2D\" value=\"" + albedoPath + "\"/>\n"
        "  <uniform name=\"u_NormalMap\" type=\"sampler2D\" value=\"" + normalPath + "\"/>\n"
        "  <uniform name=\"u_MetallicMap\" type=\"sampler2D\" value=\"" + metalRoughPath + "\"/>\n"
        "  <uniform name=\"u_RoughnessMap\" type=\"sampler2D\" value=\"" + metalRoughPath + "\"/>\n"
        "</root>\n";
    WriteTextFile(materialPath, payload);

    auto* material = NLS::Render::Resources::Loaders::MaterialLoader::Create(
        materialPath.string(),
        {false, true});
    ASSERT_NE(material, nullptr);

    EXPECT_EQ(material->GetTextureResourcePath("u_AlbedoMap"), albedoPath);
    EXPECT_EQ(material->GetTextureResourcePath("u_NormalMap"), normalPath);
    EXPECT_EQ(material->GetTextureResourcePath("u_MetallicMap"), metalRoughPath);
    EXPECT_EQ(material->GetTextureResourcePath("u_RoughnessMap"), metalRoughPath);
    EXPECT_NE(material->GetTextureResourcePath("u_AlbedoMap"), material->GetTextureResourcePath("u_NormalMap"));
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

TEST(AssetMaterialConversionTests, MaterialLoaderAppliesTextureSlotSamplerMetadataToRuntimeSampler)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_material_texture_slot_sampler_" + NLS::Guid::New().ToString());
    const auto materialPath = root / "Materials" / "Hero.nmat";

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

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(
        "App/Assets/Engine/Shaders/StandardPBR.hlsl");
    ASSERT_NE(shader, nullptr);

    NLS::Render::Resources::ShaderReflection reflection;
    reflection.properties = {
        {
            "u_AlbedoMap",
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
        },
        {
            "u_LinearWrapSampler",
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
        }
    };
    const_cast<NLS::Render::Resources::ShaderReflection&>(shader->GetReflection()) = std::move(reflection);
    shaderManager.RegisterResource(":Shaders/StandardPBR.hlsl", shader);

    const std::string texturePath = (root / "Textures" / "HeroBaseColor.ntex")
        .lexically_normal()
        .generic_string();
    const std::string payload =
        "<root>\n"
        "  <shader>:Shaders/StandardPBR.hlsl</shader>\n"
        "  <uniform name=\"u_AlbedoMap\" type=\"sampler2D\" value=\"" + texturePath + "\"/>\n"
        "  <textureSlot name=\"BaseColor\" texture=\"image/0\" resourcePath=\"" + texturePath + "\""
        " colorSpace=\"SRgb\" wrapS=\"ClampToEdge\" wrapT=\"MirrorRepeat\" minFilter=\"Nearest\" magFilter=\"Nearest\"/>\n"
        "</root>\n";
    WriteTextFile(materialPath, payload);

    auto* material = NLS::Render::Resources::Loaders::MaterialLoader::Create(
        materialPath.string(),
        {false, true});
    ASSERT_NE(material, nullptr);

    const auto* sampler = material->GetBindingSet().GetSampler("u_LinearWrapSampler");
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

TEST(AssetMaterialConversionTests, MaterialReloadClearsPreviousTextureSlotSamplerMetadata)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_material_texture_slot_sampler_reload_" + NLS::Guid::New().ToString());
    const auto materialPath = root / "Materials" / "Hero.nmat";

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

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(
        "App/Assets/Engine/Shaders/StandardPBR.hlsl");
    ASSERT_NE(shader, nullptr);

    NLS::Render::Resources::ShaderReflection reflection;
    reflection.properties = {
        {
            "u_AlbedoMap",
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
        },
        {
            "u_LinearWrapSampler",
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
        }
    };
    const_cast<NLS::Render::Resources::ShaderReflection&>(shader->GetReflection()) = std::move(reflection);
    shaderManager.RegisterResource(":Shaders/StandardPBR.hlsl", shader);

    const std::string texturePath = (root / "Textures" / "HeroBaseColor.ntex")
        .lexically_normal()
        .generic_string();
    const std::string firstPayload =
        "<root>\n"
        "  <shader>:Shaders/StandardPBR.hlsl</shader>\n"
        "  <uniform name=\"u_AlbedoMap\" type=\"sampler2D\" value=\"" + texturePath + "\"/>\n"
        "  <textureSlot name=\"BaseColor\" texture=\"image/0\" resourcePath=\"" + texturePath + "\""
        " colorSpace=\"SRgb\" wrapS=\"ClampToEdge\" wrapT=\"ClampToEdge\" minFilter=\"Nearest\" magFilter=\"Nearest\"/>\n"
        "</root>\n";
    WriteTextFile(materialPath, firstPayload);

    auto* material = NLS::Render::Resources::Loaders::MaterialLoader::Create(
        materialPath.string(),
        {false, true});
    ASSERT_NE(material, nullptr);
    ASSERT_NE(material->GetBindingSet().GetSampler("u_LinearWrapSampler"), nullptr);
    EXPECT_EQ(
        material->GetBindingSet().GetSampler("u_LinearWrapSampler")->minFilter,
        NLS::Render::RHI::TextureFilter::Nearest);

    const std::string secondPayload =
        "<root>\n"
        "  <shader>:Shaders/StandardPBR.hlsl</shader>\n"
        "  <uniform name=\"u_AlbedoMap\" type=\"sampler2D\" value=\"" + texturePath + "\"/>\n"
        "</root>\n";
    WriteTextFile(materialPath, secondPayload);
    NLS::Render::Resources::Loaders::MaterialLoader::Reload(*material, materialPath.string(), {false, true});

    const auto* sampler = material->GetBindingSet().GetSampler("u_LinearWrapSampler");
    ASSERT_NE(sampler, nullptr);
    EXPECT_EQ(sampler->wrapU, NLS::Render::RHI::TextureWrap::Repeat);
    EXPECT_EQ(sampler->wrapV, NLS::Render::RHI::TextureWrap::Repeat);
    EXPECT_EQ(sampler->minFilter, NLS::Render::RHI::TextureFilter::Linear);
    EXPECT_EQ(sampler->magFilter, NLS::Render::RHI::TextureFilter::Linear);

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
    const auto materialPath = root / "Materials" / "Hero.nmat";
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

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(
        "App/Assets/Engine/Shaders/StandardPBR.hlsl");
    ASSERT_NE(shader, nullptr);
    shaderManager.RegisterResource(":Shaders/StandardPBR.hlsl", shader);

    const auto textureResourcePath = texturePath.lexically_normal().generic_string();
    const std::string payload =
        "<root>\n"
        "  <shader>:Shaders/StandardPBR.hlsl</shader>\n"
        "  <uniform name=\"u_AlbedoMap\" type=\"sampler2D\" value=\"" + textureResourcePath + "\"/>\n"
        "</root>\n";
    WriteBinaryFile(materialPath, std::vector<uint8_t>(payload.begin(), payload.end()));

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
    const auto shaderPath = projectAssets / "Shaders" / "Cold.hlsl";
    const auto materialPath = root / "Assets" / "Materials" / "Cold.nmat";

    WriteTextFile(
        shaderPath,
        "struct VSOutput { float4 position : SV_Position; };\n"
        "VSOutput VSMain(uint vertexId : SV_VertexID) {\n"
        "    VSOutput output;\n"
        "    output.position = float4(0.0f, 0.0f, 0.0f, 1.0f);\n"
        "    return output;\n"
        "}\n"
        "float4 PSMain(VSOutput input) : SV_Target0 { return float4(1.0f, 1.0f, 1.0f, 1.0f); }\n");
    WriteTextFile(
        materialPath,
        "<root>\n"
        "  <shader>Shaders/Cold.hlsl</shader>\n"
        "</root>\n");

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
    EXPECT_FALSE(shaderManager.IsResourceRegistered("Shaders/Cold.hlsl"));

    EXPECT_TRUE(NLS::Render::Resources::Loaders::MaterialLoader::Destroy(deferred));
    shaderManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialPrewarmDoesNotPoisonLaterShaderLoading)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_material_prewarm_reload_" + NLS::Guid::New().ToString());
    const auto projectAssets = root / "Assets";
    const auto shaderArtifactPath = root / "Library" / "Artifacts" / "shader-guid" / "shader.nshader";
    const auto materialPath = root / "Library" / "Artifacts" / "material-guid" / "materials" / "Hero.nmat";

    WriteNativeArtifactTextFile(
        shaderArtifactPath,
        NLS::Core::Assets::ArtifactType::Shader,
        "shader",
        1u,
        R"(NULLUS_IMPORTED_SHADER_ARTIFACT=1
SOURCE=Assets/Shaders/Hero.hlsl
SUB_ASSET=shader:Hero
TARGET_PLATFORM=editor
STAGE_BEGIN
STAGE=Vertex
TARGET=DXIL
ENTRY=VSMain
PROFILE=vs_6_0
STATUS=Succeeded
CACHE_KEY=test-vertex
ARTIFACT_PATH=Library/Artifacts/shader-guid/shader.nshader
BYTECODE_HEX=01020304
STAGE_END
STAGE_BEGIN
STAGE=Pixel
TARGET=DXIL
ENTRY=PSMain
PROFILE=ps_6_0
STATUS=Succeeded
CACHE_KEY=test-pixel
ARTIFACT_PATH=Library/Artifacts/shader-guid/shader.nshader
BYTECODE_HEX=05060708
STAGE_END
)");
    WriteNativeArtifactTextFile(
        materialPath,
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        1u,
        "<root>\n"
        "  <shader>Library/Artifacts/shader-guid/shader.nshader</shader>\n"
        "</root>\n");

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

    const auto resourcePath = std::filesystem::path("Library/Artifacts/material-guid/materials/Hero.nmat")
        .generic_string();
    EXPECT_EQ(materialManager.PrewarmArtifact(resourcePath), nullptr);
    EXPECT_FALSE(materialManager.IsResourceRegistered(resourcePath));
    EXPECT_FALSE(shaderManager.IsResourceRegistered("Library/Artifacts/shader-guid/shader.nshader"));

    auto* loaded = materialManager.GetResource(resourcePath, true);
    ASSERT_NE(loaded, nullptr);
    EXPECT_NE(loaded->GetShader(), nullptr);
    EXPECT_TRUE(shaderManager.IsResourceRegistered("Library/Artifacts/shader-guid/shader.nshader"));

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
    const auto materialPath = root / "Library" / "Artifacts" / "material-guid" / "materials" / "Hero.nmat";
    const auto texturePath = root / "Library" / "Artifacts" / "texture-guid" / "textures" / "BaseColor.ntex";

    WriteNativeArtifactTextFile(
        materialPath,
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        1u,
        "<root>\n"
        "  <shader>:Shaders/StandardPBR.hlsl</shader>\n"
        "  <uniform name=\"u_AlbedoMap\" type=\"sampler2D\" value=\"Library/Artifacts/texture-guid/textures/BaseColor.ntex\"/>\n"
        "</root>\n");
    const auto shaderArtifactPath = root / "Library" / "Artifacts" / "shader-guid" / "shader.nshader";
    WriteBinaryFile(shaderArtifactPath, NLS::Render::Assets::SerializeShaderArtifact(MakeAlbedoMapShaderArtifact()));

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

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(shaderArtifactPath.string());
    ASSERT_NE(shader, nullptr);
    shaderManager.RegisterResource(":Shaders/StandardPBR.hlsl", shader);

    const auto resourcePath = std::filesystem::path("Library/Artifacts/material-guid/materials/Hero.nmat")
        .generic_string();
    const auto textureResourcePath = std::filesystem::path("Library/Artifacts/texture-guid/textures/BaseColor.ntex")
        .generic_string();
    auto* loaded = materialManager.LoadArtifactWithoutTextures(resourcePath);

    ASSERT_NE(loaded, nullptr);
    EXPECT_TRUE(materialManager.IsResourceRegistered(resourcePath));
    EXPECT_NE(loaded->GetShader(), nullptr);
    EXPECT_EQ(loaded->GetTextureResourcePath("u_AlbedoMap"), textureResourcePath);
    EXPECT_FALSE(textureManager.IsResourceRegistered(textureResourcePath));
    const auto* albedoMap = loaded->GetParameterBlock().TryGet("u_AlbedoMap");
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

TEST(AssetMaterialConversionTests, TextureLoaderReadsImportedTextureArtifactPayload)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_imported_texture_artifact_load_" + NLS::Guid::New().ToString());
    const auto texturePath = root / "Library" / "Artifacts" / "Hero" / "textures" / "BaseColor.ntex";

    const std::string header =
        "NULLUS_IMPORTED_TEXTURE_ARTIFACT=1\n"
        "URI=Textures/BaseColor.png\n"
        "MIME_TYPE=image/png\n"
        "BYTE_LENGTH=67\n"
        "PAYLOAD_BEGIN\n";
    auto bytes = std::vector<uint8_t>(header.begin(), header.end());
    const auto png = TinyPng();
    bytes.insert(bytes.end(), png.begin(), png.end());
    WriteBinaryFile(texturePath, bytes);

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

TEST(AssetMaterialConversionTests, TextureLoaderReadsImportedRgb16TextureArtifactPayload)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_imported_rgb16_texture_artifact_load_" + NLS::Guid::New().ToString());
    const auto texturePath = root / "Library" / "Artifacts" / "Hero" / "textures" / "Render_Main_A.ntex";

    const std::string header =
        "NULLUS_IMPORTED_TEXTURE_ARTIFACT=1\n"
        "URI=Render_Main_A.png\n"
        "MIME_TYPE=image/png\n"
        "BYTE_LENGTH=72\n"
        "PAYLOAD_BEGIN\n";
    auto bytes = std::vector<uint8_t>(header.begin(), header.end());
    const auto png = TinyRgb16Png();
    bytes.insert(bytes.end(), png.begin(), png.end());
    WriteBinaryFile(texturePath, bytes);

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
    const auto texturePath = root / "Library" / "Artifacts" / "Hero" / "textures" / "NativeBaseColor.ntex";

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
    WriteBinaryFile(texturePath, bytes);

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
    const auto texturePath = root / "Library" / "Artifacts" / "Hero" / "textures" / "NativeBaseColor.ntex";

    const auto artifact = MakeDescriptorBackedTextureArtifact(
        NLS::Render::RHI::TextureFormat::RGBA8,
        NLS::Render::Assets::TextureArtifactColorSpace::Srgb,
        2u,
        2u,
        2u);
    WriteBinaryFile(texturePath, NLS::Render::Assets::SerializeTextureArtifact(artifact));

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
    const auto texturePath = root / "Library" / "Artifacts" / "Hero" / "textures" / "CompressedBaseColor.ntex";

    const auto artifact = MakeDescriptorBackedTextureArtifact(
        NLS::Render::RHI::TextureFormat::BC7,
        NLS::Render::Assets::TextureArtifactColorSpace::Srgb,
        8u,
        8u,
        2u);
    WriteBinaryFile(texturePath, NLS::Render::Assets::SerializeTextureArtifact(artifact));

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
    const auto texturePath = root / "Library" / "Artifacts" / "Hero" / "textures" / "UnsupportedCompressed.ntex";

    const auto artifact = MakeDescriptorBackedTextureArtifact(
        NLS::Render::RHI::TextureFormat::BC1,
        NLS::Render::Assets::TextureArtifactColorSpace::Srgb,
        4u,
        4u,
        1u);
    WriteBinaryFile(texturePath, NLS::Render::Assets::SerializeTextureArtifact(artifact));

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
    EXPECT_NE(convertedFbx.serializedPayload.find("u_MetallicMap"), std::string::npos);
    EXPECT_NE(convertedFbx.serializedPayload.find("Metallic.png"), std::string::npos);
    EXPECT_NE(convertedFbx.serializedPayload.find("u_RoughnessMap"), std::string::npos);
    EXPECT_NE(convertedFbx.serializedPayload.find("Roughness.png"), std::string::npos);
    EXPECT_NE(convertedFbx.serializedPayload.find("u_OpacityMap"), std::string::npos);
    EXPECT_NE(convertedFbx.serializedPayload.find("Opacity.png"), std::string::npos);
    EXPECT_NE(convertedFbx.serializedPayload.find("u_EmissiveMap"), std::string::npos);
    EXPECT_NE(convertedFbx.serializedPayload.find("Emissive.png"), std::string::npos);
    EXPECT_NE(convertedFbx.serializedPayload.find("u_SpecularMap"), std::string::npos);
    EXPECT_NE(convertedFbx.serializedPayload.find("Specular.png"), std::string::npos);
    EXPECT_NE(
        convertedFbx.serializedPayload.find("<uniform name=\"u_Emissive\" type=\"vec4\" value=\"0.100000 0.200000 0.300000 1.000000\"/>"),
        std::string::npos);
    EXPECT_NE(
        convertedFbx.serializedPayload.find("<uniform name=\"u_Specular\" type=\"vec4\" value=\"0.700000 0.800000 0.900000 1.000000\"/>"),
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
        convertedShininessOnly.serializedPayload.find("<uniform name=\"u_Roughness\" type=\"float\" value=\"0.174078\"/>"),
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
        convertedRoughnessTexture.serializedPayload.find("<uniform name=\"u_Roughness\" type=\"float\" value=\"1.000000\"/>"),
        std::string::npos);
    EXPECT_NE(convertedRoughnessTexture.serializedPayload.find("u_RoughnessMap"), std::string::npos);
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
        convertedTextured.serializedPayload.find("<uniform name=\"u_Roughness\" type=\"float\" value=\"1.000000\"/>"),
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
            convertedInvalidScalar.serializedPayload.find("<uniform name=\"u_Roughness\" type=\"float\" value=\"1.000000\"/>"),
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
        convertedTextured.serializedPayload.find("<uniform name=\"u_Albedo\" type=\"vec4\" value=\"1.000000 1.000000 1.000000 1.000000\"/>"),
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
        convertedTextureless.serializedPayload.find("<uniform name=\"u_Albedo\" type=\"vec4\" value=\"0.250000 0.500000 0.750000 1.000000\"/>"),
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
        convertedTinted.serializedPayload.find("<uniform name=\"u_Albedo\" type=\"vec4\" value=\"0.800000 0.700000 0.600000 1.000000\"/>"),
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
        convertedTinted.serializedPayload.find("<uniform name=\"u_Albedo\" type=\"vec4\" value=\"1.000000 1.000000 1.000000 1.000000\"/>"),
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
        convertedTinted.serializedPayload.find("<uniform name=\"u_Albedo\" type=\"vec4\" value=\"0.500000 0.500000 0.500000 1.000000\"/>"),
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
    EXPECT_NE(convertedDecal.serializedPayload.find("<alphaMode>Blend</alphaMode>"), std::string::npos);
    EXPECT_NE(convertedDecal.serializedPayload.find("<surfaceMode>Decal</surfaceMode>"), std::string::npos);
    EXPECT_NE(convertedDecal.serializedPayload.find("<depthWriting>false</depthWriting>"), std::string::npos);
    EXPECT_NE(convertedDecal.serializedPayload.find("u_OpacityMap"), std::string::npos);

    NLS::Render::Assets::ImportedSceneNamedRecord transparent;
    transparent.sourceKey = "fbx/material/window_mask";
    transparent.name = "window_mask";
    transparent.materialChannels.push_back({"opacity", "fbx/texture/opacity", {}, false, 0.0});

    const auto convertedTransparent = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        transparent,
        MaterialSourceModel::FbxParserMaterial);

    EXPECT_EQ(convertedTransparent.alphaMode, MaterialAlphaMode::Blend);
    EXPECT_NE(convertedTransparent.serializedPayload.find("<surfaceMode>Transparent</surfaceMode>"), std::string::npos);
    EXPECT_EQ(convertedTransparent.serializedPayload.find("<surfaceMode>Decal</surfaceMode>"), std::string::npos);
}

TEST(AssetMaterialConversionTests, FbxBumpOnlyChannelDoesNotEnableNormalMapping)
{
    NLS::Render::Assets::ImportedScene scene;
    scene.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("e2010303-0303-4303-8303-030303030303"));
    scene.textures.push_back({"fbx/texture/bump", "HeroBump", "HeroBump.png", "image/png"});
    scene.textures.push_back({"fbx/texture/normal", "HeroNormal", "HeroNormal.png", "image/png"});

    NLS::Render::Assets::ImportedSceneNamedRecord bumpOnly;
    bumpOnly.sourceKey = "fbx/material/BumpOnly";
    bumpOnly.name = "BumpOnly";
    bumpOnly.materialChannels.push_back({"bump", "fbx/texture/bump", {}, false, 0.0});

    const auto convertedBumpOnly = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        bumpOnly,
        MaterialSourceModel::FbxParserMaterial);

    EXPECT_EQ(FindSlot(convertedBumpOnly, "Normal"), nullptr)
        << "FBX bump/height textures are not tangent-space normal maps unless converted during import.";
    EXPECT_TRUE(HasDiagnosticCode(convertedBumpOnly, "material-ignored-fbx-bump-height-map"));
    EXPECT_EQ(convertedBumpOnly.serializedPayload.find("u_NormalMap"), std::string::npos);
    EXPECT_NE(
        convertedBumpOnly.serializedPayload.find("<uniform name=\"u_EnableNormalMapping\" type=\"float\" value=\"0.000000\"/>"),
        std::string::npos);

    NLS::Render::Assets::ImportedSceneNamedRecord normal;
    normal.sourceKey = "fbx/material/NormalMapped";
    normal.name = "NormalMapped";
    normal.materialChannels.push_back({"normal", "fbx/texture/normal", {}, false, 0.0});

    const auto convertedNormal = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        normal,
        MaterialSourceModel::FbxParserMaterial);

    const auto* normalSlot = FindSlot(convertedNormal, "Normal");
    ASSERT_NE(normalSlot, nullptr);
    EXPECT_EQ(normalSlot->textureKey, "fbx/texture/normal");
    EXPECT_NE(convertedNormal.serializedPayload.find("u_NormalMap"), std::string::npos);
    EXPECT_NE(convertedNormal.serializedPayload.find("HeroNormal.png"), std::string::npos);
    EXPECT_NE(
        convertedNormal.serializedPayload.find("<uniform name=\"u_EnableNormalMapping\" type=\"float\" value=\"1.000000\"/>"),
        std::string::npos);

    NLS::Render::Assets::ImportedSceneNamedRecord normalAndBump;
    normalAndBump.sourceKey = "fbx/material/NormalAndBump";
    normalAndBump.name = "NormalAndBump";
    normalAndBump.materialChannels.push_back({"normal", "fbx/texture/normal", {}, false, 0.0});
    normalAndBump.materialChannels.push_back({"bump", "fbx/texture/bump", {}, false, 0.0});

    const auto convertedNormalAndBump = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        normalAndBump,
        MaterialSourceModel::FbxParserMaterial);

    ASSERT_EQ(CountSlots(convertedNormalAndBump, "Normal"), 1u);
    const auto* authoritativeNormal = FindSlot(convertedNormalAndBump, "Normal");
    ASSERT_NE(authoritativeNormal, nullptr);
    EXPECT_EQ(authoritativeNormal->textureKey, "fbx/texture/normal");
    EXPECT_NE(convertedNormalAndBump.serializedPayload.find("HeroNormal.png"), std::string::npos);
    EXPECT_EQ(convertedNormalAndBump.serializedPayload.find("HeroBump.png"), std::string::npos);
}

TEST(AssetMaterialConversionTests, ObjBumpChannelStillUsesNormalMapCompatibility)
{
    NLS::Render::Assets::ImportedScene scene;
    scene.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("e2010404-0404-4404-8404-040404040404"));
    scene.textures.push_back({"obj/texture/bump", "ObjBump", "ObjBump.png", "image/png"});

    NLS::Render::Assets::ImportedSceneNamedRecord obj;
    obj.sourceKey = "mtl/material/LegacyBump";
    obj.name = "LegacyBump";
    obj.materialChannels.push_back({"bump", "obj/texture/bump", {}, false, 0.0});

    const auto converted = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        obj,
        MaterialSourceModel::ObjMtl);

    const auto* normalSlot = FindSlot(converted, "Normal");
    ASSERT_NE(normalSlot, nullptr);
    EXPECT_EQ(normalSlot->textureKey, "obj/texture/bump");
    EXPECT_EQ(CountSlots(converted, "Normal"), 1u);
    EXPECT_NE(converted.serializedPayload.find("u_NormalMap"), std::string::npos);
    EXPECT_NE(converted.serializedPayload.find("ObjBump.png"), std::string::npos);
    EXPECT_NE(
        converted.serializedPayload.find("<uniform name=\"u_EnableNormalMapping\" type=\"float\" value=\"1.000000\"/>"),
        std::string::npos);
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
    const auto standard = read(root / "App/Assets/Engine/Shaders/Standard.hlsl");

    ASSERT_FALSE(standard.empty());
    ASSERT_FALSE(standardPbr.empty());
    ASSERT_FALSE(deferredGBuffer.empty());

    const auto expectBc5CompatibleNormalDecode = [](const std::string& shader)
    {
        EXPECT_NE(shader.find("ComputeNormal"), std::string::npos);
        EXPECT_NE(shader.find("DecodeNormalMapSample"), std::string::npos);
        EXPECT_NE(shader.find("u_NormalMap.Sample"), std::string::npos);
        EXPECT_NE(shader.find("sqrt(saturate(1.0f - dot(xy, xy)))"), std::string::npos);
        EXPECT_NE(shader.find("u_EnableNormalMapping > 0.5f"), std::string::npos);
    };
    expectBc5CompatibleNormalDecode(standard);
    expectBc5CompatibleNormalDecode(standardPbr);
    expectBc5CompatibleNormalDecode(deferredGBuffer);
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
    const auto lightGridCommon = read(root / "App/Assets/Engine/Shaders/LightGridCommon.hlsli");
    const auto standard = read(root / "App/Assets/Engine/Shaders/Standard.hlsl");

    ASSERT_FALSE(commonTypes.empty());
    ASSERT_FALSE(lightGridCommon.empty());
    ASSERT_FALSE(standard.empty());
    ASSERT_FALSE(standardPbr.empty());
    ASSERT_FALSE(deferredGBuffer.empty());
    ASSERT_FALSE(deferredLighting.empty());

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

    const auto expectSafeNormalMapping = [](const std::string& shader)
    {
        EXPECT_NE(shader.find("NLSTransformNormalDirection(model3x3, input.Normal)"), std::string::npos);
        EXPECT_EQ(shader.find("mul((float3x3)model, input.Normal)"), std::string::npos);
        EXPECT_NE(shader.find("NLSBuildSafeTangentFrame("), std::string::npos);
        EXPECT_NE(shader.find("NLSSafeNormalize(float3(xy, lerp(reconstructedZ, rgbZ, useRgbZ))"), std::string::npos);
        EXPECT_NE(shader.find("NLSApplyTangentNormal(tangentNormal, tangentFrame)"), std::string::npos);
    };

    expectSafeNormalMapping(standard);
    expectSafeNormalMapping(standardPbr);
    expectSafeNormalMapping(deferredGBuffer);
    EXPECT_NE(deferredGBuffer.find("const float surfaceAlpha = u_Albedo.a * albedoSample.a * opacity"), std::string::npos);
    EXPECT_NE(deferredGBuffer.find("output.Normal = float4(normalWS * 0.5f + 0.5f, surfaceAlpha)"), std::string::npos);
    EXPECT_NE(deferredGBuffer.find("output.Material = float4(metallic, roughness, ao, surfaceAlpha)"), std::string::npos);
    EXPECT_NE(deferredLighting.find("NLSDeferredSafeNormalize(encodedNormal * 2.0f - 1.0f"), std::string::npos);
    EXPECT_EQ(deferredLighting.find("normalize(encodedNormal * 2.0f - 1.0f)"), std::string::npos);
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
    EXPECT_NE(result.manifest.subAssets.front().artifactPath.find(".nmat"), std::string::npos);
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
    EXPECT_EQ(payloads[0].relativePath.generic_string(), "materials/material%3Abody.nmat");
    EXPECT_EQ(payloads[1].relativePath.generic_string(), "materials/material%2Fbody.nmat");
}
