#include "ReflectionTestUtils.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace
{
using NLS::Tests::Reflection::ExpectContains;
using NLS::Tests::Reflection::ExpectNotContains;
using NLS::Tests::Reflection::ReadAllText;
}

TEST(MetaParserGenerationDataTests, GeneratesExpectedRenderEnumAndStructBindings)
{
    const std::filesystem::path projectionModeSource = std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Rendering/Gen/Settings/EProjectionMode.generated.cpp";
    const std::filesystem::path lightTypeSource = std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Rendering/Gen/Settings/ELightType.generated.cpp";
    const std::filesystem::path boundingSphereSource = std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Rendering/Gen/Geometry/BoundingSphere.generated.cpp";
    const std::string projectionModeText = ReadAllText(projectionModeSource);
    const std::string lightTypeText = ReadAllText(lightTypeSource);
    const std::string boundingSphereText = ReadAllText(boundingSphereSource);

    ExpectContains(projectionModeText, "type.SetEnum<NLS::Render::Settings::EProjectionMode>");
    ExpectContains(projectionModeText, "\"PERSPECTIVE\"");
    ExpectContains(lightTypeText, "type.SetEnum<NLS::Render::Settings::ELightType>");
    ExpectContains(lightTypeText, "\"DIRECTIONAL\"");
    ExpectContains(boundingSphereText, "AddField<NLS::Render::Geometry::BoundingSphere, NLS::Maths::Vector3>(\"position\"");
    ExpectContains(boundingSphereText, "AddField<NLS::Render::Geometry::BoundingSphere, float>(\"radius\"");
}

TEST(MetaParserGenerationDataTests, GeneratesExpectedExternalAndPrivateReflectionBindings)
{
    const std::filesystem::path mathSource = std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Math/ExternalReflection.h";
    const std::filesystem::path privateSource = std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Base/Reflection/PrivateReflectionExternalSample.h";
    const std::filesystem::path mathMetaSource = std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Math/Gen/MetaGenerated.cpp";
    const std::filesystem::path engineMetaSource = std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Gen/MetaGenerated.cpp";
    const std::string mathText = ReadAllText(mathSource);
    const std::string privateText = ReadAllText(privateSource);
    const std::string mathMetaText = ReadAllText(mathMetaSource);
    const std::string engineMetaText = ReadAllText(engineMetaSource);

    ExpectContains(mathText, "NLS_META_EXTERNAL_BEGIN(NLS::Maths::Vector3)");
    ExpectContains(mathText, "NLS_META_EXTERNAL_STATIC_METHOD(");
    ExpectContains(mathText, "\"Dot\"");
    ExpectContains(mathText, "NLS_META_EXTERNAL_BEGIN(NLS::Maths::Quaternion)");
    ExpectContains(privateText, "PrivateReflectionExternalSampleReflectionAccess");
    ExpectContains(privateText, "\"m_hiddenValue\"");
    ExpectContains(privateText, "\"GetHiddenValue\"");
    ExpectNotContains(mathMetaText, "ExternalReflection.generated.cpp");
    ExpectNotContains(engineMetaText, "SceneSerializationData.generated.cpp");
}
