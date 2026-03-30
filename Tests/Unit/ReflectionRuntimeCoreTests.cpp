#include "ReflectionRuntimeTestFixture.h"

#include <string>
#include <vector>

namespace
{
using NLS::Tests::Reflection::ExpectReflectedType;
using NLS::Tests::Reflection::TypeExpectation;
}

TEST_F(ReflectionRuntimeTestFixture, RegistersBaseReflectionTypes)
{
    const std::vector<TypeExpectation> expectations = {
        {"NLS::meta::MetaParserFieldMethodSample", {"GetValue", "SetValue"}, {}, {"value"}, ""},
        {"NLS::meta::MetaProperty", {}, {}, {}, ""},
        {"NLS::meta::ReflectionObjectSample", {"OnSerialize"}, {}, {}, ""},
        {"NLS::meta::TestObject", {"OnSerialize", "OnDeserialize"}, {}, {}, ""},
    };

    for (const TypeExpectation& expectation : expectations)
        ExpectReflectedType(expectation);
}

TEST_F(ReflectionRuntimeTestFixture, RegistersExternalReflectionAndSerializationTypes)
{
    const std::vector<TypeExpectation> expectations = {
        {"NLS::Maths::Vector3", {"Length", "Normalised"}, {"Dot", "Cross"}, {"x", "y", "z"}, ""},
        {"NLS::Maths::Quaternion", {}, {}, {"x", "y", "z", "w"}, ""},
        {"NLS::meta::PrivateReflectionExternalSample", {"GetHiddenValue"}, {}, {"m_hiddenValue"}, ""},
        {"NLS::Engine::Serialize::SerializedComponentData", {}, {}, {"type", "data"}, ""},
        {"NLS::Engine::Serialize::SerializedActorData", {}, {}, {"name", "tag", "active", "worldID", "parent", "components"}, ""},
        {"NLS::Engine::Serialize::SerializedSceneData", {}, {}, {"version", "actors"}, ""}
    };

    for (const TypeExpectation& expectation : expectations)
        ExpectReflectedType(expectation);
}

TEST_F(ReflectionRuntimeTestFixture, RegistersRenderEnumAndStructReflectionTypes)
{
    const std::vector<TypeExpectation> expectations = {
        {"NLS::Render::Settings::EProjectionMode", {}, {}, {}, ""},
        {"NLS::Render::Settings::ELightType", {}, {}, {}, ""},
        {"NLS::Render::Geometry::BoundingSphere", {}, {}, {"position", "radius"}, ""},
        {"NLS::Engine::Components::MeshRenderer::EFrustumBehaviour", {}, {}, {}, ""}
    };

    for (const TypeExpectation& expectation : expectations)
        ExpectReflectedType(expectation);
}
