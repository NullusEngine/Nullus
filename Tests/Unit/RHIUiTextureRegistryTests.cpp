#include <gtest/gtest.h>

#include <optional>
#include <type_traits>
#include <utility>

#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/UI/RHIImGuiTextureRegistry.h"

namespace
{
    constexpr auto kPreviousFrameOrStatic =
        NLS::Render::UI::UiTextureSynchronizationScope::PreviousFrameOrStatic;

    class TestTexture final : public NLS::Render::RHI::RHITexture
    {
    public:
        explicit TestTexture(
            const NLS::Render::RHI::ResourceState state = NLS::Render::RHI::ResourceState::ShaderRead)
            : m_state(state)
        {
        }

        std::string_view GetDebugName() const override { return "RHIUiTextureRegistryTestsTexture"; }
        const NLS::Render::RHI::RHITextureDesc& GetDesc() const override { return m_desc; }
        NLS::Render::RHI::ResourceState GetState() const override
        {
            return m_state;
        }

    private:
        NLS::Render::RHI::RHITextureDesc m_desc {};
        NLS::Render::RHI::ResourceState m_state = NLS::Render::RHI::ResourceState::ShaderRead;
    };

    class TestTextureView final : public NLS::Render::RHI::RHITextureView
    {
    public:
        explicit TestTextureView(std::shared_ptr<NLS::Render::RHI::RHITexture> texture)
            : m_texture(std::move(texture))
        {
        }

        std::string_view GetDebugName() const override { return "RHIUiTextureRegistryTestsTextureView"; }
        const NLS::Render::RHI::RHITextureViewDesc& GetDesc() const override { return m_desc; }
        const std::shared_ptr<NLS::Render::RHI::RHITexture>& GetTexture() const override { return m_texture; }

    private:
        NLS::Render::RHI::RHITextureViewDesc m_desc {};
        std::shared_ptr<NLS::Render::RHI::RHITexture> m_texture;
    };
}

TEST(RHIUiTextureRegistryTests, TextureRegistryContractHeaderExists)
{
    NLS::Render::UI::RHIImGuiTextureRegistry registry;
    const auto entry = registry.Resolve({ 1u, 1u });

    EXPECT_FALSE(entry.has_value());
}

TEST(RHIUiTextureRegistryTests, ResolveReturnsCopiedEntryInsteadOfRawMapPointer)
{
    using ResolveResult = decltype(std::declval<const NLS::Render::UI::RHIImGuiTextureRegistry&>().Resolve(
        NLS::Render::UI::UiTextureId {}));

    EXPECT_TRUE((std::is_same_v<
        ResolveResult,
        std::optional<NLS::Render::UI::RHIImGuiTextureRegistryEntry>>));
}

TEST(RHIUiTextureRegistryTests, RejectsNullTextureViewsWithoutConsumingStableIds)
{
    NLS::Render::UI::RHIImGuiTextureRegistry registry;

    const auto nullId = registry.RegisterTextureView(
        nullptr,
        kPreviousFrameOrStatic);

    EXPECT_FALSE(nullId.IsValid());

    auto texture = std::make_shared<TestTexture>();
    auto textureView = std::make_shared<TestTextureView>(texture);
    const auto firstValidId = registry.RegisterTextureView(
        textureView,
        kPreviousFrameOrStatic);

    EXPECT_TRUE(firstValidId.IsValid());
    EXPECT_EQ(firstValidId.value, 1u);
    EXPECT_EQ(firstValidId.generation, 1u);
}

TEST(RHIUiTextureRegistryTests, RejectsSameFrameProducerScopeWithoutProducerDependency)
{
    NLS::Render::UI::RHIImGuiTextureRegistry registry;
    auto texture = std::make_shared<TestTexture>();
    auto textureView = std::make_shared<TestTextureView>(texture);

    const auto sameFrameId = registry.RegisterTextureView(
        textureView,
        NLS::Render::UI::UiTextureSynchronizationScope::SameFrameProducer);

    EXPECT_FALSE(sameFrameId.IsValid());
    EXPECT_FALSE(registry.Resolve(sameFrameId).has_value());
}

TEST(RHIUiTextureRegistryTests, RejectsPreviousFrameStaticTextureViewsThatAreNotShaderReadable)
{
    NLS::Render::UI::RHIImGuiTextureRegistry registry;
    auto renderTargetTexture = std::make_shared<TestTexture>(NLS::Render::RHI::ResourceState::RenderTarget);
    auto renderTargetView = std::make_shared<TestTextureView>(renderTargetTexture);

    const auto id = registry.RegisterTextureView(
        renderTargetView,
        kPreviousFrameOrStatic);

    EXPECT_FALSE(id.IsValid())
        << "PreviousFrameOrStatic UI textures must already be in ShaderRead state; same-frame producers need "
           "explicit dependency metadata instead of an implicit Unknown->ShaderRead transition.";
    EXPECT_FALSE(registry.Resolve(id).has_value());
}

TEST(RHIUiTextureRegistryTests, ReRegisteringSameTextureViewReturnsStableIdentity)
{
    NLS::Render::UI::RHIImGuiTextureRegistry registry;
    auto texture = std::make_shared<TestTexture>();
    auto textureView = std::make_shared<TestTextureView>(texture);

    const auto firstId = registry.RegisterTextureView(
        textureView,
        kPreviousFrameOrStatic);
    const auto secondId = registry.RegisterTextureView(
        textureView,
        kPreviousFrameOrStatic);

    EXPECT_EQ(secondId.value, firstId.value);
    EXPECT_EQ(secondId.generation, firstId.generation);

    const auto entry = registry.Resolve(firstId);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->textureView, textureView);
}

