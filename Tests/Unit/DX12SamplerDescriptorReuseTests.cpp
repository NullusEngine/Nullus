#include <atomic>
#include <array>
#include <barrier>
#include <thread>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

#include "Rendering/RHI/Backends/DX12/DX12Access.h"
#include "Rendering/RHI/Backends/DX12/DX12Device.h"
#include "Rendering/RHI/Backends/DX12/DX12Descriptor.h"
#include "Rendering/RHI/Backends/DX12/DX12ExplicitDeviceFactory.h"
#include "Rendering/RHI/Core/RHIBinding.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/RHI/Core/RHIResource.h"

static_assert(!std::is_copy_constructible_v<NLS::Render::Backend::DX12SamplerDescriptorTableCache::Allocation>);
static_assert(!std::is_copy_assignable_v<NLS::Render::Backend::DX12SamplerDescriptorTableCache::Allocation>);
static_assert(!std::is_move_constructible_v<NLS::Render::Backend::DX12SamplerDescriptorTableCache::Allocation>);
static_assert(!std::is_move_assignable_v<NLS::Render::Backend::DX12SamplerDescriptorTableCache::Allocation>);

namespace
{
    constexpr uint32_t kDX12SamplerHeapCapacity = 2048u;

    std::shared_ptr<NLS::Render::RHI::RHIDevice> CreateTestDeviceOrSkip()
    {
        const auto resources = NLS::Render::Backend::CreateDX12DeviceResources(false);
        if (!resources.IsValid())
            return nullptr;

        return NLS::Render::Backend::CreateNativeDX12ExplicitDevice(
            resources.device.Get(),
            resources.graphicsQueue.Get(),
            resources.computeQueue.Get(),
            resources.factory.Get(),
            resources.adapter.Get(),
            resources.capabilities,
            resources.vendor,
            resources.hardware);
    }

    std::shared_ptr<NLS::Render::RHI::RHIBindingLayout> CreateSamplerBindingLayout(
        const std::shared_ptr<NLS::Render::RHI::RHIDevice>& device,
        const uint32_t binding = 0u)
    {
        NLS::Render::RHI::RHIBindingLayoutDesc layoutDesc;
        layoutDesc.debugName = "DX12SamplerDescriptorReuseTestsLayout";
        layoutDesc.entries.push_back({
            "u_TestSampler",
            NLS::Render::RHI::BindingType::Sampler,
            0u,
            binding,
            1u,
            NLS::Render::RHI::ShaderStageMask::Fragment,
            0u,
            0u
        });
        return device->CreateBindingLayout(layoutDesc);
    }

    std::shared_ptr<NLS::Render::RHI::RHIBindingLayout> CreateMultiSamplerBindingLayout(
        const std::shared_ptr<NLS::Render::RHI::RHIDevice>& device)
    {
        NLS::Render::RHI::RHIBindingLayoutDesc layoutDesc;
        layoutDesc.debugName = "DX12SamplerDescriptorReuseTestsMultiRangeLayout";
        layoutDesc.entries.push_back({
            "u_FirstSampler",
            NLS::Render::RHI::BindingType::Sampler,
            0u,
            0u,
            1u,
            NLS::Render::RHI::ShaderStageMask::Fragment,
            0u,
            0u
        });
        layoutDesc.entries.push_back({
            "u_SecondSampler",
            NLS::Render::RHI::BindingType::Sampler,
            0u,
            3u,
            1u,
            NLS::Render::RHI::ShaderStageMask::Fragment,
            0u,
            0u
        });
        return device->CreateBindingLayout(layoutDesc);
    }

