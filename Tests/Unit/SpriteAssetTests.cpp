#include <gtest/gtest.h>
#include <Core/Assets/SpriteSortingLayers.h>
#include <Assets/SpriteArtifact.h>
#include <Assets/SpriteGeometry.h>
#include <Assets/NativeArtifactContainer.h>
#include <Core/ResourceManagement/ResourceHandle.h>
#include <filesystem>
#include <limits>
#include <Rendering/Sprites/SpriteBatcher.h>

namespace
{
using namespace NLS::Core::Assets;
TEST(SpriteResources, SharedFrameLeaseSurvivesSceneOwnerReleaseAndEndsWithTheLastSnapshot)
{
    using namespace NLS::Core::ResourceManagement;
    ResourceLifetimeRegistry registry;
    int resource = 7;
    ResourceLifetimeAcquireRequest frameRequest {"sprite-frame", ResourceLifetimeResourceType::Texture,
        "sprite-test-texture", 16, ResourceLifetimeOwnerKind::RenderSnapshot};
    auto firstFrame = std::make_shared<ResourceHandle<int>>(registry, frameRequest,
        [&resource](const ResourceId&) { return &resource; });
    auto secondFrame = firstFrame;
    const auto sceneLease = registry.Acquire({"scene-object", ResourceLifetimeResourceType::Texture,
        "sprite-test-texture", 16, ResourceLifetimeOwnerKind::SceneInstance});
    registry.ReleaseOwnersByKind(ResourceLifetimeOwnerKind::SceneInstance);
    EXPECT_EQ(secondFrame->Get(), &resource);
    EXPECT_EQ(registry.GetActiveOwnerCount(sceneLease.type, sceneLease.normalizedPath), 1);
    firstFrame.reset();
    EXPECT_TRUE(registry.HasActiveOwners(sceneLease.type, sceneLease.normalizedPath));
    secondFrame.reset();
    EXPECT_FALSE(registry.HasActiveOwners(sceneLease.type, sceneLease.normalizedPath));
}

AssetMeta SpriteMeta()
{
    AssetMeta meta;
    meta.id = AssetId::New();
    meta.assetType = AssetType::Texture;
    meta.importerId = "texture";
    meta.settings["TEXTURE_TYPE"] = "sprite";
    return meta;
}
SpriteArtifact Sprite(const AssetMeta& meta)
{
    std::string error;
    auto sprite = BuildSingleSpriteArtifact(meta, 200, 100, "texture:main", "Hero", error);
    if (!sprite) throw std::runtime_error(error);
    return *sprite;
}

TEST(SpriteAsset, IdentitySurvivesRenameAndSourceResize)
{
    auto meta = SpriteMeta();
    auto before = Sprite(meta);
    std::string error;
    auto after = BuildSingleSpriteArtifact(meta, 400, 300, "texture:main", "Renamed", error);
    ASSERT_TRUE(after) << error;
    EXPECT_EQ(before.SubAssetKey(), after->SubAssetKey());
    EXPECT_NE(before.width, after->width);
    meta.settings["SPRITE_ID"] = NLS::Guid::New().ToString();
    EXPECT_EQ(Sprite(meta).id, meta.settings["SPRITE_ID"]);
}
TEST(SpriteAsset, OrdinaryTextureKeepsItsOnlyPrimaryArtifact)
{
    auto meta = SpriteMeta();
    meta.settings.clear();
    ArtifactWriteRequest request;
    request.primarySubAssetKey = "texture:main";
    request.artifacts.emplace_back("texture:main", ArtifactType::Texture, "texture", "texture", std::vector<uint8_t>{1});
    std::string error;
    ASSERT_TRUE(AppendSingleSpriteArtifact(request, meta, 200, 100, "texture:main", "Hero", error));
    EXPECT_EQ(request.artifacts.size(), 1);
    EXPECT_EQ(request.primarySubAssetKey, "texture:main");
}
TEST(SpriteAsset, SiblingPreservesTexturePrimaryAndRecordsDependency)
{
    auto meta = SpriteMeta();
    ArtifactWriteRequest request;
    request.primarySubAssetKey = "texture:main";
    request.artifacts.emplace_back("texture:main", ArtifactType::Texture, "texture", "texture", std::vector<uint8_t>{1});
    std::string error;
    ASSERT_TRUE(AppendSingleSpriteArtifact(request, meta, 200, 100, "texture:main", "Hero", error));
    ASSERT_EQ(request.artifacts.size(), 2);
    EXPECT_EQ(request.primarySubAssetKey, "texture:main");
    EXPECT_EQ(request.artifacts.back().artifactType, ArtifactType::Sprite);
    ASSERT_EQ(request.artifacts.back().dependencies.size(), 1);
    EXPECT_EQ(request.artifacts.back().dependencies[0].hashOrVersion, "texture:main");
}
TEST(SpriteAsset, RectAndPivotUseLogicalPixelsIndependentOfTargetSize)
{
    auto sprite = Sprite(SpriteMeta());
    sprite.x = 20; sprite.y = 10; sprite.width = 100; sprite.height = 50;
    sprite.pivot = {0.25f, 1}; sprite.pixelsPerUnit = 50;
    const auto rect = sprite.LocalRect();
    EXPECT_FLOAT_EQ(rect[0], -0.5f); EXPECT_FLOAT_EQ(rect[1], -1);
    EXPECT_FLOAT_EQ(rect[2], 1.5f); EXPECT_FLOAT_EQ(rect[3], 0);
    const auto uv = sprite.UvRect();
    EXPECT_FLOAT_EQ(uv[0], 0.1f); EXPECT_FLOAT_EQ(uv[1], 0.1f);
    EXPECT_FLOAT_EQ(uv[2], 0.6f); EXPECT_FLOAT_EQ(uv[3], 0.6f);
}
TEST(SpriteAsset, BinaryRoundtripIsDeterministic)
{
    auto sprite = Sprite(SpriteMeta());
    auto bytes = SerializeSpriteArtifact(sprite);
    std::string error;
    auto loaded = DeserializeSpriteArtifact(bytes, error);
    ASSERT_TRUE(loaded) << error;
    EXPECT_EQ(bytes, SerializeSpriteArtifact(*loaded));
    EXPECT_EQ(sprite.textureAssetId, loaded->textureAssetId);
    EXPECT_EQ(sprite.pivot, loaded->pivot);
}
TEST(SpriteAsset, BordersUseVersionTwoWhileLegacyPayloadBytesRemainVersionOne)
{
    auto sprite=Sprite(SpriteMeta());const auto legacy=SerializeSpriteArtifact(sprite);ASSERT_GT(legacy.size(),8);
    EXPECT_EQ(legacy[4],1);std::string error;
    EXPECT_EQ(DeserializeSpriteArtifact(legacy,error)->border,(std::array<uint32_t,4>{}));
    sprite.border={20,10,30,20};const auto bordered=SerializeSpriteArtifact(sprite);
    EXPECT_EQ(bordered.size(),legacy.size()+16);EXPECT_EQ(bordered[4],2);
    const auto loaded=DeserializeSpriteArtifact(bordered,error);ASSERT_TRUE(loaded)<<error;
    EXPECT_EQ(loaded->border,sprite.border);EXPECT_EQ(SerializeSpriteArtifact(*loaded),bordered);
    for(size_t size=legacy.size();size<bordered.size();++size)
        EXPECT_FALSE(DeserializeSpriteArtifact(std::span(bordered).first(size),error));
    sprite.border={};EXPECT_EQ(SerializeSpriteArtifact(sprite),legacy);
    auto meta=SpriteMeta();meta.settings["SPRITE_BORDER_LEFT"]="20";
    EXPECT_EQ(Sprite(meta).border[0],20);
    for(const auto* invalid:{"-1","1.5","nan","4294967296","201"})
    {
        meta.settings["SPRITE_BORDER_LEFT"]=invalid;
        EXPECT_FALSE(BuildSingleSpriteArtifact(meta,200,100,"texture:main","Hero",error));
    }
}
TEST(SpriteGeometry, NineSlicesPreserveCornerWidthsUvOrientationAndCompleteCoverage)
{
    auto sprite=Sprite(SpriteMeta());sprite.border={20,10,30,20};std::string error;
    const auto geometry=BuildSlicedSpriteGeometry(sprite,4,3,error);ASSERT_TRUE(geometry)<<error;ASSERT_EQ(geometry->count,9);
    EXPECT_EQ(geometry->bounds,(std::array<float,4>{-2,-1.5f,2,1.5f}));
    const auto& corner=geometry->patches[0];EXPECT_NEAR(corner.localRect[2]-corner.localRect[0],.2f,1e-6f);
    EXPECT_NEAR(corner.localRect[3]-corner.localRect[1],.1f,1e-6f);
    EXPECT_FLOAT_EQ(corner.uvRect[1],.9f);EXPECT_FLOAT_EQ(corner.uvRect[3],1);
    const auto& center=geometry->patches[4];EXPECT_NEAR(center.localRect[2]-center.localRect[0],3.5f,1e-6f);
    EXPECT_FLOAT_EQ(center.uvRect[0],.1f);EXPECT_FLOAT_EQ(center.uvRect[2],.85f);
    EXPECT_FLOAT_EQ(center.uvRect[1],.2f);EXPECT_FLOAT_EQ(center.uvRect[3],.9f);
    float area=0;for(uint32_t i=0;i<geometry->count;++i)
    {const auto& rect=geometry->patches[i].localRect;area+=(rect[2]-rect[0])*(rect[3]-rect[1]);}
    EXPECT_NEAR(area,12.f,1e-5f);
    sprite.x=20;sprite.y=10;sprite.width=100;sprite.height=50;sprite.border={10,5,20,10};
    const auto cropped=BuildSlicedSpriteGeometry(sprite,4,3,error);ASSERT_TRUE(cropped)<<error;
    EXPECT_FLOAT_EQ(cropped->patches[0].uvRect[0],.1f);EXPECT_FLOAT_EQ(cropped->patches[0].uvRect[1],.55f);
    EXPECT_FLOAT_EQ(cropped->patches[0].uvRect[2],.15f);EXPECT_FLOAT_EQ(cropped->patches[0].uvRect[3],.6f);
}
TEST(SpriteGeometry, SmallTargetsCompressBordersWithoutOverlappingOrDegeneratePatches)
{
    auto sprite=Sprite(SpriteMeta());sprite.border={20,10,30,20};std::string error;
    const auto small=BuildSlicedSpriteGeometry(sprite,.25f,.15f,error);ASSERT_TRUE(small)<<error;EXPECT_EQ(small->count,4);
    float area=0;for(uint32_t i=0;i<small->count;++i)
    {
        const auto& rect=small->patches[i].localRect;EXPECT_GT(rect[2],rect[0]);EXPECT_GT(rect[3],rect[1]);
        area+=(rect[2]-rect[0])*(rect[3]-rect[1]);
    }
    EXPECT_NEAR(area,.25f*.15f,1e-7f);
    sprite.border={};const auto plain=BuildSlicedSpriteGeometry(sprite,4,3,error);ASSERT_TRUE(plain);EXPECT_EQ(plain->count,1);
    for(float size:{0.f,-1.f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()})
        EXPECT_FALSE(BuildSlicedSpriteGeometry(sprite,size,1,error));
}
TEST(SpriteGeometry, TiledSpritesRepeatAtNativeSizeAndClipPartialTiles)
{
    auto sprite=Sprite(SpriteMeta());std::string error;
    const auto geometry=BuildTiledSpriteGeometry(sprite,5,2.5f,error);ASSERT_TRUE(geometry)<<error;
    ASSERT_EQ(geometry->patches.size(),9);
    const auto& last=geometry->patches.back();
    EXPECT_EQ(last.uvRect,(std::array<float,4>{0,.5f,.5f,1}));
    EXPECT_FLOAT_EQ(last.localRect[2],2.5f);EXPECT_FLOAT_EQ(last.localRect[3],1.25f);
    float area=0;for(const auto& patch:geometry->patches)
        area+=(patch.localRect[2]-patch.localRect[0])*(patch.localRect[3]-patch.localRect[1]);
    EXPECT_NEAR(area,12.5f,1e-5f);
    EXPECT_FALSE(BuildTiledSpriteGeometry(sprite,1000000,1000000,error));EXPECT_FALSE(error.empty());
}
TEST(SpriteGeometry, TiledBorderStripsKeepCornersAndRepeatTheCenter)
{
    auto sprite=Sprite(SpriteMeta());sprite.sourceWidth=sprite.width=sprite.sourceHeight=sprite.height=4;
    sprite.pixelsPerUnit=4;sprite.border={1,1,1,1};std::string error;
    const auto geometry=BuildTiledSpriteGeometry(sprite,1.5f,1.5f,error);ASSERT_TRUE(geometry)<<error;
    EXPECT_EQ(geometry->patches.size(),16);
    size_t centers=0;float area=0;
    for(const auto& patch:geometry->patches)
    {
        if(patch.uvRect==std::array<float,4>{.25f,.25f,.75f,.75f})++centers;
        area+=(patch.localRect[2]-patch.localRect[0])*(patch.localRect[3]-patch.localRect[1]);
    }
    EXPECT_EQ(centers,4);EXPECT_NEAR(area,2.25f,1e-6f);
    const auto small=BuildTiledSpriteGeometry(sprite,.25f,.25f,error);ASSERT_TRUE(small);EXPECT_EQ(small->patches.size(),4);
}
TEST(SpriteAsset, RejectsTruncationAtEveryByteAndTrailingData)
{
    auto bytes = SerializeSpriteArtifact(Sprite(SpriteMeta()));
    std::string error;
    for (size_t size = 0; size < bytes.size(); ++size)
        EXPECT_FALSE(DeserializeSpriteArtifact(std::span(bytes).first(size), error)) << size;
    bytes.push_back(0);
    EXPECT_FALSE(DeserializeSpriteArtifact(bytes, error));
}
TEST(SpriteAsset, RejectsUnsupportedVersionsAndOversizedPayloads)
{
    auto bytes = SerializeSpriteArtifact(Sprite(SpriteMeta()));
    bytes[4] = 255;
    std::string error;
    EXPECT_FALSE(DeserializeSpriteArtifact(bytes, error));
    bytes.resize(17000);
    EXPECT_FALSE(DeserializeSpriteArtifact(bytes, error));
}
TEST(SpriteAsset, RejectsOverflowingRectsAndNonfiniteParameters)
{
    auto sprite = Sprite(SpriteMeta());
    std::string error;
    sprite.x = UINT32_MAX;
    EXPECT_FALSE(sprite.Validate(error));
    sprite.x = 0; sprite.width = UINT32_MAX;
    EXPECT_FALSE(sprite.Validate(error));
    sprite.width = 200; sprite.pixelsPerUnit = 0;
    EXPECT_FALSE(sprite.Validate(error));
    sprite.pixelsPerUnit = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(sprite.Validate(error));
    sprite.pixelsPerUnit = 100; sprite.pivot[0] = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(sprite.Validate(error));
}
TEST(SpriteAsset, InvalidImportSettingsFailWithoutPartialSibling)
{
    auto meta = SpriteMeta();
    ArtifactWriteRequest request;
    std::string error;
    for (const auto& value : {"0", "-1", "nan", "100oops"})
    {
        meta.settings["SPRITE_PIXELS_PER_UNIT"] = value;
        EXPECT_FALSE(AppendSingleSpriteArtifact(request, meta, 100, 100, "texture:main", "Hero", error));
        EXPECT_TRUE(request.artifacts.empty());
        EXPECT_FALSE(error.empty());
    }
}
TEST(SpriteAsset, NativeContainerRetainsSpriteTypeAndDetectsCorruption)
{
    auto meta = SpriteMeta();
    const auto sprite = Sprite(meta);
    NativeArtifactMetadata metadata;
    metadata.artifactType = ArtifactType::Sprite; metadata.schemaName = "sprite";
    metadata.schemaVersion = SpriteArtifactVersion; metadata.sourceAssetId = meta.id;
    metadata.subAssetKey = sprite.SubAssetKey();
    auto bytes = WriteNativeArtifactContainer(metadata, SerializeSpriteArtifact(sprite));
    auto loaded = ReadNativeArtifactContainer(bytes, ArtifactType::Sprite, SpriteArtifactVersion);
    ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded->metadata.subAssetKey, sprite.SubAssetKey());
    std::string error;
    EXPECT_TRUE(DeserializeSpriteArtifact(loaded->payload, error));
    bytes.back() ^= 1;
    EXPECT_FALSE(ReadNativeArtifactContainer(bytes, ArtifactType::Sprite, SpriteArtifactVersion));
}
TEST(SpriteBatcher, PreservesTransparentTextureInterleaving)
{
    using namespace NLS::Render::Sprites;
    std::vector<SpriteBatchItem> items {
        {{0,0,3,1},{1,10,0},0}, {{0,0,2,2},{1,20,0},1}, {{0,0,1,3},{1,10,0},2}};
    const auto result = BuildSpriteBatches(items);
    ASSERT_EQ(result.batches.size(), 3);
    EXPECT_EQ(result.sorted[1].binding.texture, 20);
    EXPECT_EQ(result.stateBreaks, 2);
}
TEST(SpriteBatcher, LayerAndOrderPrecedeDepthWithStableTies)
{
    using namespace NLS::Render::Sprites;
    std::vector<SpriteBatchItem> items {
        {{1,0,100,3},{},0}, {{0,2,100,2},{},1}, {{0,1,1,9},{},2}, {{0,1,1,4},{},3}};
    const auto result = BuildSpriteBatches(items);
    ASSERT_EQ(result.sorted.size(), 4);
    EXPECT_EQ(result.sorted[0].sourceIndex, 3);
    EXPECT_EQ(result.sorted[1].sourceIndex, 2);
    EXPECT_EQ(result.sorted[2].sourceIndex, 1);
    EXPECT_EQ(result.sorted[3].sourceIndex, 0);
}
TEST(SpriteBatcher, BatchesTenThousandCompatibleSpritesAndSplitsAtCapacity)
{
    using namespace NLS::Render::Sprites;
    std::vector<SpriteBatchItem> items(10000);
    for (uint32_t index = 0; index < items.size(); ++index) items[index].sorting.stableId = index;
    auto result = BuildSpriteBatches(items);
    ASSERT_EQ(result.batches.size(), 1);
    EXPECT_EQ(result.batches.front().count, 10000);
    result = BuildSpriteBatches(items, 4096);
    ASSERT_EQ(result.batches.size(), 3);
    EXPECT_EQ(result.batches.back().count, 1808);
    EXPECT_EQ(result.capacityBreaks, 2);
    items[4].visible = false;
    items[8].sorting.depth = std::numeric_limits<float>::quiet_NaN();
    result = BuildSpriteBatches(items);
    EXPECT_EQ(result.submitted, 10000);
    EXPECT_EQ(result.culled, 2);
    EXPECT_EQ(result.sorted.size(), 9998);
}
TEST(SpriteBatcher, ForeignTransparentDrawPreventsInstancingAcrossItsDepth)
{
    using namespace NLS::Render::Sprites;
    const std::vector<SpriteBatchItem> sprites {
        {{0,0,5,1},{1,1,0},0}, {{0,0,3,3},{1,1,0},1}, {{0,0,1,5},{1,1,0},2}};
    const std::vector<SpriteSortKey> meshAndVfx {{0,0,4,2},{0,0,2,4}};
    const auto result = BuildSpriteBatches(sprites, 16384, meshAndVfx);
    ASSERT_EQ(result.batches.size(),3);
    EXPECT_EQ(result.orderingBreaks,2);
    EXPECT_EQ(result.stateBreaks,0);
}
TEST(SpriteSortingLayers, RenameAndReorderKeepReferencesAndMissingLayerFallsBack)
{
    SpriteSortingLayers table;
    const auto id=NLS::Guid::New().ToString();table.layers.push_back({id,"Characters"});
    table.layers[1].name="Actors";std::swap(table.layers[0],table.layers[1]);
    std::string error;SpriteSortingLayers reloaded;
    ASSERT_TRUE(SpriteSortingLayers::Parse(table.Serialize(),reloaded,error))<<error;
    EXPECT_EQ(reloaded.ResolveRank(id),0);
    EXPECT_EQ(reloaded.ResolveRank(SpriteSortingLayers::DefaultId),1);
    EXPECT_EQ(reloaded.ResolveRank("removed-layer"),1);
    EXPECT_EQ(reloaded.layers[0].name,"Actors");
}
TEST(SpriteSortingLayers, InvalidTableDoesNotReplaceTheLastValidRevision)
{
    SpriteSortingLayers table;const auto before=table.Serialize();std::string error;
    EXPECT_FALSE(SpriteSortingLayers::Parse("{\"version\":99,\"layers\":[]}",table,error));
    EXPECT_EQ(table.Serialize(),before);
    auto duplicate=table;duplicate.layers.push_back(duplicate.layers[0]);
    EXPECT_FALSE(SpriteSortingLayers::Parse(duplicate.Serialize(),table,error));
    auto missing=table;missing.layers[0].id=NLS::Guid::New().ToString();
    EXPECT_FALSE(SpriteSortingLayers::Parse(missing.Serialize(),table,error));
    EXPECT_EQ(table.Serialize(),before);
}
}
