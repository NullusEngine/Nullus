#include <gtest/gtest.h>
#include <fstream>
#include "Guid.h"
#include "Rendering/Assets/MaterialAsset.h"
#include "Assets/NativeArtifactContainer.h"

namespace
{
using namespace NLS::Render::Assets;
using namespace NLS::Render::Resources;
MaterialAsset Root()
{
    MaterialAsset asset;
    asset.identity = {"root", "material:main", "Assets/Root.mat"};
    asset.shader = {"shader","shader:Main","Assets/Main.shader"};
    asset.parameters.push_back({1,"_Value",UniformType::UNIFORM_FLOAT,0.1f});
    asset.staticParameters["FEATURE"] = true;
    return asset;
}
}

TEST(MaterialAssetTests, ChildOverrideAndExplicitFalseWinWithoutChangingParent)
{
    const auto root = Root();
    MaterialAsset middle;
    middle.identity = {"middle","material:main","Assets/Middle.mat"}; middle.parent = root.identity;
    middle.parameters.push_back({1,"_Value",UniformType::UNIFORM_FLOAT,0.5f});
    MaterialAsset leaf;
    leaf.identity = {"leaf","material:main","Assets/Leaf.mat"}; leaf.parent = middle.identity;
    leaf.staticParameters["FEATURE"] = false;
    std::string error;
    auto provider = [&](const MaterialAssetReference& ref) -> std::optional<MaterialAsset>
        { if (ref.guid == "root") return root; if (ref.guid == "middle") return middle; return std::nullopt; };
    const auto result = MaterialAssetResolver::Resolve(leaf,provider,error);
    ASSERT_TRUE(result) << error;
    EXPECT_FLOAT_EQ(std::get<float>(result->parameters.at("_Value").value),0.5f);
    EXPECT_FALSE(result->staticParameters.at("FEATURE"));
    EXPECT_TRUE(root.staticParameters.at("FEATURE"));
    EXPECT_EQ(result->ancestry.size(),3);
}

TEST(MaterialAssetTests, RejectsMissingParentsCyclesAndAmbiguousShaderOwnership)
{
    MaterialAsset child;
    child.identity = {"child",{},"Assets/Child.mat"}; child.parent = {"parent",{},"Assets/Parent.mat"};
    std::string error;
    EXPECT_FALSE(MaterialAssetResolver::Resolve(child,{},error));
    EXPECT_NE(error.find("Missing parent"),std::string::npos);
    const auto cycle = MaterialAssetResolver::Resolve(child,[&](const auto& ref) -> std::optional<MaterialAsset>
    { auto parent = child; parent.identity = ref; parent.parent = child.identity; return parent; },error);
    EXPECT_FALSE(cycle);
    EXPECT_NE(error.find("cycle"),std::string::npos);
    child.shader = Root().shader;
    EXPECT_FALSE(child.Validate(error));
}

TEST(MaterialAssetTests, StableIdsSurviveParameterRenameAndUnknownValuesRemainRecoverable)
{
    auto asset = Root();
    asset.parameters.push_back({2,"Removed",UniformType::UNIFORM_FLOAT,0.9f});
    MaterialParameterSchema schema;
    MaterialParameterDesc renamed;
    renamed.id = 1; renamed.name = "Renamed"; renamed.defaultValue = 0.0f;
    schema.parameters.push_back(renamed);
    std::string error;
    const auto resolved = MaterialAssetResolver::Resolve(asset,{},error,&schema);
    ASSERT_TRUE(resolved) << error;
    EXPECT_FLOAT_EQ(std::get<float>(resolved->parameters.at("Renamed").value),0.1f);
    EXPECT_EQ(resolved->orphanParameters.size(),1);
}

TEST(MaterialAssetTests, NativeAssetSaveReplacesOnlySourceAndPreservesInheritance)
{
    auto asset = Root(); asset.shader = {}; asset.parent = Root().identity;
    asset.identity = {NLS::Guid::New().ToString(),"material:main","Assets/Child.mat"};
    const auto path = std::filesystem::temp_directory_path()/("NullusMaterial_"+NLS::Guid::New().ToString()+".mat");
    std::string error;
    ASSERT_TRUE(SaveMaterialAsset(asset,path,error)) << error;
    asset.staticParameters["FEATURE"] = false;
    ASSERT_TRUE(SaveMaterialAsset(asset,path,error)) << error;
    std::ifstream input(path,std::ios::binary);
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input),{}};
    const auto container = NLS::Core::Assets::ReadNativeArtifactContainer(bytes,NLS::Core::Assets::ArtifactType::Material,1);
    ASSERT_TRUE(container);
    const auto decoded = DeserializeMaterialAsset(std::string_view(reinterpret_cast<const char*>(container->payload.data()),container->payload.size()),error);
    ASSERT_TRUE(decoded) << error;
    EXPECT_EQ(decoded->parent,asset.parent);
    EXPECT_FALSE(decoded->staticParameters.at("FEATURE"));
    NLS::Render::RHI::SamplerDesc sampler;sampler.wrapU=NLS::Render::RHI::TextureWrap::ClampToBorder;
    sampler.wrapV=NLS::Render::RHI::TextureWrap::MirrorRepeat;sampler.maxAnisotropy=8;sampler.mipLodBias=-1.5f;
    sampler.minLod=1;sampler.maxLod=6;sampler.compareEnabled=true;sampler.compareFunc=NLS::Render::Settings::EComparaisonAlgorithm::GREATER;
    sampler.borderColor={1,0.5f,0.25f,1};asset.samplerOverrides["sampler_Map"]=sampler;
    const auto roundtrip=DeserializeMaterialAsset(SerializeMaterialAsset(asset),error);ASSERT_TRUE(roundtrip)<<error;
    const auto& restored=roundtrip->samplerOverrides.at("sampler_Map");
    EXPECT_EQ(restored.wrapU,sampler.wrapU);EXPECT_EQ(restored.maxAnisotropy,8);EXPECT_EQ(restored.compareFunc,sampler.compareFunc);
    EXPECT_FLOAT_EQ(restored.mipLodBias,-1.5f);EXPECT_EQ(restored.borderColor,sampler.borderColor);
    EXPECT_FALSE(std::filesystem::exists(path.string()+".meta"));
    input.close(); std::filesystem::remove(path);
}
