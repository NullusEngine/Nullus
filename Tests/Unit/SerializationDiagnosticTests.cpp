#include <gtest/gtest.h>

#include "Serialize/ObjectGraphInstantiator.h"
#include "Serialize/ObjectGraphDocument.h"
#include "Serialize/SerializationDiagnostic.h"

TEST(SerializationDiagnosticTests, DiagnosticStoresCodeSeverityAndMessage)
{
    NLS::Engine::Serialize::SerializationDiagnostic diagnostic(
        NLS::Engine::Serialize::SerializationDiagnosticCode::InvalidGuid,
        NLS::Engine::Serialize::SerializationDiagnosticSeverity::Error,
        "Invalid object id");

    EXPECT_EQ(diagnostic.GetCode(), NLS::Engine::Serialize::SerializationDiagnosticCode::InvalidGuid);
    EXPECT_EQ(diagnostic.GetSeverity(), NLS::Engine::Serialize::SerializationDiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.GetMessage(), "Invalid object id");
    EXPECT_TRUE(diagnostic.IsError());
}

TEST(SerializationDiagnosticTests, DiagnosticListReportsErrors)
{
    NLS::Engine::Serialize::SerializationDiagnosticList diagnostics;

    EXPECT_FALSE(diagnostics.HasErrors());

    diagnostics.Add({
        NLS::Engine::Serialize::SerializationDiagnosticCode::MissingAsset,
        NLS::Engine::Serialize::SerializationDiagnosticSeverity::Warning,
        "Missing editor asset"
    });
    EXPECT_FALSE(diagnostics.HasErrors());

    diagnostics.Add({
        NLS::Engine::Serialize::SerializationDiagnosticCode::DuplicateObjectId,
        NLS::Engine::Serialize::SerializationDiagnosticSeverity::Error,
        "Duplicate object id"
    });
    EXPECT_TRUE(diagnostics.HasErrors());
    EXPECT_EQ(diagnostics.GetItems().size(), 2u);
}

namespace
{
NLS::Engine::Serialize::ObjectId MakeDiagnosticObjectId(const char* label)
{
    return NLS::Engine::Serialize::ObjectId(NLS::Guid::NewDeterministic(label));
}

bool HasDiagnostic(
    const NLS::Engine::Serialize::SerializationDiagnosticList& diagnostics,
    NLS::Engine::Serialize::SerializationDiagnosticCode code,
    NLS::Engine::Serialize::SerializationDiagnosticSeverity severity)
{
    for (const auto& diagnostic : diagnostics.GetItems())
    {
        if (diagnostic.GetCode() == code && diagnostic.GetSeverity() == severity)
            return true;
    }
    return false;
}

NLS::Engine::Serialize::ObjectGraphDocument MakeUnknownTypeSceneDocument()
{
    using namespace NLS::Engine::Serialize;

    ObjectGraphDocument document;
    document.documentId = NLS::Guid::NewDeterministic("UnknownType.Document");
    const auto sceneId = MakeDiagnosticObjectId("UnknownType.Scene");
    const auto unknownId = MakeDiagnosticObjectId("UnknownType.Object");
    document.root = sceneId;

    ObjectRecord scene;
    scene.id = sceneId;
    scene.typeName = "NLS::Engine::SceneSystem::Scene";
    scene.properties.push_back({"gameObjects", PropertyValue::Array({PropertyValue::OwnedReference(unknownId)})});

    ObjectRecord unknown;
    unknown.id = unknownId;
    unknown.typeName = "Plugin.UnknownGameObject";
    unknown.debugName = "Unknown Plugin Object";
    unknown.properties.push_back({"raw", PropertyValue::String("preserve me")});

    document.objects.push_back(std::move(scene));
    document.objects.push_back(std::move(unknown));
    return document;
}
}

TEST(SerializationDiagnosticTests, EditorLoadPolicyPreservesUnknownTypesAsWarnings)
{
    using namespace NLS::Engine::Serialize;

    LoadPolicy policy;
    policy.unknownTypePolicy = UnknownTypePolicy::Preserve;

    const auto result = ObjectGraphInstantiator::AnalyzeDocument(MakeUnknownTypeSceneDocument(), policy);

    EXPECT_FALSE(result.diagnostics.HasErrors());
    EXPECT_TRUE(HasDiagnostic(
        result.diagnostics,
        SerializationDiagnosticCode::UnknownType,
        SerializationDiagnosticSeverity::Warning));
}

TEST(SerializationDiagnosticTests, RuntimeLoadPolicyFailsUnknownTypes)
{
    using namespace NLS::Engine::Serialize;

    LoadPolicy policy;
    policy.unknownTypePolicy = UnknownTypePolicy::Fail;

    const auto result = ObjectGraphInstantiator::AnalyzeDocument(MakeUnknownTypeSceneDocument(), policy);

    EXPECT_TRUE(result.diagnostics.HasErrors());
    EXPECT_TRUE(HasDiagnostic(
        result.diagnostics,
        SerializationDiagnosticCode::UnknownType,
        SerializationDiagnosticSeverity::Error));
}

TEST(SerializationDiagnosticTests, SceneInstantiationWithRuntimePolicyRejectsUnknownTypes)
{
    using namespace NLS::Engine::Serialize;

    LoadPolicy policy;
    policy.unknownTypePolicy = UnknownTypePolicy::Fail;

    const auto result = ObjectGraphInstantiator::InstantiateScene(MakeUnknownTypeSceneDocument(), policy);

    EXPECT_EQ(result.scene, nullptr);
    EXPECT_TRUE(result.diagnostics.HasErrors());
    EXPECT_TRUE(HasDiagnostic(
        result.diagnostics,
        SerializationDiagnosticCode::UnknownType,
        SerializationDiagnosticSeverity::Error));
}