    std::shared_ptr<NLS::Render::RHI::RHIBindingLayout> CreateMixedResourceSamplerBindingLayout(
        const std::shared_ptr<NLS::Render::RHI::RHIDevice>& device)
    {
        NLS::Render::RHI::RHIBindingLayoutDesc layoutDesc;
        layoutDesc.debugName = "DX12SamplerDescriptorReuseTestsMixedLayout";
        layoutDesc.entries.push_back({
            "u_TestConstants",
            NLS::Render::RHI::BindingType::UniformBuffer,
            0u,
            0u,
            1u,
            NLS::Render::RHI::ShaderStageMask::Fragment,
            0u,
            0u
        });
        layoutDesc.entries.push_back({
            "u_TestSampler",
            NLS::Render::RHI::BindingType::Sampler,
            0u,
            1u,
            1u,
            NLS::Render::RHI::ShaderStageMask::Fragment,
            0u,
            0u
        });
        return device->CreateBindingLayout(layoutDesc);
    }

    NLS::Render::RHI::RHIBindingSetDesc CreateSamplerBindingSetDesc(
        const std::shared_ptr<NLS::Render::RHI::RHIBindingLayout>& layout,
        std::shared_ptr<NLS::Render::RHI::RHISampler> sampler,
        const uint32_t binding = 0u)
    {
        NLS::Render::RHI::RHIBindingSetDesc desc;
        desc.layout = layout;
        desc.debugName = "DX12SamplerDescriptorReuseTestsBindingSet";
        desc.entries.push_back({
            binding,
            NLS::Render::RHI::BindingType::Sampler,
            nullptr,
            0u,
            0u,
            0u,
            nullptr,
            std::move(sampler)
        });
        return desc;
    }

    NLS::Render::RHI::RHIBindingSetDesc CreateMultiSamplerBindingSetDesc(
        const std::shared_ptr<NLS::Render::RHI::RHIBindingLayout>& layout,
        std::shared_ptr<NLS::Render::RHI::RHISampler> firstSampler,
        std::shared_ptr<NLS::Render::RHI::RHISampler> secondSampler)
    {
        NLS::Render::RHI::RHIBindingSetDesc desc;
        desc.layout = layout;
        desc.debugName = "DX12SamplerDescriptorReuseTestsMultiRangeBindingSet";
        desc.entries.push_back({
            0u,
            NLS::Render::RHI::BindingType::Sampler,
            nullptr,
            0u,
            0u,
            0u,
            nullptr,
            std::move(firstSampler)
        });
        desc.entries.push_back({
            3u,
            NLS::Render::RHI::BindingType::Sampler,
            nullptr,
            0u,
            0u,
            0u,
            nullptr,
            std::move(secondSampler)
        });
        return desc;
    }

