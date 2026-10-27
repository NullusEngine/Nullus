#include <gtest/gtest.h>
#include <algorithm>
#include <cstring>

#include <filesystem>
#include <fstream>
#include "Guid.h"
#include "Math/Vector4.h"
#include "Rendering/Assets/ShaderArtifact.h"
#include "Rendering/Resources/Loaders/ShaderLoader.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/MaterialInstance.h"

namespace
{
namespace R = NLS::Render;
R::Resources::ShaderReflection Layout(uint32_t offset, uint32_t binding)
{
    R::Resources::ShaderReflection reflection;
    reflection.constantBuffers.push_back({"MaterialProperties", R::ShaderCompiler::ShaderStage::Pixel, 2, binding,
        offset+16, {{"_Tint", R::Resources::UniformType::UNIFORM_FLOAT_VEC4, offset, 16, 1}}, R::RHI::ShaderStageMask::Fragment});
    reflection.properties.push_back({"_Tint", R::Resources::UniformType::UNIFORM_FLOAT_VEC4,
        R::Resources::ShaderResourceKind::Value, R::ShaderCompiler::ShaderStage::Pixel, 2, binding, -1, 1,
        offset, 16, "MaterialProperties", R::RHI::ShaderStageMask::Fragment});
    return reflection;
}
}

TEST(MaterialRuntimeTests, ArtifactRoundTripKeepsTargetVariantLayoutsAndLogicalDefaults)
{
    R::Assets::ShaderArtifact artifact;
    artifact.sourcePath = "Assets/Tint.shader";
    artifact.subAssetKey = "shader:Tint";
    artifact.reflection = Layout(0,0);
    artifact.variantReflections = {
        {R::ShaderCompiler::ShaderTargetPlatform::DXIL, 0, Layout(0,0)},
        {R::ShaderCompiler::ShaderTargetPlatform::DXIL, 77, Layout(32,3)},
        {R::ShaderCompiler::ShaderTargetPlatform::SPIRV, 77, Layout(16,2)}};
    R::Resources::MaterialParameterDesc tint;
    tint.id = R::Resources::MakeMaterialParameterId("tint-guid", "_Tint");
    tint.name = "_Tint";
    tint.displayName = "Tint";
    tint.type = R::Resources::UniformType::UNIFORM_FLOAT_VEC4;
    tint.defaultValue = std::array<float,4>{0.2f,0.4f,0.6f,1.0f};
    artifact.parameterSchema.parameters.push_back(tint);
    const auto bytes = R::Assets::SerializeShaderArtifact(artifact);
    const auto decoded = R::Assets::DeserializeShaderArtifact(bytes);
    ASSERT_TRUE(decoded);
    ASSERT_EQ(decoded->variantReflections.size(),3);
    EXPECT_EQ(decoded->variantReflections[1].reflection.constantBuffers[0].members[0].byteOffset,32);
    EXPECT_EQ(decoded->variantReflections[2].reflection.constantBuffers[0].bindingIndex,2);
    ASSERT_EQ(decoded->parameterSchema.parameters.size(),1);
    EXPECT_EQ(decoded->parameterSchema.parameters[0].id,tint.id);
    EXPECT_EQ((std::get<std::array<float,4>>(decoded->parameterSchema.parameters[0].defaultValue)[2]),0.6f);
}

