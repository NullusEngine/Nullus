#include "ReflectionTestUtils.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace
{
using NLS::Tests::Reflection::ExpectContains;
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
    const std::filesystem::path mathSource = std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Math/Gen/ExternalReflection.generated.cpp";
    const std::filesystem::path privateSource = std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Base/Gen/Reflection/PrivateReflectionExternalSample.generated.cpp";
    const std::filesystem::path serializeSource = std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Gen/Serialize/SceneSerializationData.generated.cpp";
    const std::string mathText = ReadAllText(mathSource);
    const std::string privateText = ReadAllText(privateSource);
    const std::string serializeText = ReadAllText(serializeSource);

    ExpectContains(mathText, "AllocateType(\"NLS::Maths::Vector3\")");
    ExpectContains(mathText, "AddStaticMethod<NLS::Maths::Vector3>(\"Dot\"");
    ExpectContains(mathText, "AllocateType(\"NLS::Maths::Quaternion\")");
    ExpectContains(privateText, "PrivateAccess_NLS__meta__PrivateReflectionExternalSample");
    ExpectContains(privateText, "AddField<NLS::meta::PrivateReflectionExternalSample, int>(\"m_hiddenValue\"");
    ExpectContains(privateText, "AddMethod(\"GetHiddenValue\"");
    ExpectContains(serializeText, "AllocateType(\"NLS::Engine::Serialize::SerializedSceneData\")");
    ExpectContains(serializeText, "AddField<NLS::Engine::Serialize::SerializedSceneData, NLS::Array<NLS::Engine::Serialize::SerializedActorData>>(\"actors\"");
}
