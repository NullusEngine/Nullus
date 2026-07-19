#include <gtest/gtest.h>

#include "Assets/AssetThumbnailPool.h"
#include "Assets/ThumbnailRendererRegistry.h"

#include <cstdint>
#include <memory>

namespace
{
template <typename T>
std::shared_ptr<T> MakeOpaqueShared(const uintptr_t value)
{
    auto* storage = new uintptr_t(value);
    return std::shared_ptr<T>(
        reinterpret_cast<T*>(storage),
        [](T* pointer)
        {
            delete reinterpret_cast<uintptr_t*>(pointer);
        });
}

NLS::Editor::Assets::AssetThumbnailGpuTexture MakeGpuTexture(
    const uintptr_t identity,
    const std::shared_ptr<void>& lease)
{
    return {
        MakeOpaqueShared<NLS::Render::RHI::RHITexture>(identity),
        MakeOpaqueShared<NLS::Render::RHI::RHITextureView>(identity + 100u),
        lease,
        96u,
        96u
    };
}

class RegistryPreviewRenderer final : public NLS::Editor::Assets::IEditorThumbnailPreviewRenderer
{
public:
    explicit RegistryPreviewRenderer(const NLS::Editor::Assets::AssetThumbnailKind supportedKind)
        : kind(supportedKind)
    {
    }

    bool Supports(const NLS::Editor::Assets::AssetThumbnailRequest& request) const override
    {
        return request.kind == kind;
    }

    NLS::Editor::Assets::EditorThumbnailPreviewResourcePumpResult PumpResources(
        const NLS::Editor::Assets::AssetThumbnailRequest&) override
    {
        ++pumpCount;
        return { true, false, {} };
    }

    NLS::Editor::Assets::EditorThumbnailPreviewResult Render(
        const NLS::Editor::Assets::AssetThumbnailRequest&) override
    {
        ++renderCount;
        return {};
    }

    NLS::Editor::Assets::AssetThumbnailKind kind;
    size_t pumpCount = 0u;
    size_t renderCount = 0u;
};
}

TEST(AssetThumbnailPoolTests, PublishesGpuTextureWithoutDiskImageAndResolvesLazily)
{
    auto pool = std::make_shared<NLS::Editor::Assets::AssetThumbnailPool>(4u);
    size_t resolveCount = 0u;
    size_t retireCount = 0u;
    pool->SetTextureCallbacks(
        [&resolveCount](const auto&)
        {
            ++resolveCount;
            return reinterpret_cast<void*>(uintptr_t {77u});
        },
        [&retireCount](const auto&, const bool)
        {
            ++retireCount;
        });

    auto lease = std::make_shared<uint8_t>(0u);
    const auto thumbnail = pool->MakeThumbnail("asset-key", 3u);
    ASSERT_TRUE(pool->Publish("asset-key", 3u, MakeGpuTexture(1u, lease)));

    const auto first = thumbnail.Resolve(10u);
    const auto second = thumbnail.Resolve(11u);
    EXPECT_TRUE(first.IsReady());
    EXPECT_EQ(first.width, 96u);
    EXPECT_EQ(first.height, 96u);
    EXPECT_EQ(second.textureId, first.textureId);
    EXPECT_EQ(resolveCount, 1u);

    pool->Remove("asset-key");
    EXPECT_EQ(retireCount, 1u);
}

TEST(AssetThumbnailPoolTests, RejectsStaleGenerationWithoutReplacingResidentTexture)
{
    auto pool = std::make_shared<NLS::Editor::Assets::AssetThumbnailPool>(4u);
    pool->SetTextureCallbacks(
        [](const auto&)
        {
            return reinterpret_cast<void*>(uintptr_t {1u});
        },
        {});

    ASSERT_TRUE(pool->Publish("asset-key", 5u, MakeGpuTexture(1u, std::make_shared<uint8_t>(0u))));
    EXPECT_FALSE(pool->Publish("asset-key", 4u, MakeGpuTexture(2u, std::make_shared<uint8_t>(0u))));
    EXPECT_EQ(
        pool->GetStatus("asset-key", 5u),
        NLS::Editor::Assets::ThumbnailRenderStatus::Ready);
    EXPECT_EQ(
        pool->GetStatus("asset-key", 4u),
        NLS::Editor::Assets::ThumbnailRenderStatus::NotReady);
    EXPECT_TRUE(pool->Resolve("asset-key", 5u, 1u).IsReady());
}