TEST(MaterialRuntimeTests, ShaderSelectsPhysicalVariantAndKeepsPriorSnapshotAfterReload)
{
    const auto root = std::filesystem::temp_directory_path()/ ("nullus_material_runtime_"+NLS::Guid::New().ToString());
    const auto path = root/"Library"/"Artifacts"/"Tint"/std::string(64,'c');
    std::filesystem::create_directories(path.parent_path());
    R::Assets::ShaderArtifact artifact;
    artifact.sourcePath = "Assets/Tint.shader";
    artifact.subAssetKey = "shader:Tint";
    artifact.reflection = Layout(0,0);
    artifact.variantReflections = {{R::ShaderCompiler::ShaderTargetPlatform::DXIL,0,Layout(0,0)},
        {R::ShaderCompiler::ShaderTargetPlatform::DXIL,77,Layout(32,3)}};
    auto bytes = R::Assets::SerializeShaderArtifact(artifact);
    std::ofstream file(path,std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size()); file.close();
    auto* shader = R::Resources::Loaders::ShaderLoader::Create(path.string());
    ASSERT_NE(shader,nullptr);
    const auto old = shader->GetReflectionSnapshot(R::ShaderCompiler::ShaderTargetPlatform::DXIL,77);
    ASSERT_NE(old,nullptr);
    EXPECT_EQ(old->constantBuffers[0].bindingIndex,3);
    EXPECT_EQ(shader->GetReflectionSnapshot(R::ShaderCompiler::ShaderTargetPlatform::DXIL,99),nullptr);
    EXPECT_EQ(shader->GetReflectionSnapshot(R::ShaderCompiler::ShaderTargetPlatform::SPIRV,77),nullptr);
    R::Resources::Loaders::ShaderLoader::Destroy(shader);
    EXPECT_EQ(old->constantBuffers[0].members[0].byteOffset,32);
    std::filesystem::remove_all(root);
}

TEST(MaterialRuntimeTests, ConstantCaptureUsesPhysicalOffsetsAndRejectsUnencodedArrays)
{
    R::Resources::MaterialParameterBlock parameters;
    parameters.Set("_Tint",NLS::Maths::Vector4(0.2f,0.4f,0.6f,1));
    auto layout = Layout(32,3).constantBuffers[0];
    std::vector<uint8_t> bytes;
    std::string error;
    ASSERT_TRUE(R::Resources::CaptureMaterialConstantBuffer(layout,parameters,bytes,error)) << error;
    EXPECT_EQ(bytes.size(),48);
    EXPECT_TRUE(std::all_of(bytes.begin(),bytes.begin()+32,[](uint8_t value){return value==0;}));
    float red = 0;
    std::memcpy(&red,bytes.data()+32,sizeof(red));
    EXPECT_FLOAT_EQ(red,0.2f);
    layout.members[0].arraySize = 2;
    EXPECT_FALSE(R::Resources::CaptureMaterialConstantBuffer(layout,parameters,bytes,error));
    EXPECT_NE(error.find("array"),std::string::npos);
}

TEST(MaterialRuntimeTests, DynamicOverridesAreIndependentAndSurviveRebaseWithStableMaterialIdentity)
{
    auto* shader = R::Resources::Shader::CreateForTesting("Dynamic");
    shader->SetReflectionForTesting(Layout(0,0));
    {
        R::Resources::Material base(shader);
        base.Set("_Tint",NLS::Maths::Vector4(1,0,0,1));
        R::Resources::MaterialInstance first(base), second(base);
        const auto id = first.GetMaterial().GetInstanceId();
        std::string error;
        ASSERT_TRUE(first.SetParameter("_Tint",std::array<float,4>{0,1,0,1},error)) << error;
        EXPECT_FLOAT_EQ(base.Get<NLS::Maths::Vector4>("_Tint").x,1);
        EXPECT_FLOAT_EQ(second.GetMaterial().Get<NLS::Maths::Vector4>("_Tint").x,1);
        base.Set("_Tint",NLS::Maths::Vector4(0,0,1,1));
        ASSERT_TRUE(first.Rebase(base,error));
        EXPECT_EQ(first.GetMaterial().GetInstanceId(),id);
        EXPECT_FLOAT_EQ(first.GetMaterial().Get<NLS::Maths::Vector4>("_Tint").y,1);
        ASSERT_TRUE(first.ClearParameter("_Tint",error));
        EXPECT_FLOAT_EQ(first.GetMaterial().Get<NLS::Maths::Vector4>("_Tint").z,1);
        R::Resources::MaterialInstance grandchild(first.GetMaterial());
        base.Set("_Tint",NLS::Maths::Vector4(0.5f,0.25f,0.75f,1));
        R::Resources::MaterialInstance::NotifyBaseChanged(base);
        EXPECT_FLOAT_EQ(grandchild.GetMaterial().Get<NLS::Maths::Vector4>("_Tint").x,0.5f);
        EXPECT_TRUE(first.GetMaterial().IsDynamicInstance());
        EXPECT_FALSE(first.SetParameter("_Tint",2.0f,error));
    }
    R::Resources::Shader::DestroyForTesting(shader);
}