    NLS::Render::RHI::RHIBindingSetDesc CreateMixedResourceSamplerBindingSetDesc(
        const std::shared_ptr<NLS::Render::RHI::RHIBindingLayout>& layout,
        std::shared_ptr<NLS::Render::RHI::RHIBuffer> buffer,
        std::shared_ptr<NLS::Render::RHI::RHISampler> sampler)
    {
        NLS::Render::RHI::RHIBindingSetDesc desc;
        desc.layout = layout;
        desc.debugName = "DX12SamplerDescriptorReuseTestsMixedBindingSet";
        desc.entries.push_back({
            0u,
            NLS::Render::RHI::BindingType::UniformBuffer,
            std::move(buffer),
            0u,
            256u,
            0u,
            nullptr,
            nullptr
        });
        desc.entries.push_back({
            1u,
            NLS::Render::RHI::BindingType::Sampler,
            nullptr,
            0u,
            0u,
            0u,
            nullptr,
            std::move(sampler)
        });
        return desc;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE GetGpuHandle(
        const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& bindingSet,
        const NLS::Render::RHI::DX12::DX12DescriptorHeapKind heapKind)
    {
        if (bindingSet == nullptr)
            return {};

        const auto nativeHandle = bindingSet->GetNativeBindingSetHandle();
        auto* access = nativeHandle.backend == NLS::Render::RHI::BackendType::DX12
            ? static_cast<NLS::Render::Backend::IDX12BindingSetAccess*>(nativeHandle.handle)
            : nullptr;
        return access != nullptr
            ? access->GetGPUHandle(0u, heapKind)
            : D3D12_GPU_DESCRIPTOR_HANDLE {};
    }

    D3D12_GPU_DESCRIPTOR_HANDLE GetSamplerGpuHandle(
        const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& bindingSet)
    {
        return GetGpuHandle(bindingSet, NLS::Render::RHI::DX12::DX12DescriptorHeapKind::Sampler);
    }

    D3D12_GPU_DESCRIPTOR_HANDLE GetResourceGpuHandle(
        const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& bindingSet)
    {
        return GetGpuHandle(bindingSet, NLS::Render::RHI::DX12::DX12DescriptorHeapKind::Resource);
    }

    std::shared_ptr<NLS::Render::RHI::RHIBuffer> CreateUniformBuffer(
        const std::shared_ptr<NLS::Render::RHI::RHIDevice>& device,
        const uint32_t seed)
    {
        std::array<uint32_t, 64u> data{};
        data[0] = seed;

        NLS::Render::RHI::RHIBufferDesc desc;
        desc.size = sizeof(data);
        desc.usage = NLS::Render::RHI::BufferUsageFlags::Uniform;
        desc.memoryUsage = NLS::Render::RHI::MemoryUsage::CPUToGPU;
        desc.debugName = "DX12SamplerDescriptorReuseTestsUniformBuffer";

        NLS::Render::RHI::RHIBufferUploadDesc uploadDesc;
        uploadDesc.data = data.data();
        uploadDesc.dataSize = sizeof(data);
        uploadDesc.debugName = "DX12SamplerDescriptorReuseTestsUniformUpload";
        return device->CreateBuffer(desc, uploadDesc);
    }

    NLS::Render::RHI::SamplerDesc MakeUniqueSamplerDesc(const uint32_t index)
    {
        NLS::Render::RHI::SamplerDesc desc;
        desc.minLod = static_cast<float>(index);
        desc.mipLodBias = static_cast<float>(index % 17u) * 0.125f;
        desc.maxAnisotropy = 1u + (index % 16u);
        return desc;
    }

    NLS::Render::RHI::SamplerDesc MakeNativeEquivalentDefaultSamplerDesc()
    {
        NLS::Render::RHI::SamplerDesc desc;
        desc.maxAnisotropy = 0u;
        desc.mipLodBias = -0.0f;
        return desc;
    }
}

TEST(DX12SamplerDescriptorReuseTests, IdenticalSamplerTablesCreateBeyondSingleHeapCapacity)
{
    auto device = CreateTestDeviceOrSkip();
    if (device == nullptr)
        GTEST_SKIP() << "DX12 device unavailable on this test machine";

    auto layout = CreateSamplerBindingLayout(device);
    ASSERT_NE(layout, nullptr);
    auto sampler = device->CreateSampler(NLS::Render::RHI::SamplerDesc{}, "SharedSampler");
    ASSERT_NE(sampler, nullptr);

    std::vector<std::shared_ptr<NLS::Render::RHI::RHIBindingSet>> bindingSets;
    bindingSets.reserve(kDX12SamplerHeapCapacity + 1u);
    for (uint32_t index = 0u; index < kDX12SamplerHeapCapacity + 1u; ++index)
    {
        auto bindingSet = device->CreateBindingSet(CreateSamplerBindingSetDesc(layout, sampler));
        ASSERT_NE(bindingSet, nullptr) << "binding set index " << index << " should reuse identical sampler tables";
        bindingSets.push_back(std::move(bindingSet));
    }

    const auto firstHandle = GetSamplerGpuHandle(bindingSets.front());
    EXPECT_NE(firstHandle.ptr, 0u);
    for (const auto& bindingSet : bindingSets)
    {
        const auto handle = GetSamplerGpuHandle(bindingSet);
        EXPECT_EQ(handle.ptr, firstHandle.ptr);
    }
}

TEST(DX12SamplerDescriptorReuseTests, DifferentSamplerTablesUseDifferentDescriptorRanges)
{
    auto device = CreateTestDeviceOrSkip();
    if (device == nullptr)
        GTEST_SKIP() << "DX12 device unavailable on this test machine";

    auto layout = CreateSamplerBindingLayout(device);
    ASSERT_NE(layout, nullptr);

    NLS::Render::RHI::SamplerDesc nearestDesc;
    nearestDesc.minFilter = NLS::Render::RHI::TextureFilter::Nearest;
    nearestDesc.magFilter = NLS::Render::RHI::TextureFilter::Nearest;

    auto linearSampler = device->CreateSampler(NLS::Render::RHI::SamplerDesc{}, "LinearSampler");
    auto nearestSampler = device->CreateSampler(nearestDesc, "NearestSampler");
    ASSERT_NE(linearSampler, nullptr);
    ASSERT_NE(nearestSampler, nullptr);

    auto linearSet = device->CreateBindingSet(CreateSamplerBindingSetDesc(layout, linearSampler));
    ASSERT_NE(linearSet, nullptr);
    auto nearestSet = device->CreateBindingSet(CreateSamplerBindingSetDesc(layout, nearestSampler));
    ASSERT_NE(nearestSet, nullptr);

    const auto linearHandle = GetSamplerGpuHandle(linearSet);
    const auto nearestHandle = GetSamplerGpuHandle(nearestSet);
    EXPECT_NE(linearHandle.ptr, 0u);
    EXPECT_NE(nearestHandle.ptr, 0u);
    EXPECT_NE(linearHandle.ptr, nearestHandle.ptr);
}

TEST(DX12SamplerDescriptorReuseTests, NativeEquivalentSamplerTablesReuseDescriptorRange)
{
    auto device = CreateTestDeviceOrSkip();
    if (device == nullptr)
        GTEST_SKIP() << "DX12 device unavailable on this test machine";

    auto layout = CreateSamplerBindingLayout(device);
    ASSERT_NE(layout, nullptr);

    auto defaultSampler = device->CreateSampler(NLS::Render::RHI::SamplerDesc{}, "DefaultSampler");
    auto nativeEquivalentSampler = device->CreateSampler(
        MakeNativeEquivalentDefaultSamplerDesc(),
        "NativeEquivalentSampler");
    ASSERT_NE(defaultSampler, nullptr);
    ASSERT_NE(nativeEquivalentSampler, nullptr);

    auto defaultSet = device->CreateBindingSet(CreateSamplerBindingSetDesc(layout, defaultSampler));
    auto equivalentSet = device->CreateBindingSet(CreateSamplerBindingSetDesc(layout, nativeEquivalentSampler));
    ASSERT_NE(defaultSet, nullptr);
    ASSERT_NE(equivalentSet, nullptr);

    const auto defaultHandle = GetSamplerGpuHandle(defaultSet);
    const auto equivalentHandle = GetSamplerGpuHandle(equivalentSet);
    EXPECT_NE(defaultHandle.ptr, 0u);
    EXPECT_EQ(equivalentHandle.ptr, defaultHandle.ptr);
}

TEST(DX12SamplerDescriptorReuseTests, DifferentSamplerTableLayoutCoordinatesDoNotAlias)
{
    auto device = CreateTestDeviceOrSkip();
    if (device == nullptr)
        GTEST_SKIP() << "DX12 device unavailable on this test machine";

    auto firstLayout = CreateSamplerBindingLayout(device, 0u);
    auto secondLayout = CreateSamplerBindingLayout(device, 1u);
    ASSERT_NE(firstLayout, nullptr);
    ASSERT_NE(secondLayout, nullptr);
    auto sampler = device->CreateSampler(NLS::Render::RHI::SamplerDesc{}, "LayoutSampler");
    ASSERT_NE(sampler, nullptr);

    auto first = device->CreateBindingSet(CreateSamplerBindingSetDesc(firstLayout, sampler, 0u));
    ASSERT_NE(first, nullptr);
    auto second = device->CreateBindingSet(CreateSamplerBindingSetDesc(secondLayout, sampler, 1u));
    ASSERT_NE(second, nullptr);

    const auto firstHandle = GetSamplerGpuHandle(first);
    const auto secondHandle = GetSamplerGpuHandle(second);
    EXPECT_NE(firstHandle.ptr, 0u);
    EXPECT_NE(secondHandle.ptr, 0u);
    EXPECT_NE(firstHandle.ptr, secondHandle.ptr);
}

TEST(DX12SamplerDescriptorReuseTests, MultiRangeSamplerTablesReuseBeyondSingleHeapCapacity)
{
    auto device = CreateTestDeviceOrSkip();
    if (device == nullptr)
        GTEST_SKIP() << "DX12 device unavailable on this test machine";

    auto layout = CreateMultiSamplerBindingLayout(device);
    ASSERT_NE(layout, nullptr);

    NLS::Render::RHI::SamplerDesc nearestDesc;
    nearestDesc.minFilter = NLS::Render::RHI::TextureFilter::Nearest;
    nearestDesc.magFilter = NLS::Render::RHI::TextureFilter::Nearest;
    auto firstSampler = device->CreateSampler(NLS::Render::RHI::SamplerDesc{}, "MultiFirstSampler");
    auto secondSampler = device->CreateSampler(nearestDesc, "MultiSecondSampler");
    ASSERT_NE(firstSampler, nullptr);
    ASSERT_NE(secondSampler, nullptr);

    std::vector<std::shared_ptr<NLS::Render::RHI::RHIBindingSet>> bindingSets;
    bindingSets.reserve(kDX12SamplerHeapCapacity + 1u);
    for (uint32_t index = 0u; index < kDX12SamplerHeapCapacity + 1u; ++index)
    {
        auto bindingSet = device->CreateBindingSet(
            CreateMultiSamplerBindingSetDesc(layout, firstSampler, secondSampler));
        ASSERT_NE(bindingSet, nullptr) << "multi-range binding set index " << index;
        bindingSets.push_back(std::move(bindingSet));
    }

    const auto firstHandle = GetSamplerGpuHandle(bindingSets.front());
    EXPECT_NE(firstHandle.ptr, 0u);
    for (const auto& bindingSet : bindingSets)
        EXPECT_EQ(GetSamplerGpuHandle(bindingSet).ptr, firstHandle.ptr);
}

TEST(DX12SamplerDescriptorReuseTests, MixedResourceAndSamplerTablesKeepIndependentHandles)
{
    auto device = CreateTestDeviceOrSkip();
    if (device == nullptr)
        GTEST_SKIP() << "DX12 device unavailable on this test machine";

    auto layout = CreateMixedResourceSamplerBindingLayout(device);
    ASSERT_NE(layout, nullptr);
    auto sampler = device->CreateSampler(NLS::Render::RHI::SamplerDesc{}, "MixedSharedSampler");
    ASSERT_NE(sampler, nullptr);
    auto firstBuffer = CreateUniformBuffer(device, 1u);
    auto secondBuffer = CreateUniformBuffer(device, 2u);
    ASSERT_NE(firstBuffer, nullptr);
    ASSERT_NE(secondBuffer, nullptr);

    auto firstSet = device->CreateBindingSet(
        CreateMixedResourceSamplerBindingSetDesc(layout, firstBuffer, sampler));
    auto secondSet = device->CreateBindingSet(
        CreateMixedResourceSamplerBindingSetDesc(layout, secondBuffer, sampler));
    ASSERT_NE(firstSet, nullptr);
    ASSERT_NE(secondSet, nullptr);

    const auto firstSamplerHandle = GetSamplerGpuHandle(firstSet);
    const auto secondSamplerHandle = GetSamplerGpuHandle(secondSet);
    const auto firstResourceHandle = GetResourceGpuHandle(firstSet);
    const auto secondResourceHandle = GetResourceGpuHandle(secondSet);
    EXPECT_NE(firstSamplerHandle.ptr, 0u);
    EXPECT_NE(firstResourceHandle.ptr, 0u);
    EXPECT_EQ(secondSamplerHandle.ptr, firstSamplerHandle.ptr);
    EXPECT_NE(secondResourceHandle.ptr, firstResourceHandle.ptr);
}

TEST(DX12SamplerDescriptorReuseTests, ReleasingOneSharedOwnerKeepsTableReservedForRemainingOwner)
{
    auto device = CreateTestDeviceOrSkip();
    if (device == nullptr)
        GTEST_SKIP() << "DX12 device unavailable on this test machine";

    auto sharedLayout = CreateSamplerBindingLayout(device);
    ASSERT_NE(sharedLayout, nullptr);
    auto sharedSampler = device->CreateSampler(NLS::Render::RHI::SamplerDesc{}, "PartialReleaseSharedSampler");
    ASSERT_NE(sharedSampler, nullptr);

    auto firstOwner = device->CreateBindingSet(CreateSamplerBindingSetDesc(sharedLayout, sharedSampler));
    auto secondOwner = device->CreateBindingSet(CreateSamplerBindingSetDesc(sharedLayout, sharedSampler));
    ASSERT_NE(firstOwner, nullptr);
    ASSERT_NE(secondOwner, nullptr);

    const auto sharedHandle = GetSamplerGpuHandle(secondOwner);
    EXPECT_NE(sharedHandle.ptr, 0u);
    EXPECT_EQ(GetSamplerGpuHandle(firstOwner).ptr, sharedHandle.ptr);
    firstOwner.reset();
    EXPECT_EQ(GetSamplerGpuHandle(secondOwner).ptr, sharedHandle.ptr);

    auto uniqueLayout = CreateSamplerBindingLayout(device);
    ASSERT_NE(uniqueLayout, nullptr);

    std::vector<std::shared_ptr<NLS::Render::RHI::RHIBindingSet>> uniqueSets;
    uniqueSets.reserve(kDX12SamplerHeapCapacity - 1u);
    for (uint32_t index = 0u; index < kDX12SamplerHeapCapacity - 1u; ++index)
    {
        auto sampler = device->CreateSampler(MakeUniqueSamplerDesc(index + 1u), "PartialReleaseUniqueSampler");
        ASSERT_NE(sampler, nullptr);
        auto bindingSet = device->CreateBindingSet(CreateSamplerBindingSetDesc(uniqueLayout, sampler));
        ASSERT_NE(bindingSet, nullptr) << "unique binding set index " << index;
        uniqueSets.push_back(std::move(bindingSet));
    }

    auto overflowSampler = device->CreateSampler(MakeUniqueSamplerDesc(kDX12SamplerHeapCapacity), "PartialReleaseOverflowSampler");
    ASSERT_NE(overflowSampler, nullptr);
    EXPECT_EQ(
        device->CreateBindingSet(CreateSamplerBindingSetDesc(uniqueLayout, overflowSampler)),
        nullptr);
}

TEST(DX12SamplerDescriptorReuseTests, ConcurrentSharedSamplerTableCreateAndReleaseReusesStableRange)
{
    auto device = CreateTestDeviceOrSkip();
    if (device == nullptr)
        GTEST_SKIP() << "DX12 device unavailable on this test machine";

    auto layout = CreateSamplerBindingLayout(device);
    ASSERT_NE(layout, nullptr);
    auto sampler = device->CreateSampler(NLS::Render::RHI::SamplerDesc{}, "ConcurrentSharedSampler");
    ASSERT_NE(sampler, nullptr);
    auto stableOwner = device->CreateBindingSet(CreateSamplerBindingSetDesc(layout, sampler));
    ASSERT_NE(stableOwner, nullptr);
    const auto stableHandle = GetSamplerGpuHandle(stableOwner);
    ASSERT_NE(stableHandle.ptr, 0u);

    constexpr uint32_t threadCount = 8u;
    constexpr uint32_t iterationsPerThread = 64u;
    std::barrier startGate(static_cast<std::ptrdiff_t>(threadCount));
    std::atomic<bool> failed = false;
    std::vector<std::thread> workers;
    workers.reserve(threadCount);
    for (uint32_t threadIndex = 0u; threadIndex < threadCount; ++threadIndex)
    {
        workers.emplace_back(
            [&]()
            {
                startGate.arrive_and_wait();
                for (uint32_t iteration = 0u; iteration < iterationsPerThread; ++iteration)
                {
                    auto bindingSet = device->CreateBindingSet(CreateSamplerBindingSetDesc(layout, sampler));
                    if (bindingSet == nullptr || GetSamplerGpuHandle(bindingSet).ptr != stableHandle.ptr)
                    {
                        failed.store(true, std::memory_order_relaxed);
                        return;
                    }
                    bindingSet.reset();
                }
            });
    }

    for (auto& worker : workers)
        worker.join();

    EXPECT_FALSE(failed.load(std::memory_order_relaxed));
    EXPECT_EQ(GetSamplerGpuHandle(stableOwner).ptr, stableHandle.ptr);
}

TEST(DX12SamplerDescriptorReuseTests, FinalSharedSamplerTableOwnerReleasesDescriptorRange)
{
    auto device = CreateTestDeviceOrSkip();
    if (device == nullptr)
        GTEST_SKIP() << "DX12 device unavailable on this test machine";

    auto sharedLayout = CreateSamplerBindingLayout(device);
    ASSERT_NE(sharedLayout, nullptr);
    auto sharedSampler = device->CreateSampler(NLS::Render::RHI::SamplerDesc{}, "ReleaseSharedSampler");
    ASSERT_NE(sharedSampler, nullptr);

    std::vector<std::shared_ptr<NLS::Render::RHI::RHIBindingSet>> sharedSets;
    sharedSets.reserve(64u);
    for (uint32_t index = 0u; index < 64u; ++index)
    {
        auto bindingSet = device->CreateBindingSet(CreateSamplerBindingSetDesc(sharedLayout, sharedSampler));
        ASSERT_NE(bindingSet, nullptr);
        sharedSets.push_back(std::move(bindingSet));
    }
    sharedSets.clear();

    auto uniqueLayout = CreateSamplerBindingLayout(device);
    ASSERT_NE(uniqueLayout, nullptr);

    std::vector<std::shared_ptr<NLS::Render::RHI::RHIBindingSet>> uniqueSets;
    uniqueSets.reserve(kDX12SamplerHeapCapacity);
    for (uint32_t index = 0u; index < kDX12SamplerHeapCapacity; ++index)
    {
        auto sampler = device->CreateSampler(MakeUniqueSamplerDesc(index), "UniqueSampler");
        ASSERT_NE(sampler, nullptr);
        auto bindingSet = device->CreateBindingSet(CreateSamplerBindingSetDesc(uniqueLayout, sampler));
        ASSERT_NE(bindingSet, nullptr) << "unique binding set index " << index;
        uniqueSets.push_back(std::move(bindingSet));
    }

    auto overflowSampler = device->CreateSampler(MakeUniqueSamplerDesc(kDX12SamplerHeapCapacity), "OverflowSampler");
    ASSERT_NE(overflowSampler, nullptr);
    EXPECT_EQ(
        device->CreateBindingSet(CreateSamplerBindingSetDesc(uniqueLayout, overflowSampler)),
        nullptr);
}