TEST(AssetThumbnailPoolTests, RepeatedPublicationOfSameGpuTextureIsIdempotent)
{
    auto pool = std::make_shared<NLS::Editor::Assets::AssetThumbnailPool>(4u);
    size_t resolveCount = 0u;
    size_t retireCount = 0u;
    pool->SetTextureCallbacks(
        [&resolveCount](const auto&)
        {
            ++resolveCount;
            return reinterpret_cast<void*>(uintptr_t {1u});
        },
        [&retireCount](const auto&, const bool)
        {
            ++retireCount;
        });

    auto texture = MakeGpuTexture(1u, std::make_shared<uint8_t>(0u));
    ASSERT_TRUE(pool->Publish("asset-key", 5u, texture));
    ASSERT_TRUE(pool->Resolve("asset-key", 5u, 1u).IsReady());
    ASSERT_TRUE(pool->Publish("asset-key", 5u, texture));
    ASSERT_TRUE(pool->Resolve("asset-key", 5u, 2u).IsReady());

    EXPECT_EQ(resolveCount, 1u);
    EXPECT_EQ(retireCount, 0u);
    EXPECT_EQ(pool->GetResidentTextureCount(), 1u);
}

TEST(AssetThumbnailPoolTests, StackAllocatedPoolReturnsInvalidThumbnailHandleWithoutThrowing)
{
    NLS::Editor::Assets::AssetThumbnailPool pool(1u);
    const auto thumbnail = pool.MakeThumbnail("asset-key", 1u);
    EXPECT_FALSE(thumbnail.IsValid());
    EXPECT_FALSE(thumbnail.Resolve(1u).IsReady());
}

TEST(AssetThumbnailPoolTests, RetainsRenderTargetLeaseUntilEntryIsEvicted)
{
    auto pool = std::make_shared<NLS::Editor::Assets::AssetThumbnailPool>(1u);
    auto lease = std::make_shared<uint8_t>(0u);
    std::weak_ptr<void> weakLease = lease;

    ASSERT_TRUE(pool->Publish("first", 1u, MakeGpuTexture(1u, lease)));
    lease.reset();
    EXPECT_FALSE(weakLease.expired());

    ASSERT_TRUE(pool->Publish(
        "second",
        1u,
        MakeGpuTexture(2u, std::make_shared<uint8_t>(0u))));
    (void)pool->Resolve("second", 1u, 2u);
    pool->Prune(2u);

    EXPECT_FALSE(weakLease.expired())
        << "The previous UI frame may still sample a texture after its pool entry is evicted.";
    pool->Prune(5u);
    EXPECT_TRUE(weakLease.expired());
    EXPECT_EQ(pool->GetResidentTextureCount(), 1u);
}

TEST(AssetThumbnailPoolTests, NotReadyUpdateDoesNotHideAlreadyPublishedTexture)
{
    auto pool = std::make_shared<NLS::Editor::Assets::AssetThumbnailPool>(2u);
    ASSERT_TRUE(pool->Publish("asset-key", 7u, MakeGpuTexture(1u, std::make_shared<uint8_t>(0u))));

    pool->SetStatus(
        "asset-key",
        7u,
        NLS::Editor::Assets::ThumbnailRenderStatus::NotReady);

    EXPECT_EQ(
        pool->GetStatus("asset-key", 7u),
        NLS::Editor::Assets::ThumbnailRenderStatus::Ready);
}

TEST(ThumbnailRendererRegistryTests, DispatchesOnlyToRendererRegisteredForAssetKind)
{
    using namespace NLS::Editor::Assets;

    auto modelRenderer = std::make_shared<RegistryPreviewRenderer>(AssetThumbnailKind::ModelPreview);
    auto materialRenderer = std::make_shared<RegistryPreviewRenderer>(AssetThumbnailKind::MaterialSphere);
    ThumbnailRendererRegistry registry;
    registry.Register(AssetThumbnailKind::ModelPreview, modelRenderer);
    registry.Register(AssetThumbnailKind::MaterialSphere, materialRenderer);

    AssetThumbnailRequest modelRequest;
    modelRequest.kind = AssetThumbnailKind::ModelPreview;
    EXPECT_TRUE(registry.Supports(modelRequest));
    EXPECT_TRUE(registry.PumpResources(modelRequest).supported);
    (void)registry.Render(modelRequest);
    EXPECT_EQ(modelRenderer->pumpCount, 1u);
    EXPECT_EQ(modelRenderer->renderCount, 1u);
    EXPECT_EQ(materialRenderer->pumpCount, 0u);
    EXPECT_EQ(materialRenderer->renderCount, 0u);

    AssetThumbnailRequest prefabRequest;
    prefabRequest.kind = AssetThumbnailKind::PrefabPreview;
    EXPECT_FALSE(registry.Supports(prefabRequest));
    EXPECT_EQ(
        registry.Render(prefabRequest).diagnostic,
        "thumbnail-renderer-unregistered");
    EXPECT_EQ(
        registry.Render(prefabRequest).status,
        ThumbnailRenderStatus::Unsupported);
}