TEST(RHIUiTextureRegistryTests, ReleasedTextureViewNoLongerResolvesForNewSnapshots)
{
    NLS::Render::UI::RHIImGuiTextureRegistry registry;
    auto texture = std::make_shared<TestTexture>();
    auto textureView = std::make_shared<TestTextureView>(texture);

    const auto id = registry.RegisterTextureView(textureView, kPreviousFrameOrStatic);
    ASSERT_TRUE(registry.Resolve(id).has_value());

    registry.ReleaseTextureView(textureView, 7u);

    EXPECT_FALSE(registry.Resolve(id).has_value());
}

TEST(RHIUiTextureRegistryTests, ReleasedTextureViewStillResolvesForPublishedFrameUntilRetired)
{
    NLS::Render::UI::RHIImGuiTextureRegistry registry;
    auto texture = std::make_shared<TestTexture>();
    auto textureView = std::make_shared<TestTextureView>(texture);

    const auto id = registry.RegisterTextureView(textureView, kPreviousFrameOrStatic);
    ASSERT_TRUE(id.IsValid());

    registry.ReleaseTextureView(textureView, 7u);

    const auto inFlightEntry = registry.ResolveForFrame(id, 7u);
    ASSERT_TRUE(inFlightEntry.has_value());
    EXPECT_EQ(inFlightEntry->textureView, textureView);

    EXPECT_FALSE(registry.ResolveForFrame(id, 8u).has_value());
    registry.ReleaseRetiredTextureViewsUpTo(7u);
    EXPECT_FALSE(registry.ResolveForFrame(id, 7u).has_value());
}

TEST(RHIUiTextureRegistryTests, ReleaseRetainsTextureViewEntryUntilRetiredFrame)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    NLS::Render::UI::RHIImGuiTextureRegistry registry;
    auto texture = std::make_shared<TestTexture>();
    auto textureView = std::make_shared<TestTextureView>(texture);

    const auto id = registry.RegisterTextureView(textureView, kPreviousFrameOrStatic);
    ASSERT_TRUE(id.IsValid());
    ASSERT_EQ(registry.GetEntryCountForTesting(), 1u);

    registry.ReleaseTextureView(textureView, 8u);

    EXPECT_EQ(registry.GetEntryCountForTesting(), 1u);
    registry.ReleaseRetiredTextureViewsUpTo(7u);
    EXPECT_EQ(registry.GetEntryCountForTesting(), 1u);
    registry.ReleaseRetiredTextureViewsUpTo(8u);
    EXPECT_EQ(registry.GetEntryCountForTesting(), 0u);
#else
    GTEST_SKIP() << "Requires NLS_ENABLE_TEST_HOOKS.";
#endif
}

TEST(RHIUiTextureRegistryTests, ReleaseRetirementQueueDrainsOnlyCompletedFrames)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    NLS::Render::UI::RHIImGuiTextureRegistry registry;
    auto firstTexture = std::make_shared<TestTexture>();
    auto firstView = std::make_shared<TestTextureView>(firstTexture);
    auto secondTexture = std::make_shared<TestTexture>();
    auto secondView = std::make_shared<TestTextureView>(secondTexture);

    const auto firstId = registry.RegisterTextureView(firstView, kPreviousFrameOrStatic);
    const auto secondId = registry.RegisterTextureView(secondView, kPreviousFrameOrStatic);
    ASSERT_TRUE(firstId.IsValid());
    ASSERT_TRUE(secondId.IsValid());

    registry.ReleaseTextureView(firstView, 5u);
    registry.ReleaseTextureView(secondView, 7u);

    EXPECT_EQ(registry.GetEntryCountForTesting(), 2u);
    EXPECT_EQ(registry.GetPendingRetiredEntryCountForTesting(), 2u);

    registry.ReleaseRetiredTextureViewsUpTo(5u);
    EXPECT_EQ(registry.GetEntryCountForTesting(), 1u);
    EXPECT_EQ(registry.GetPendingRetiredEntryCountForTesting(), 1u);
    EXPECT_FALSE(registry.ResolveForFrame(firstId, 5u).has_value());
    EXPECT_TRUE(registry.ResolveForFrame(secondId, 7u).has_value());

    registry.ReleaseRetiredTextureViewsUpTo(7u);
    EXPECT_EQ(registry.GetEntryCountForTesting(), 0u);
    EXPECT_EQ(registry.GetPendingRetiredEntryCountForTesting(), 0u);
#else
    GTEST_SKIP() << "Requires NLS_ENABLE_TEST_HOOKS.";
#endif
}

TEST(RHIUiTextureRegistryTests, ReRegisteringReleasedTextureViewUsesNewIdentityWhileOldEntryIsRetained)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    NLS::Render::UI::RHIImGuiTextureRegistry registry;
    auto texture = std::make_shared<TestTexture>();
    auto textureView = std::make_shared<TestTextureView>(texture);

    const auto firstId = registry.RegisterTextureView(textureView, kPreviousFrameOrStatic);
    ASSERT_TRUE(firstId.IsValid());

    registry.ReleaseTextureView(textureView, 10u);
    const auto secondId = registry.RegisterTextureView(textureView, kPreviousFrameOrStatic);

    EXPECT_TRUE(secondId.IsValid());
    EXPECT_NE(secondId.value, firstId.value);
    EXPECT_EQ(registry.GetEntryCountForTesting(), 2u);

    registry.ReleaseRetiredTextureViewsUpTo(10u);
    EXPECT_EQ(registry.GetEntryCountForTesting(), 1u);
    const auto entry = registry.Resolve(secondId);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->textureView, textureView);
#else
    GTEST_SKIP() << "Requires NLS_ENABLE_TEST_HOOKS.";
#endif
}
