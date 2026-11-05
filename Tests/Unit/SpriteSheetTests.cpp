#include <gtest/gtest.h>
#include <Assets/SpriteSheet.h>
#include <Json/json.hpp>
#include <algorithm>
#include <limits>

namespace
{
using namespace NLS::Core::Assets;
AssetMeta SheetMeta()
{
    AssetMeta meta; meta.id = AssetId::New(); meta.assetType = AssetType::Texture;
    meta.settings["TEXTURE_TYPE"] = "sprite";
    return meta;
}

TEST(SpriteSheet, SingleModeKeepsTheExistingBinaryAndTextureIdentity)
{
    auto meta = SheetMeta(); std::string error;
    const auto original = BuildSingleSpriteArtifact(meta,32,16,"texture:main","Hero",error); ASSERT_TRUE(original);
    const auto sprites = BuildSpriteArtifacts(meta,32,16,"texture:main","Hero",error);
    ASSERT_TRUE(sprites); ASSERT_EQ(sprites->size(),1);
    EXPECT_EQ(SerializeSpriteArtifact(sprites->front()),SerializeSpriteArtifact(*original));
    ArtifactWriteRequest request; request.primarySubAssetKey = "texture:main";
    request.artifacts.emplace_back("texture:main",ArtifactType::Texture,"texture","Main.ntexture",std::vector<uint8_t>{1});
    ASSERT_TRUE(AppendSpriteArtifacts(request,meta,32,16,"texture:main","Hero",error)) << error;
    ASSERT_EQ(request.artifacts.size(),2); EXPECT_EQ(request.primarySubAssetKey,"texture:main");
    EXPECT_EQ(request.artifacts.front().payload,std::vector<uint8_t>{1});
    ArtifactManifest manifest;manifest.dependencies=request.dependencies;
    EXPECT_TRUE(HasCurrentSpriteCompilerDependency(manifest,meta));
    for(auto& dependency:manifest.dependencies)if(dependency.value==SpriteCompilerDependency)dependency.hashOrVersion="1";
    EXPECT_FALSE(HasCurrentSpriteCompilerDependency(manifest,meta));
    manifest.dependencies.clear();EXPECT_FALSE(HasCurrentSpriteCompilerDependency(manifest,meta));
    meta.settings["TEXTURE_TYPE"]="default";EXPECT_TRUE(HasCurrentSpriteCompilerDependency(manifest,meta));
}

TEST(SpriteSheet, GridReslicingAndMetadataReorderingRetainPersistentIdentitiesAndNames)
{
    SpriteGridSettings settings; settings.cellWidth = 16; settings.cellHeight = 8;
    std::string error;
    auto slices = GenerateGridSpriteSlices(32,16,settings,{},error); ASSERT_TRUE(slices) << error;
    ASSERT_EQ(slices->size(),4); (*slices)[2].name = "Renamed frame";
    const auto identity = (*slices)[2].id;
    std::reverse(slices->begin(),slices->end());
    const auto encoded = SerializeSpriteSlices(*slices,error); ASSERT_FALSE(encoded.empty()) << error;
    const auto decoded = DeserializeSpriteSlices(encoded,error); ASSERT_TRUE(decoded) << error;
    const auto repeated = GenerateGridSpriteSlices(32,16,settings,*decoded,error); ASSERT_TRUE(repeated) << error;
    EXPECT_EQ((*repeated)[2].id,identity); EXPECT_EQ((*repeated)[2].name,"Renamed frame");
    auto meta = SheetMeta(); meta.settings[SpriteImportModeSetting] = "Multiple";
    meta.settings[SpriteSlicesSetting] = encoded;
    const auto built = BuildSpriteArtifacts(meta,32,16,"texture:main","Sheet",error); ASSERT_TRUE(built) << error;
    ASSERT_EQ(built->size(),4);
    for (const auto& sprite : *built)
    {
        EXPECT_EQ(sprite.textureAssetId,meta.id); EXPECT_EQ(sprite.sourceWidth,32); EXPECT_EQ(sprite.sourceHeight,16);
        EXPECT_EQ(sprite.SubAssetKey(),"sprite:"+sprite.id);
        EXPECT_TRUE(std::any_of(slices->begin(),slices->end(),[&](const auto& slice) {return slice.id == sprite.id && slice.name == sprite.name;}));
    }
}

TEST(SpriteSheet, GridOffsetsPaddingPartialCellsAndCountLimitsAreChecked)
{
    SpriteGridSettings settings; settings.cellWidth = settings.cellHeight = 4;
    settings.offsetX = settings.offsetY = 1; settings.paddingX = settings.paddingY = 1;
    std::string error;
    auto full = GenerateGridSpriteSlices(12,12,settings,{},error); ASSERT_TRUE(full); ASSERT_EQ(full->size(),4);
    EXPECT_EQ(full->back().x,6); EXPECT_EQ(full->back().y,6);
    settings.includePartialCells = true;
    auto partial = GenerateGridSpriteSlices(12,12,settings,{},error); ASSERT_TRUE(partial); ASSERT_EQ(partial->size(),9);
    EXPECT_EQ(partial->back().x,11); EXPECT_EQ(partial->back().width,1); EXPECT_EQ(partial->back().height,1);
    settings.cellWidth = 0; EXPECT_FALSE(GenerateGridSpriteSlices(12,12,settings,{},error));
    settings = {}; settings.cellWidth = settings.cellHeight = 1;
    EXPECT_FALSE(GenerateGridSpriteSlices(UINT32_MAX,UINT32_MAX,settings,{},error));
}

TEST(SpriteSheet, InvalidMetadataAndDuplicateKeysDoNotPartiallyAppendArtifacts)
{
    auto meta = SheetMeta(); meta.settings[SpriteImportModeSetting] = "multiple";
    std::string error;
    auto slices = GenerateGridSpriteSlices(32,16,{}, {},error); ASSERT_TRUE(slices); ASSERT_EQ(slices->size(),2);
    const auto valid = SerializeSpriteSlices(*slices,error); ASSERT_FALSE(valid.empty());
    ArtifactWriteRequest request; request.primarySubAssetKey = "texture:main";
    request.artifacts.emplace_back("texture:main",ArtifactType::Texture,"texture","Main.ntexture",std::vector<uint8_t>{7});
    for (const auto& invalid : {std::string("{bad"),std::string("{\"version\":3,\"sprites\":[]}")})
    {
        meta.settings[SpriteSlicesSetting] = invalid;
        EXPECT_FALSE(AppendSpriteArtifacts(request,meta,32,16,"texture:main","Sheet",error));
        EXPECT_EQ(request.artifacts.size(),1); EXPECT_FALSE(error.empty());
    }
    auto json = nlohmann::json::parse(valid);
    json["sprites"][1]["id"] = json["sprites"][0]["id"];
    meta.settings[SpriteSlicesSetting] = json.dump();
    EXPECT_FALSE(AppendSpriteArtifacts(request,meta,32,16,"texture:main","Sheet",error));
    EXPECT_EQ(request.artifacts.size(),1);
    meta.settings[SpriteSlicesSetting] = valid;
    EXPECT_FALSE(AppendSpriteArtifacts(request,meta,8,8,"texture:main","Sheet",error));
    EXPECT_EQ(request.artifacts.size(),1);
    ASSERT_TRUE(AppendSpriteArtifacts(request,meta,32,16,"texture:main","Sheet",error));
    ASSERT_EQ(request.artifacts.size(),3);
    EXPECT_FALSE(AppendSpriteArtifacts(request,meta,32,16,"texture:main","Sheet",error));
    EXPECT_EQ(request.artifacts.size(),3); EXPECT_EQ(request.primarySubAssetKey,"texture:main");
}

TEST(SpriteSheet, MultipleModeCanPublishNoSlicesWithoutReplacingTheTexture)
{
    auto meta = SheetMeta(); meta.settings[SpriteImportModeSetting] = "multiple";
    std::string error;
    const auto sprites = BuildSpriteArtifacts(meta,32,16,"texture:main","Sheet",error);
    ASSERT_TRUE(sprites); EXPECT_TRUE(sprites->empty());
    meta.settings[SpriteSlicesSetting] = SerializeSpriteSlices({},error);
    ASSERT_FALSE(meta.settings[SpriteSlicesSetting].empty());
    EXPECT_TRUE(BuildSpriteArtifacts(meta,32,16,"texture:main","Sheet",error)->empty());
    meta.settings[SpriteImportModeSetting] = "unsupported";
    EXPECT_FALSE(BuildSpriteArtifacts(meta,32,16,"texture:main","Sheet",error));
}
TEST(SpriteSheet, BorderedSlicesRoundTripAndResliceWithPersistentIdentity)
{
    std::string error;SpriteGridSettings grid;grid.cellWidth=grid.cellHeight=16;
    auto slices=GenerateGridSpriteSlices(32,16,grid,{},error);ASSERT_TRUE(slices);
    const auto legacy=SerializeSpriteSlices(*slices,error);EXPECT_EQ(nlohmann::json::parse(legacy)["version"],1);
    slices->front().border={2,3,4,5};const auto id=slices->front().id;
    const auto text=SerializeSpriteSlices(*slices,error);EXPECT_EQ(nlohmann::json::parse(text)["version"],2);
    const auto loaded=DeserializeSpriteSlices(text,error);ASSERT_TRUE(loaded)<<error;
    EXPECT_EQ(loaded->front().border,slices->front().border);
    const auto repeated=GenerateGridSpriteSlices(32,16,grid,*loaded,error);ASSERT_TRUE(repeated);
    EXPECT_EQ(repeated->front().id,id);EXPECT_EQ(repeated->front().border,slices->front().border);
    auto meta=SheetMeta();meta.settings[SpriteImportModeSetting]="multiple";meta.settings[SpriteSlicesSetting]=text;
    const auto artifacts=BuildSpriteArtifacts(meta,32,16,"texture:main","Sheet",error);ASSERT_TRUE(artifacts)<<error;
    EXPECT_EQ(artifacts->front().border,slices->front().border);
    auto invalid=nlohmann::json::parse(text);invalid["sprites"][0]["border"][0]=-1;
    EXPECT_FALSE(DeserializeSpriteSlices(invalid.dump(),error));
    invalid=nlohmann::json::parse(text);invalid["sprites"][0]["border"][0]=1.5;
    EXPECT_FALSE(DeserializeSpriteSlices(invalid.dump(),error));
    slices->front().border={UINT32_MAX,0,UINT32_MAX,0};EXPECT_TRUE(SerializeSpriteSlices(*slices,error).empty());
}

TEST(SpriteSheet, AlphaSlicingPreservesRegionsAndDistinguishesDiagonalPixels)
{
    std::vector<uint8_t> rgba(8*6*4);
    const auto pixel=[&](int x,int y,uint8_t alpha=255){rgba[(y*8+x)*4+3]=alpha;};
    for(int y=1;y<3;++y)for(int x=1;x<3;++x)pixel(x,y);
    for(int x=5;x<8;++x)pixel(x,4);
    pixel(4,0,20);
    std::string error;SpriteGridSettings defaults;
    auto slices=GenerateAlphaSpriteSlices(rgba,8,6,30,1,defaults,{},error);ASSERT_TRUE(slices)<<error;
    ASSERT_EQ(slices->size(),2);EXPECT_EQ(slices->front().x,1);EXPECT_EQ(slices->front().y,1);
    EXPECT_EQ(slices->front().width,2);EXPECT_EQ(slices->front().height,2);
    slices->front().name="Retained name";const auto id=slices->front().id;
    auto repeated=GenerateAlphaSpriteSlices(rgba,8,6,30,1,defaults,*slices,error);ASSERT_TRUE(repeated);
    EXPECT_EQ(repeated->front().id,id);EXPECT_EQ(repeated->front().name,"Retained name");
    const auto filtered=GenerateAlphaSpriteSlices(rgba,8,6,30,4,defaults,{},error);ASSERT_TRUE(filtered);EXPECT_EQ(filtered->size(),1);
    rgba.assign(2*2*4,0);rgba[3]=255;rgba[15]=255;
    const auto diagonal=GenerateAlphaSpriteSlices(rgba,2,2,0,1,defaults,{},error);ASSERT_TRUE(diagonal);EXPECT_EQ(diagonal->size(),2);
    EXPECT_FALSE(GenerateAlphaSpriteSlices(rgba,3,3,0,1,defaults,{},error));
    rgba.assign(16,0);const auto empty=GenerateAlphaSpriteSlices(rgba,2,2,0,1,defaults,{},error);ASSERT_TRUE(empty);EXPECT_TRUE(empty->empty());
}
}
