#include <gtest/gtest.h>

#include <algorithm>

#include "Assets/EditorAssetDatabase.h"
#include "Assets/PrefabEditorWorkflow.h"
#include "Components/LightComponent.h"
#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "GameObject.h"
#include "Guid.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/Mesh.h"
#include "SceneSystem/Scene.h"
#include "Serialize/ObjectGraphReader.h"

namespace
{
bool HasDiagnosticCode(
    const NLS::Editor::Assets::PrefabEditorOperationResult& result,
    const std::string& code)
{
    for (const auto& diagnostic : result.diagnostics)
    {
        if (diagnostic.code == code)
            return true;
    }
    return false;
}

bool HasCommand(
    const std::vector<NLS::Editor::Assets::EditorAssetCommandDescriptor>& commands,
    const std::string& commandId,
    bool enabled)
{
    return std::any_of(
        commands.begin(),
        commands.end(),
        [&commandId, enabled](const NLS::Editor::Assets::EditorAssetCommandDescriptor& command)
        {
            return command.commandId == commandId && command.enabled == enabled;
        });
}

const NLS::Editor::Assets::PrefabOverrideRecord* FindOverride(
    const std::vector<NLS::Editor::Assets::PrefabOverrideRecord>& overrides,
    NLS::Engine::Serialize::PatchOperationType type,
    const std::string& property)
{
    for (const auto& overrideRecord : overrides)
    {
        if (overrideRecord.patch.type == type && overrideRecord.patch.property == property)
            return &overrideRecord;
    }
    return nullptr;
}

const NLS::Engine::Serialize::PropertyRecord* FindProperty(
    const NLS::Engine::Serialize::ObjectRecord& record,
    const char* name)
{
    for (const auto& property : record.properties)
    {
        if (property.name == name)
            return &property;
    }
    return nullptr;
}

const NLS::Engine::Serialize::PropertyValue* FindObjectProperty(
    const NLS::Engine::Serialize::PropertyValue& value,
    const char* name)
{
    if (value.GetKind() != NLS::Engine::Serialize::PropertyValue::Kind::Object)
        return nullptr;

    for (const auto& property : value.GetObject())
    {
        if (property.first == name)
            return &property.second;
    }
    return nullptr;
}

const NLS::Engine::Serialize::ObjectRecord* FindObjectRecord(
    const NLS::Engine::Serialize::ObjectGraphDocument& graph,
    const NLS::Engine::Serialize::ObjectId& id)
{
    for (const auto& record : graph.objects)
    {
        if (record.id == id)
            return &record;
    }
    return nullptr;
}

std::vector<NLS::Engine::Serialize::ObjectId> ReadOwnedReferences(
    const NLS::Engine::Serialize::ObjectRecord& record,
    const char* name)
{
    std::vector<NLS::Engine::Serialize::ObjectId> ids;
    const auto* property = FindProperty(record, name);
    if (!property || property->value.GetKind() != NLS::Engine::Serialize::PropertyValue::Kind::Array)
        return ids;

    for (const auto& value : property->value.GetArray())
    {
        if (value.GetKind() == NLS::Engine::Serialize::PropertyValue::Kind::OwnedReference)
            ids.push_back(value.GetObjectId());
    }
    return ids;
}

std::string ReadStringProperty(
    const NLS::Engine::Serialize::ObjectRecord& record,
    const char* name)
{
    const auto* property = FindProperty(record, name);
    if (!property || property->value.GetKind() != NLS::Engine::Serialize::PropertyValue::Kind::String)
        return {};
    return property->value.GetString();
}

const NLS::Engine::Serialize::PropertyRecord* FindRootProperty(
    const NLS::Engine::Assets::PrefabArtifact& artifact,
    const char* name)
{
    const auto* rootRecord = FindObjectRecord(artifact.graph, artifact.graph.root);
    if (!rootRecord)
        return nullptr;
    return FindProperty(*rootRecord, name);
}

const NLS::Engine::Serialize::ObjectRecord* FindObjectRecord(
    const NLS::Engine::Serialize::ObjectGraphDocument& graph,
    const std::string& debugName,
    const std::string& typeName)
{
    for (const auto& record : graph.objects)
    {
        if (record.debugName == debugName && record.typeName == typeName)
            return &record;
    }
    return nullptr;
}

NLS::Engine::GameObject* FindChildByName(NLS::Engine::GameObject& root, const std::string& name)
{
    for (auto* child : root.GetChildren())
    {
        if (child && child->GetName() == name)
            return child;
    }
    return nullptr;
}

size_t CountChildrenByName(NLS::Engine::GameObject& root, const std::string& name)
{
    return static_cast<size_t>(std::count_if(
        root.GetChildren().begin(),
        root.GetChildren().end(),
        [&name](const NLS::Engine::GameObject* child)
        {
            return child != nullptr && child->GetName() == name;
        }));
}

NLS::Engine::GameObject* FindDescendantByName(NLS::Engine::GameObject& root, const std::string& name)
{
    if (root.GetName() == name)
        return &root;

    for (auto* child : root.GetChildren())
    {
        if (!child)
            continue;
        if (auto* found = FindDescendantByName(*child, name))
            return found;
    }
    return nullptr;
}

template <typename T>
NLS::Engine::Serialize::PPtr<T> MakePPtr(const NLS::Engine::Serialize::ObjectIdentifier& identifier)
{
    return NLS::Engine::Serialize::PPtr<T>(
        NLS::Engine::Serialize::PersistentManager::Instance().ObjectIdentifierToInstanceID(identifier));
}
}

TEST(PrefabEditorWorkflowTests, CreatesPrefabFromSelectedRootWithStableObjectIds)
{
    NLS::Engine::GameObject root("Workbench", "Prop");
    root.AddComponent<NLS::Engine::Components::LightComponent>()->SetIntensity(4.0f);

    NLS::Editor::Assets::PrefabEditorWorkflow workflow;
    const auto result = workflow.CreatePrefabFromSelection({
        &root,
        {},
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("81818181-8181-4181-8181-818181818181")),
        "Assets/Prefabs/Workbench.prefab"
    });

    ASSERT_EQ(result.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);
    EXPECT_TRUE(result.diagnostics.empty());
    ASSERT_TRUE(result.artifact.has_value());
    ASSERT_FALSE(result.prefabSourceText.empty());
    EXPECT_EQ(result.artifact->assetId.GetGuid().ToString(), "81818181-8181-4181-8181-818181818181");
    EXPECT_EQ(result.artifact->graph.format, "Nullus.ObjectGraph.Prefab");
    EXPECT_TRUE(result.createdPrefabAssetId.IsValid());

    const auto parsed = NLS::Engine::Serialize::ObjectGraphReader::Read(result.prefabSourceText);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->root, result.artifact->graph.root);

    const auto second = workflow.CreatePrefabFromSelection({
        &root,
        {},
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("81818181-8181-4181-8181-818181818181")),
        "Assets/Prefabs/Workbench.prefab"
    });
    ASSERT_TRUE(second.artifact.has_value());
    EXPECT_EQ(second.artifact->graph.root, result.artifact->graph.root);
}

TEST(PrefabEditorWorkflowTests, RejectsMultiRootSelectionWithoutWrapperPolicy)
{
    NLS::Engine::GameObject first("First");
    NLS::Engine::GameObject second("Second");

    NLS::Editor::Assets::PrefabEditorWorkflow workflow;
    const auto result = workflow.CreatePrefabFromSelection({
        &first,
        {&first, &second},
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("82828282-8282-4282-8282-828282828282")),
        "Assets/Prefabs/Multi.prefab"
    });

    EXPECT_EQ(result.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Rejected);
    EXPECT_TRUE(HasDiagnosticCode(result, "prefab-multi-root-selection"));
    EXPECT_FALSE(result.artifact.has_value());
}

TEST(PrefabEditorWorkflowTests, InstantiatesPrefabAndStoresSceneConnection)
{
    NLS::Engine::GameObject root("Crate", "Prop");
    auto artifact = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &root,
        {},
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("83838383-8383-4383-8383-838383838383")),
        "Assets/Prefabs/Crate.prefab"
    }).artifact;
    ASSERT_TRUE(artifact.has_value());

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::PrefabEditorWorkflow workflow;
    auto result = workflow.InstantiatePrefab({
        &*artifact,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("83838383-8383-4383-8383-838383838383")),
        "prefab:Crate"
    }, scene);

    ASSERT_EQ(result.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);
    ASSERT_TRUE(result.instance.has_value());
    ASSERT_NE(result.instance->instanceRoot, nullptr);
    EXPECT_EQ(result.instance->prefabAssetId.GetGuid().ToString(), "83838383-8383-4383-8383-838383838383");
    EXPECT_EQ(result.instance->prefabSubAssetKey, "prefab:Crate");
    EXPECT_EQ(result.instance->instanceRoot->GetName(), "Crate");
    EXPECT_FALSE(result.instance->sourceToInstance.empty());
    EXPECT_TRUE(result.instance->localPatches.empty());
    ASSERT_EQ(scene.GetGameObjects().size(), 1u);
    EXPECT_EQ(scene.GetGameObjects()[0], result.instance->instanceRoot);
}

TEST(PrefabEditorWorkflowTests, ConnectExistingPrefabInstanceCompletesPreviewRootStructure)
{
    const std::string meshPath = "Library/Artifacts/PreviewCommitHero/db7ffec2d25e80c7b075bc30a992e27e5f392f809146715c3cdf514a6fba8beb";
    const std::string materialPath = "Library/Artifacts/PreviewCommitHero/8ca977f3a8a054ff6767e381b334be9e47456f725e02f84e11a3b5b1f3f4218b";
    const auto meshId = NLS::Engine::Serialize::AssetId(
        NLS::Guid::Parse("83838383-8383-4383-8383-838383838385"));
    const auto materialId = NLS::Engine::Serialize::AssetId(
        NLS::Guid::Parse("83838383-8383-4383-8383-838383838386"));

    NLS::Engine::GameObject sourceRoot("PreviewCommitHero", "Model");
    auto* sourceMeshFilter = sourceRoot.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* sourceMeshRenderer = sourceRoot.AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(sourceMeshFilter, nullptr);
    ASSERT_NE(sourceMeshRenderer, nullptr);
    sourceMeshFilter->SetMeshReference(MakePPtr<NLS::Render::Resources::Mesh>(
        NLS::Engine::Serialize::ObjectIdentifier::Asset(
            meshId,
            NLS::Engine::Serialize::MakeLocalIdentifierInFile(meshId.GetGuid(), meshPath),
            meshPath)));
    sourceMeshRenderer->SetMaterialReferences({
        MakePPtr<NLS::Render::Resources::Material>(
            NLS::Engine::Serialize::ObjectIdentifier::Asset(
                materialId,
                NLS::Engine::Serialize::MakeLocalIdentifierInFile(materialId.GetGuid(), materialPath),
                materialPath))
    });
    auto* sourceChild = new NLS::Engine::GameObject("PreviewCommitChild", "ModelPart");
    sourceChild->SetParent(sourceRoot);

    auto artifact = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &sourceRoot,
        {},
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("83838383-8383-4383-8383-838383838384")),
        "Assets/Prefabs/PreviewCommitHero.prefab"
    }).artifact;
    ASSERT_TRUE(artifact.has_value());

    NLS::Engine::SceneSystem::Scene scene;
    auto& previewRoot = scene.CreateGameObject("PreviewCommitHero", "Model");

    NLS::Editor::Assets::PrefabEditorWorkflow workflow;
    auto result = workflow.ConnectExistingPrefabInstance({
        &*artifact,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("83838383-8383-4383-8383-838383838384")),
        "prefab:PreviewCommitHero"
    }, previewRoot);

    ASSERT_EQ(result.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);
    ASSERT_TRUE(result.instance.has_value());
    EXPECT_EQ(result.instance->instanceRoot, &previewRoot)
        << "Scene View release must keep the same preview root instead of instantiating a replacement.";
    EXPECT_NE(previewRoot.GetComponent<NLS::Engine::Components::MeshFilter>(), nullptr);
    EXPECT_NE(previewRoot.GetComponent<NLS::Engine::Components::MeshRenderer>(), nullptr);
    EXPECT_EQ(previewRoot.GetComponent<NLS::Engine::Components::MeshFilter>()->GetModelPath(), meshPath)
        << "Committing an incomplete drag preview must preserve the prefab mesh reference hint so renderer resolution does not reload or lose tasks.";
    const auto materialPaths = previewRoot.GetComponent<NLS::Engine::Components::MeshRenderer>()->GetMaterialPaths();
    ASSERT_EQ(materialPaths.size(), 1u);
    EXPECT_EQ(materialPaths.front(), materialPath)
        << "Committing an incomplete drag preview must preserve material hints for the same live object instead of creating a second load path.";
    auto* completedChild = FindChildByName(previewRoot, "PreviewCommitChild");
    ASSERT_NE(completedChild, nullptr);

    scene.AddGameObject(&previewRoot);
    EXPECT_NE(
        std::find(scene.GetGameObjects().begin(), scene.GetGameObjects().end(), completedChild),
        scene.GetGameObjects().end())
        << "Prefab completion must register newly restored children with the active scene for save and render traversal.";
    EXPECT_EQ(scene.GetFastAccessComponents().modelRenderers.size(), 1u);

    ASSERT_NE(result.instance->sourceByInstanceObject.find(&previewRoot), result.instance->sourceByInstanceObject.end());
    EXPECT_GE(result.instance->sourceByInstanceObject.size(), 2u)
        << "The connected instance must map the completed prefab hierarchy so renderer resolution can find mesh/material tasks.";
    EXPECT_FALSE(result.instance->sourceToInstance.empty());
}

TEST(PrefabEditorWorkflowTests, ConnectExistingPrefabInstancePreservesPreviewTransientMesh)
{
    const std::string meshPath = "Library/Artifacts/PreviewCommitTransient/db7ffec2d25e80c7b075bc30a992e27e5f392f809146715c3cdf514a6fba8beb";
    const auto meshId = NLS::Engine::Serialize::AssetId(
        NLS::Guid::Parse("83838383-8383-4383-8383-838383838387"));

    NLS::Engine::GameObject sourceRoot("PreviewCommitTransient", "Model");
    auto* sourceMeshFilter = sourceRoot.AddComponent<NLS::Engine::Components::MeshFilter>();
    ASSERT_NE(sourceMeshFilter, nullptr);
    sourceMeshFilter->SetMeshReference(MakePPtr<NLS::Render::Resources::Mesh>(
        NLS::Engine::Serialize::ObjectIdentifier::Asset(
            meshId,
            NLS::Engine::Serialize::MakeLocalIdentifierInFile(meshId.GetGuid(), meshPath),
            meshPath)));

    auto artifact = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &sourceRoot,
        {},
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("83838383-8383-4383-8383-838383838388")),
        "Assets/Prefabs/PreviewCommitTransient.prefab"
    }).artifact;
    ASSERT_TRUE(artifact.has_value());

    NLS::Engine::SceneSystem::Scene scene;
    auto& previewRoot = scene.CreateGameObject("PreviewCommitTransient", "Model");
    auto* previewMeshFilter = previewRoot.AddComponent<NLS::Engine::Components::MeshFilter>();
    ASSERT_NE(previewMeshFilter, nullptr);
    auto transientMesh = std::shared_ptr<NLS::Render::Resources::Mesh>(
        new NLS::Render::Resources::Mesh({}, {}, 0u));
    previewMeshFilter->SetResolvedTransientMeshFromReference(transientMesh);

    auto result = NLS::Editor::Assets::PrefabEditorWorkflow().ConnectExistingPrefabInstance({
        &*artifact,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("83838383-8383-4383-8383-838383838388")),
        "prefab:PreviewCommitTransient"
    }, previewRoot);

    ASSERT_EQ(result.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);
    ASSERT_TRUE(result.instance.has_value());
    EXPECT_EQ(result.instance->instanceRoot, &previewRoot);
    ASSERT_NE(previewRoot.GetComponent<NLS::Engine::Components::MeshFilter>(), nullptr);
    EXPECT_TRUE(previewRoot.GetComponent<NLS::Engine::Components::MeshFilter>()->HasResolvedTransientMesh())
        << "Connecting the preview root must not overwrite a mesh already loaded during drag preview.";
}

TEST(PrefabEditorWorkflowTests, ConnectExistingPrefabInstanceCompletesDuplicateNamedChildrenIndependently)
{
    NLS::Engine::GameObject sourceRoot("DuplicateChildPreview", "Model");
    auto* firstChild = new NLS::Engine::GameObject("RepeatedPart", "ModelPart");
    firstChild->SetParent(sourceRoot);
    auto* secondChild = new NLS::Engine::GameObject("RepeatedPart", "ModelPart");
    secondChild->SetParent(sourceRoot);

    auto artifact = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &sourceRoot,
        {},
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("83838383-8383-4383-8383-838383838389")),
        "Assets/Prefabs/DuplicateChildPreview.prefab"
    }).artifact;
    ASSERT_TRUE(artifact.has_value());

    NLS::Engine::SceneSystem::Scene scene;
    auto& previewRoot = scene.CreateGameObject("DuplicateChildPreview", "Model");

    auto result = NLS::Editor::Assets::PrefabEditorWorkflow().ConnectExistingPrefabInstance({
        &*artifact,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("83838383-8383-4383-8383-838383838389")),
        "prefab:DuplicateChildPreview"
    }, previewRoot);

    ASSERT_EQ(result.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);
    ASSERT_TRUE(result.instance.has_value());
    EXPECT_EQ(CountChildrenByName(previewRoot, "RepeatedPart"), 2u)
        << "Completing a preview root must not collapse same-name prefab siblings into one live child.";
    EXPECT_GE(result.instance->sourceByInstanceObject.size(), 3u);
}

TEST(PrefabEditorWorkflowTests, ConnectExistingPrefabInstanceCompletesDuplicateSameTypeComponentsIndependently)
{
    NLS::Engine::GameObject sourceRoot("DuplicateComponentPreview", "Model");
    ASSERT_NE(sourceRoot.AddComponent<NLS::Engine::Components::LightComponent>(), nullptr);
    ASSERT_NE(sourceRoot.AddComponent<NLS::Engine::Components::LightComponent>(), nullptr);

    auto artifact = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &sourceRoot,
        {},
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("83838383-8383-4383-8383-83838383838a")),
        "Assets/Prefabs/DuplicateComponentPreview.prefab"
    }).artifact;
    ASSERT_TRUE(artifact.has_value());

    NLS::Engine::SceneSystem::Scene scene;
    auto& previewRoot = scene.CreateGameObject("DuplicateComponentPreview", "Model");

    auto result = NLS::Editor::Assets::PrefabEditorWorkflow().ConnectExistingPrefabInstance({
        &*artifact,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("83838383-8383-4383-8383-83838383838a")),
        "prefab:DuplicateComponentPreview"
    }, previewRoot);

    ASSERT_EQ(result.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);
    ASSERT_TRUE(result.instance.has_value());
    const auto duplicateLightCount = static_cast<size_t>(std::count_if(
        previewRoot.GetComponents().begin(),
        previewRoot.GetComponents().end(),
        [](const auto& component)
        {
            return component &&
                component->GetType() == NLS_TYPEOF(NLS::Engine::Components::LightComponent);
        }));
    EXPECT_EQ(duplicateLightCount, 2u)
        << "Completing a preview root must not map duplicate same-type source components onto one live component.";
}

TEST(PrefabEditorWorkflowTests, ConnectExistingPrefabInstanceCompletesMissingMaterialSlotsWithoutClearingLoadedSlot)
{
    const std::string firstMaterialPath = "Library/Artifacts/PreviewCommitPartialMaterials/dcae5a38be96376d6b06a1b70d9e3897ddfbe16937de85e3ffa05c78b878b351";
    const std::string secondMaterialPath = "Library/Artifacts/PreviewCommitPartialMaterials/47ad399b45bcdda2bfbe6ee59e6a6e36ac148a09e62f7ec47862fae4f8e8c07a";
    const auto firstMaterialId = NLS::Engine::Serialize::AssetId(
        NLS::Guid::Parse("83838383-8383-4383-8383-83838383838b"));
    const auto secondMaterialId = NLS::Engine::Serialize::AssetId(
        NLS::Guid::Parse("83838383-8383-4383-8383-83838383838c"));

    NLS::Engine::GameObject sourceRoot("PartialMaterialPreview", "Model");
    auto* sourceMeshRenderer = sourceRoot.AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(sourceMeshRenderer, nullptr);
    sourceMeshRenderer->SetMaterialReferences({
        MakePPtr<NLS::Render::Resources::Material>(
            NLS::Engine::Serialize::ObjectIdentifier::Asset(
                firstMaterialId,
                NLS::Engine::Serialize::MakeLocalIdentifierInFile(firstMaterialId.GetGuid(), firstMaterialPath),
                firstMaterialPath)),
        MakePPtr<NLS::Render::Resources::Material>(
            NLS::Engine::Serialize::ObjectIdentifier::Asset(
                secondMaterialId,
                NLS::Engine::Serialize::MakeLocalIdentifierInFile(secondMaterialId.GetGuid(), secondMaterialPath),
                secondMaterialPath))
    });

    auto artifact = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &sourceRoot,
        {},
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("83838383-8383-4383-8383-83838383838d")),
        "Assets/Prefabs/PartialMaterialPreview.prefab"
    }).artifact;
    ASSERT_TRUE(artifact.has_value());

    NLS::Engine::SceneSystem::Scene scene;
    auto& previewRoot = scene.CreateGameObject("PartialMaterialPreview", "Model");
    auto* previewMeshRenderer = previewRoot.AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(previewMeshRenderer, nullptr);
    auto firstMaterial = std::make_unique<NLS::Render::Resources::Material>();
    firstMaterial->path = firstMaterialPath;
    previewMeshRenderer->SetResolvedMaterialFromReference(0u, *firstMaterial);

    auto result = NLS::Editor::Assets::PrefabEditorWorkflow().ConnectExistingPrefabInstance({
        &*artifact,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("83838383-8383-4383-8383-83838383838d")),
        "prefab:PartialMaterialPreview"
    }, previewRoot);

    ASSERT_EQ(result.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);
    ASSERT_TRUE(result.instance.has_value());
    ASSERT_EQ(previewMeshRenderer->GetMaterialAtIndex(0u), firstMaterial.get());
    const auto materialPaths = previewMeshRenderer->GetMaterialPaths();
    ASSERT_EQ(materialPaths.size(), 2u);
    EXPECT_EQ(materialPaths[0], firstMaterialPath);
    EXPECT_EQ(materialPaths[1], secondMaterialPath)
        << "Partially loaded preview materials must still receive hints for not-yet-loaded slots.";
}

TEST(PrefabEditorWorkflowTests, RegistryTracksConnectedPrefabPresentationStates)
{
    NLS::Engine::GameObject root("Crate", "Prop");
    auto artifact = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &root,
        {},
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("83111111-1111-4311-8311-111111111111")),
        "Assets/Prefabs/Crate.prefab"
    }).artifact;
    ASSERT_TRUE(artifact.has_value());

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::PrefabEditorWorkflow workflow;
    auto instantiate = workflow.InstantiatePrefab({
        &*artifact,
        artifact->assetId,
        "prefab:Crate"
    }, scene);
    ASSERT_TRUE(instantiate.instance.has_value());

    NLS::Editor::Assets::PrefabInstanceRegistry registry;
    auto& registered = registry.Register(std::move(*instantiate.instance));

    ASSERT_NE(registered.instanceRoot, nullptr);
    const auto rootPresentation = registry.GetPresentation(*registered.instanceRoot);
    EXPECT_EQ(rootPresentation.state, NLS::Editor::Assets::PrefabHierarchyState::Root);
    EXPECT_EQ(rootPresentation.assetId, artifact->assetId);
    EXPECT_EQ(rootPresentation.subAssetKey, "prefab:Crate");
    EXPECT_FALSE(rootPresentation.hasOverrides);
    EXPECT_FALSE(rootPresentation.missingAsset);

    registered.localPatches.push_back(NLS::Engine::Serialize::PatchOperation::ReplaceProperty(
        artifact->graph.root,
        "name",
        NLS::Engine::Serialize::PropertyValue::String("LocalName")));

    const auto overriddenPresentation = registry.GetPresentation(*registered.instanceRoot);
    EXPECT_EQ(overriddenPresentation.state, NLS::Editor::Assets::PrefabHierarchyState::Root);
    EXPECT_TRUE(overriddenPresentation.hasOverrides);
    EXPECT_EQ(overriddenPresentation.color, NLS::Editor::Assets::PrefabHierarchyColorToken::ConnectedRoot);

    registry.MarkAssetPendingResources(artifact->assetId, true);
    const auto pendingPresentation = registry.GetPresentation(*registered.instanceRoot);
    EXPECT_TRUE(pendingPresentation.pendingResources);
    EXPECT_EQ(pendingPresentation.color, NLS::Editor::Assets::PrefabHierarchyColorToken::ConnectedRoot);

    registry.MarkAssetMissing(artifact->assetId, true);
    const auto missingPresentation = registry.GetPresentation(*registered.instanceRoot);
    EXPECT_TRUE(missingPresentation.missingAsset);
    EXPECT_EQ(missingPresentation.color, NLS::Editor::Assets::PrefabHierarchyColorToken::Missing);

    registry.MarkAssetMissing(artifact->assetId, false);
    registry.MarkAssetPendingResources(artifact->assetId, false);
    const auto unpack = workflow.UnpackPrefabInstance(registered);
    ASSERT_EQ(unpack.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);

    const auto unpackedPresentation = registry.GetPresentation(*registered.instanceRoot);
    EXPECT_TRUE(unpackedPresentation.unpacked);
    EXPECT_EQ(unpackedPresentation.color, NLS::Editor::Assets::PrefabHierarchyColorToken::Unpacked);
}

TEST(PrefabEditorWorkflowTests, RegistryResourceFailurePresentationIsScopedToInstanceRoot)
{
    NLS::Engine::SceneSystem::Scene scene;
    auto& firstRoot = scene.CreateGameObject("SharedPrefab", "Prop");
    auto& secondRoot = scene.CreateGameObject("SharedPrefab", "Prop");

    const auto prefabAssetId =
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("83112222-2222-4322-8322-222222222222"));

    NLS::Editor::Assets::PrefabInstanceRecord firstInstance;
    firstInstance.prefabAssetId = prefabAssetId;
    firstInstance.prefabSubAssetKey = "prefab:SharedPrefab";
    firstInstance.instanceRoot = &firstRoot;

    NLS::Editor::Assets::PrefabInstanceRecord secondInstance;
    secondInstance.prefabAssetId = prefabAssetId;
    secondInstance.prefabSubAssetKey = "prefab:SharedPrefab";
    secondInstance.instanceRoot = &secondRoot;

    NLS::Editor::Assets::PrefabInstanceRegistry registry;
    registry.Register(std::move(firstInstance));
    registry.Register(std::move(secondInstance));

    registry.MarkInstanceResourceFailure(firstRoot, true);
    registry.MarkInstancePendingResources(firstRoot, false);

    const auto failedPresentation = registry.GetPresentation(firstRoot);
    EXPECT_TRUE(failedPresentation.missingAsset);
    EXPECT_EQ(failedPresentation.color, NLS::Editor::Assets::PrefabHierarchyColorToken::Missing);

    const auto siblingPresentation = registry.GetPresentation(secondRoot);
    EXPECT_FALSE(siblingPresentation.missingAsset)
        << "Renderer-resource failure on one prefab instance must not poison sibling instances "
           "that reference the same prefab asset.";
    EXPECT_EQ(siblingPresentation.color, NLS::Editor::Assets::PrefabHierarchyColorToken::ConnectedRoot);
}

TEST(PrefabEditorWorkflowTests, RegistrySourceMissingStateIsScopedToSubAssetKey)
{
    NLS::Engine::SceneSystem::Scene scene;
    auto& firstRoot = scene.CreateGameObject("SharedModelVariantA", "Prefab");
    auto& secondRoot = scene.CreateGameObject("SharedModelVariantB", "Prefab");

    const auto prefabAssetId =
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("83114444-4444-4344-8344-444444444444"));

    NLS::Editor::Assets::PrefabInstanceRecord firstInstance;
    firstInstance.prefabAssetId = prefabAssetId;
    firstInstance.prefabSubAssetKey = "prefab:VariantA";
    firstInstance.instanceRoot = &firstRoot;

    NLS::Editor::Assets::PrefabInstanceRecord secondInstance;
    secondInstance.prefabAssetId = prefabAssetId;
    secondInstance.prefabSubAssetKey = "prefab:VariantB";
    secondInstance.instanceRoot = &secondRoot;

    NLS::Editor::Assets::PrefabInstanceRegistry registry;
    registry.Register(std::move(firstInstance));
    registry.Register(std::move(secondInstance));

    registry.MarkAssetMissing(prefabAssetId, "prefab:VariantA", true);

    EXPECT_TRUE(registry.GetPresentation(firstRoot).missingAsset);
    EXPECT_FALSE(registry.GetPresentation(secondRoot).missingAsset)
        << "A missing sub-prefab must not mark sibling prefab instances from the same source asset as missing.";

    registry.MarkAssetMissing(prefabAssetId, "prefab:VariantA", false);
    EXPECT_FALSE(registry.GetPresentation(firstRoot).missingAsset);
}

TEST(PrefabEditorWorkflowTests, RegistryClearsInstanceResourceFailureWithoutTouchingSourceAssetMissing)
{
    NLS::Engine::SceneSystem::Scene scene;
    auto& root = scene.CreateGameObject("RecoveringPrefab", "Prop");
    const auto prefabAssetId =
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("83113333-3333-4333-8333-333333333333"));

    NLS::Editor::Assets::PrefabInstanceRecord instance;
    instance.prefabAssetId = prefabAssetId;
    instance.prefabSubAssetKey = "prefab:RecoveringPrefab";
    instance.instanceRoot = &root;

    NLS::Editor::Assets::PrefabInstanceRegistry registry;
    registry.Register(std::move(instance));

    registry.MarkInstanceResourceFailure(root, true);
    EXPECT_TRUE(registry.GetPresentation(root).missingAsset);

    registry.MarkInstanceResourceFailure(root, false);
    EXPECT_FALSE(registry.GetPresentation(root).missingAsset)
        << "A successful retry must be able to clear only the instance-level renderer resource failure.";

    registry.MarkAssetMissing(prefabAssetId, true);
    registry.MarkInstanceResourceFailure(root, false);
    EXPECT_TRUE(registry.GetPresentation(root).missingAsset)
        << "Clearing renderer resource failure must not hide a real missing source prefab asset.";
}

TEST(PrefabEditorWorkflowTests, RegistryRemovesPrefabRootInstanceWithoutTouchingUnrelatedPrefabs)
{
    NLS::Engine::SceneSystem::Scene scene;
    auto& root = scene.CreateGameObject("PrefabRoot", "Prefab");
    auto& child = scene.CreateGameObject("PrefabChild", "Prefab");
    auto& unrelated = scene.CreateGameObject("UnrelatedPrefab", "Prefab");
    child.SetParent(root);

    NLS::Editor::Assets::PrefabInstanceRecord instance;
    instance.prefabAssetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("83111111-1111-4311-8311-111111111111"));
    instance.prefabSubAssetKey = "prefab:PrefabRoot";
    instance.instanceRoot = &root;
    instance.sourceByInstanceObject.emplace(
        &root,
        NLS::Engine::Serialize::ObjectId(NLS::Guid::NewDeterministic("Registry.Remove.Root")));
    instance.sourceByInstanceObject.emplace(
        &child,
        NLS::Engine::Serialize::ObjectId(NLS::Guid::NewDeterministic("Registry.Remove.Child")));

    NLS::Editor::Assets::PrefabInstanceRecord unrelatedInstance;
    unrelatedInstance.prefabAssetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("83222222-2222-4322-8322-222222222222"));
    unrelatedInstance.prefabSubAssetKey = "prefab:UnrelatedPrefab";
    unrelatedInstance.instanceRoot = &unrelated;
    unrelatedInstance.sourceByInstanceObject.emplace(
        &unrelated,
        NLS::Engine::Serialize::ObjectId(NLS::Guid::NewDeterministic("Registry.Remove.Unrelated")));

    NLS::Editor::Assets::PrefabInstanceRegistry registry;
    registry.Register(std::move(instance));
    registry.Register(std::move(unrelatedInstance));

    EXPECT_EQ(registry.FindRootInstance(root), registry.FindInstance(root));
    EXPECT_TRUE(registry.RemoveRootInstance(root));
    EXPECT_EQ(registry.FindInstance(root), nullptr);
    EXPECT_EQ(registry.FindInstance(child), nullptr);
    EXPECT_NE(registry.FindInstance(unrelated), nullptr);
    EXPECT_FALSE(registry.RemoveRootInstance(root));
}

TEST(PrefabEditorWorkflowTests, RegistryPrunesDestroyedChildMappingWithoutRemovingRootInstance)
{
    NLS::Engine::SceneSystem::Scene scene;
    auto& root = scene.CreateGameObject("PrefabRoot", "Prefab");
    auto& child = scene.CreateGameObject("PrefabChild", "Prefab");
    child.SetParent(root);

    NLS::Editor::Assets::PrefabInstanceRecord instance;
    instance.prefabAssetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("83333333-3333-4333-8333-333333333333"));
    instance.prefabSubAssetKey = "prefab:PrefabRoot";
    instance.instanceRoot = &root;
    instance.sourceByInstanceObject.emplace(
        &root,
        NLS::Engine::Serialize::ObjectId(NLS::Guid::NewDeterministic("Registry.Prune.Root")));
    instance.sourceByInstanceObject.emplace(
        &child,
        NLS::Engine::Serialize::ObjectId(NLS::Guid::NewDeterministic("Registry.Prune.Child")));

    NLS::Editor::Assets::PrefabInstanceRegistry registry;
    registry.Register(std::move(instance));

    EXPECT_NE(registry.FindInstance(child), nullptr);
    EXPECT_TRUE(registry.RemoveObjectMapping(child));
    EXPECT_EQ(registry.FindInstance(child), nullptr);
    EXPECT_NE(registry.FindRootInstance(root), nullptr);
    EXPECT_FALSE(registry.RemoveObjectMapping(child));
}

TEST(PrefabEditorWorkflowTests, RegistryTracksPrefabChildAndGeneratedReadOnlyPresentationStates)
{
    NLS::Engine::SceneSystem::Scene sourceScene;
    auto& root = sourceScene.CreateGameObject("ImportedRoot", "Prefab");
    auto& child = sourceScene.CreateGameObject("ImportedChild", "Prefab");
    child.SetParent(root);

    auto artifact = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &root,
        {},
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("83111111-1111-4311-8311-111111111111")),
        "Assets/Prefabs/ImportedRoot.prefab"
    }).artifact;
    ASSERT_TRUE(artifact.has_value());
    artifact->generatedModelPrefab = true;

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::PrefabEditorWorkflow workflow;
    auto instantiate = workflow.InstantiatePrefab({
        &*artifact,
        artifact->assetId,
        "prefab:ImportedRoot"
    }, scene);
    ASSERT_TRUE(instantiate.instance.has_value());
    ASSERT_NE(instantiate.instance->instanceRoot, nullptr);

    auto* instanceChild = FindDescendantByName(*instantiate.instance->instanceRoot, "ImportedChild");
    ASSERT_NE(instanceChild, nullptr);

    NLS::Editor::Assets::PrefabInstanceRegistry registry;
    auto& registered = registry.Register(std::move(*instantiate.instance));

    const auto rootPresentation = registry.GetPresentation(*registered.instanceRoot);
    EXPECT_EQ(rootPresentation.state, NLS::Editor::Assets::PrefabHierarchyState::Root);
    EXPECT_TRUE(rootPresentation.generatedReadOnly);
    EXPECT_EQ(rootPresentation.color, NLS::Editor::Assets::PrefabHierarchyColorToken::ConnectedRoot);

    const auto childPresentation = registry.GetPresentation(*instanceChild);
    EXPECT_EQ(childPresentation.state, NLS::Editor::Assets::PrefabHierarchyState::Child);
    EXPECT_TRUE(childPresentation.generatedReadOnly);
    EXPECT_EQ(childPresentation.color, NLS::Editor::Assets::PrefabHierarchyColorToken::ConnectedChild);
}

TEST(PrefabEditorWorkflowTests, SavingPrefabStageRefreshesRegisteredConnectedInstancesAndKeepsLocalOverrides)
{
    NLS::Engine::GameObject root("Crate", "Prop");
    auto artifact = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &root,
        {},
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("83222222-2222-4322-8322-222222222222")),
        "Assets/Prefabs/Crate.prefab"
    }).artifact;
    ASSERT_TRUE(artifact.has_value());

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::PrefabEditorWorkflow workflow;

    auto first = workflow.InstantiatePrefab({
        &*artifact,
        artifact->assetId,
        "prefab:Crate"
    }, scene);
    auto second = workflow.InstantiatePrefab({
        &*artifact,
        artifact->assetId,
        "prefab:Crate"
    }, scene);
    ASSERT_TRUE(first.instance.has_value());
    ASSERT_TRUE(second.instance.has_value());

    NLS::Editor::Assets::PrefabInstanceRegistry registry;
    auto& firstInstance = registry.Register(std::move(*first.instance));
    auto& secondInstance = registry.Register(std::move(*second.instance));

    secondInstance.localPatches.push_back(NLS::Engine::Serialize::PatchOperation::ReplaceProperty(
        artifact->graph.root,
        "name",
        NLS::Engine::Serialize::PropertyValue::String("LocalCrate")));
    secondInstance.instanceRoot->SetName("LocalCrate");

    auto open = workflow.OpenPrefabStage({
        &*artifact,
        artifact->assetId,
        "prefab:Crate"
    });
    ASSERT_TRUE(open.stage.has_value());
    open.stage->stageRoot->SetName("SourceCrate");
    workflow.MarkStageDirty(*open.stage);

    auto save = workflow.SavePrefabStage(*open.stage, *artifact, &registry);

    ASSERT_EQ(save.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);
    EXPECT_EQ(firstInstance.instanceRoot->GetName(), "SourceCrate");
    EXPECT_EQ(secondInstance.instanceRoot->GetName(), "LocalCrate");
    EXPECT_EQ(firstInstance.sourceGraph.root, artifact->graph.root);
    EXPECT_EQ(secondInstance.sourceGraph.root, artifact->graph.root);
    EXPECT_EQ(registry.FindInstance(*firstInstance.instanceRoot), &firstInstance);
    EXPECT_EQ(registry.FindInstance(*secondInstance.instanceRoot), &secondInstance);
}

TEST(PrefabEditorWorkflowTests, DiscoversReflectedPropertyOverridesFromPrefabInstance)
{
    NLS::Engine::GameObject root("Crate", "Prop");
    auto artifact = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &root,
        {},
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("84848484-8484-4484-8484-848484848484")),
        "Assets/Prefabs/Crate.prefab"
    }).artifact;
    ASSERT_TRUE(artifact.has_value());

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::PrefabEditorWorkflow workflow;
    auto instantiate = workflow.InstantiatePrefab({
        &*artifact,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("84848484-8484-4484-8484-848484848484")),
        "prefab:Crate"
    }, scene);
    ASSERT_TRUE(instantiate.instance.has_value());
    instantiate.instance->instanceRoot->SetName("RenamedCrate");

    const auto overrides = workflow.DiscoverOverrides(*artifact, *instantiate.instance);

    ASSERT_EQ(overrides.size(), 1u);
    EXPECT_EQ(overrides[0].sourceObject, artifact->graph.root);
    EXPECT_EQ(overrides[0].propertyPath, "name");
    EXPECT_EQ(overrides[0].patch.property, "name");
    ASSERT_EQ(overrides[0].patch.value.GetKind(), NLS::Engine::Serialize::PropertyValue::Kind::String);
    EXPECT_EQ(overrides[0].patch.value.GetString(), "RenamedCrate");
}

TEST(PrefabEditorWorkflowTests, DiscoversReflectedComponentPropertyOverridesFromPrefabInstance)
{
    NLS::Engine::GameObject root("MovedCrate", "Prop");
    auto artifact = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &root,
        {},
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("84848484-8484-4484-8484-848484848486")),
        "Assets/Prefabs/MovedCrate.prefab"
    }).artifact;
    ASSERT_TRUE(artifact.has_value());

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::PrefabEditorWorkflow workflow;
    auto instantiate = workflow.InstantiatePrefab({
        &*artifact,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("84848484-8484-4484-8484-848484848486")),
        "prefab:MovedCrate"
    }, scene);
    ASSERT_TRUE(instantiate.instance.has_value());
    ASSERT_NE(instantiate.instance->instanceRoot, nullptr);

    instantiate.instance->instanceRoot->GetTransform()->SetLocalPosition({7.0f, 3.0f, -2.0f});

    const auto overrides = workflow.DiscoverOverrides(*artifact, *instantiate.instance);
    const auto* positionOverride = FindOverride(
        overrides,
        NLS::Engine::Serialize::PatchOperationType::ReplaceProperty,
        "localPosition");

    ASSERT_NE(positionOverride, nullptr);
    EXPECT_NE(positionOverride->sourceObject, artifact->graph.root);
    EXPECT_EQ(positionOverride->propertyPath, "localPosition");
    ASSERT_EQ(positionOverride->patch.value.GetKind(), NLS::Engine::Serialize::PropertyValue::Kind::Object);
    const auto* x = FindObjectProperty(positionOverride->patch.value, "x");
    const auto* y = FindObjectProperty(positionOverride->patch.value, "y");
    const auto* z = FindObjectProperty(positionOverride->patch.value, "z");
    ASSERT_NE(x, nullptr);
    ASSERT_NE(y, nullptr);
    ASSERT_NE(z, nullptr);
    EXPECT_DOUBLE_EQ(x->GetNumber(), 7.0);
    EXPECT_DOUBLE_EQ(y->GetNumber(), 3.0);
    EXPECT_DOUBLE_EQ(z->GetNumber(), -2.0);
}

TEST(PrefabEditorWorkflowTests, DiscoversDuplicateSameTypeComponentPropertyOverridesBySourceOrder)
{
    NLS::Engine::GameObject root("DuplicateLightOverrides", "Prop");
    auto* firstSourceLight = root.AddComponent<NLS::Engine::Components::LightComponent>();
    auto* secondSourceLight = root.AddComponent<NLS::Engine::Components::LightComponent>();
    ASSERT_NE(firstSourceLight, nullptr);
    ASSERT_NE(secondSourceLight, nullptr);
    firstSourceLight->SetIntensity(1.0f);
    secondSourceLight->SetIntensity(2.0f);

    auto artifact = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &root,
        {},
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("84848484-8484-4484-8484-848484848487")),
        "Assets/Prefabs/DuplicateLightOverrides.prefab"
    }).artifact;
    ASSERT_TRUE(artifact.has_value());

    const auto* rootRecord = FindObjectRecord(artifact->graph, artifact->graph.root);
    ASSERT_NE(rootRecord, nullptr);
    const auto sourceComponents = ReadOwnedReferences(*rootRecord, "components");
    ASSERT_GE(sourceComponents.size(), 3u);
    const auto secondLightSource = sourceComponents[2];

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::PrefabEditorWorkflow workflow;
    auto instantiate = workflow.InstantiatePrefab({
        &*artifact,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("84848484-8484-4484-8484-848484848487")),
        "prefab:DuplicateLightOverrides"
    }, scene);
    ASSERT_TRUE(instantiate.instance.has_value());
    ASSERT_NE(instantiate.instance->instanceRoot, nullptr);

    size_t lightIndex = 0u;
    for (const auto& component : instantiate.instance->instanceRoot->GetComponents())
    {
        if (!component || component->GetType() != NLS_TYPEOF(NLS::Engine::Components::LightComponent))
            continue;

        if (lightIndex == 1u)
            static_cast<NLS::Engine::Components::LightComponent*>(component.get())->SetIntensity(9.0f);
        ++lightIndex;
    }
    ASSERT_EQ(lightIndex, 2u);

    const auto overrides = workflow.DiscoverOverrides(*artifact, *instantiate.instance);
    const auto found = std::find_if(
        overrides.begin(),
        overrides.end(),
        [&](const NLS::Editor::Assets::PrefabOverrideRecord& overrideRecord)
        {
            return overrideRecord.patch.type == NLS::Engine::Serialize::PatchOperationType::ReplaceProperty &&
                overrideRecord.patch.target == secondLightSource &&
                overrideRecord.patch.property == "intensity" &&
                overrideRecord.patch.value.GetKind() == NLS::Engine::Serialize::PropertyValue::Kind::Number &&
                overrideRecord.patch.value.GetNumber() == 9.0;
        });

    EXPECT_NE(found, overrides.end())
        << "Duplicate same-type component property overrides must stay aligned with the corresponding source component.";
}

TEST(PrefabEditorWorkflowTests, PrefersLivePropertyOverrideOverStaleLocalPatchWhenApplying)
{
    NLS::Engine::GameObject root("Crate", "Prop");
    auto artifact = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &root,
        {},
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("84848484-8484-4484-8484-848484848485")),
        "Assets/Prefabs/Crate.prefab"
    }).artifact;
    ASSERT_TRUE(artifact.has_value());

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::PrefabEditorWorkflow workflow;
    auto instantiate = workflow.InstantiatePrefab({
        &*artifact,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("84848484-8484-4484-8484-848484848485")),
        "prefab:Crate"
    }, scene);
    ASSERT_TRUE(instantiate.instance.has_value());
    instantiate.instance->localPatches.push_back(NLS::Engine::Serialize::PatchOperation::ReplaceProperty(
        artifact->graph.root,
        "name",
        NLS::Engine::Serialize::PropertyValue::String("StaleName")));
    instantiate.instance->instanceRoot->SetName("FreshName");

    const auto overrides = workflow.DiscoverOverrides(*artifact, *instantiate.instance);
    const auto* nameOverride = FindOverride(
        overrides,
        NLS::Engine::Serialize::PatchOperationType::ReplaceProperty,
        "name");

    ASSERT_EQ(overrides.size(), 1u);
    ASSERT_NE(nameOverride, nullptr);
    ASSERT_EQ(nameOverride->patch.value.GetKind(), NLS::Engine::Serialize::PropertyValue::Kind::String);
    EXPECT_EQ(nameOverride->patch.value.GetString(), "FreshName");

    const auto result = workflow.ApplyAllOverrides(*artifact, overrides);

    ASSERT_EQ(result.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);
    EXPECT_EQ(ReadStringProperty(*FindObjectRecord(artifact->graph, artifact->graph.root), "name"), "FreshName");
}

TEST(PrefabEditorWorkflowTests, DoesNotExposePersistedStructuralPatchWithoutLivePayload)
{
    NLS::Engine::GameObject root("Assembly", "Prop");
    auto artifact = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &root,
        {},
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("84848484-8484-4484-8484-848484848486")),
        "Assets/Prefabs/Assembly.prefab"
    }).artifact;
    ASSERT_TRUE(artifact.has_value());

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::PrefabEditorWorkflow workflow;
    auto instantiate = workflow.InstantiatePrefab({
        &*artifact,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("84848484-8484-4484-8484-848484848486")),
        "prefab:Assembly"
    }, scene);
    ASSERT_TRUE(instantiate.instance.has_value());
    instantiate.instance->localPatches.push_back(NLS::Engine::Serialize::PatchOperation::InsertOwned(
        artifact->graph.root,
        "children",
        NLS::Engine::Serialize::ObjectId(NLS::Guid::Parse("84848484-8484-4484-8484-848484848487")),
        0u));

    const auto overrides = workflow.DiscoverOverrides(*artifact, *instantiate.instance);

    EXPECT_EQ(FindOverride(overrides, NLS::Engine::Serialize::PatchOperationType::InsertOwned, "children"), nullptr);
    EXPECT_TRUE(overrides.empty());
}

TEST(PrefabEditorWorkflowTests, DiscoversAddedComponentOverridesFromPrefabInstance)
{
    NLS::Engine::GameObject root("Lamp", "Prop");
    auto artifact = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &root,
        {},
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("85858585-8585-4585-8585-858585858585")),
        "Assets/Prefabs/Lamp.prefab"
    }).artifact;
    ASSERT_TRUE(artifact.has_value());

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::PrefabEditorWorkflow workflow;
    auto instantiate = workflow.InstantiatePrefab({
        &*artifact,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("85858585-8585-4585-8585-858585858585")),
        "prefab:Lamp"
    }, scene);
    ASSERT_TRUE(instantiate.instance.has_value());
    instantiate.instance->instanceRoot->AddComponent<NLS::Engine::Components::LightComponent>()->SetIntensity(7.0f);

    const auto overrides = workflow.DiscoverOverrides(*artifact, *instantiate.instance);
    const auto* inserted = FindOverride(
        overrides,
        NLS::Engine::Serialize::PatchOperationType::InsertOwned,
        "components");

    ASSERT_NE(inserted, nullptr);
    EXPECT_EQ(inserted->sourceObject, artifact->graph.root);
    EXPECT_EQ(inserted->propertyPath, "components");
    EXPECT_TRUE(inserted->patch.object.IsValid());
    EXPECT_TRUE(inserted->patch.hasIndex);
    EXPECT_TRUE(inserted->objectRecord.has_value());
    EXPECT_EQ(inserted->objectRecord->typeName, "NLS::Engine::Components::LightComponent");
}

TEST(PrefabEditorWorkflowTests, DiscoversRemovedAndReorderedComponentOverridesFromPrefabInstance)
{
    NLS::Engine::GameObject root("Lamp", "Prop");
    root.AddComponent<NLS::Engine::Components::LightComponent>()->SetIntensity(3.0f);
    auto artifact = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &root,
        {},
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("86868686-8686-4686-8686-868686868686")),
        "Assets/Prefabs/Lamp.prefab"
    }).artifact;
    ASSERT_TRUE(artifact.has_value());

    NLS::Engine::SceneSystem::Scene reorderScene;
    NLS::Editor::Assets::PrefabEditorWorkflow workflow;
    auto reorderedInstance = workflow.InstantiatePrefab({
        &*artifact,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("86868686-8686-4686-8686-868686868686")),
        "prefab:Lamp"
    }, reorderScene);
    ASSERT_TRUE(reorderedInstance.instance.has_value());
    auto* light = reorderedInstance.instance->instanceRoot->GetComponent<NLS::Engine::Components::LightComponent>();
    ASSERT_NE(light, nullptr);
    ASSERT_TRUE(reorderedInstance.instance->instanceRoot->MoveComponent(light, 0u));

    const auto reorderOverrides = workflow.DiscoverOverrides(*artifact, *reorderedInstance.instance);
    const auto* moved = FindOverride(
        reorderOverrides,
        NLS::Engine::Serialize::PatchOperationType::MoveOwned,
        "components");
    ASSERT_NE(moved, nullptr);
    EXPECT_TRUE(moved->patch.hasIndex);
    EXPECT_EQ(moved->patch.index, 0u);

    NLS::Engine::SceneSystem::Scene removeScene;
    auto removedInstance = workflow.InstantiatePrefab({
        &*artifact,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("86868686-8686-4686-8686-868686868686")),
        "prefab:Lamp"
    }, removeScene);
    ASSERT_TRUE(removedInstance.instance.has_value());
    light = removedInstance.instance->instanceRoot->GetComponent<NLS::Engine::Components::LightComponent>();
    ASSERT_NE(light, nullptr);
    ASSERT_TRUE(removedInstance.instance->instanceRoot->RemoveComponent(light));

    const auto removeOverrides = workflow.DiscoverOverrides(*artifact, *removedInstance.instance);
    const auto* removed = FindOverride(
        removeOverrides,
        NLS::Engine::Serialize::PatchOperationType::RemoveOwned,
        "components");
    ASSERT_NE(removed, nullptr);
    EXPECT_TRUE(removed->patch.object.IsValid());
}

TEST(PrefabEditorWorkflowTests, DiscoversAddedRemovedAndReorderedChildOverridesFromPrefabInstance)
{
    NLS::Engine::GameObject root("Assembly", "Prop");
    NLS::Engine::GameObject first("First", "Part");
    NLS::Engine::GameObject second("Second", "Part");
    first.SetParent(root);
    second.SetParent(root);
    auto artifact = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &root,
        {},
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("87878787-8787-4787-8787-878787878787")),
        "Assets/Prefabs/Assembly.prefab"
    }).artifact;
    first.DetachFromParent();
    second.DetachFromParent();
    ASSERT_TRUE(artifact.has_value());

    NLS::Editor::Assets::PrefabEditorWorkflow workflow;

    NLS::Engine::SceneSystem::Scene addScene;
    auto addedInstance = workflow.InstantiatePrefab({
        &*artifact,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("87878787-8787-4787-8787-878787878787")),
        "prefab:Assembly"
    }, addScene);
    ASSERT_TRUE(addedInstance.instance.has_value());
    NLS::Engine::GameObject addedChild("Third", "Part");
    addedChild.SetParent(*addedInstance.instance->instanceRoot);
    const auto addOverrides = workflow.DiscoverOverrides(*artifact, *addedInstance.instance);
    addedChild.DetachFromParent();
    const auto* inserted = FindOverride(
        addOverrides,
        NLS::Engine::Serialize::PatchOperationType::InsertOwned,
        "children");
    ASSERT_NE(inserted, nullptr);
    ASSERT_TRUE(inserted->objectRecord.has_value());
    EXPECT_EQ(ReadStringProperty(*inserted->objectRecord, "name"), "Third");

    NLS::Engine::SceneSystem::Scene reorderScene;
    auto reorderedInstance = workflow.InstantiatePrefab({
        &*artifact,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("87878787-8787-4787-8787-878787878787")),
        "prefab:Assembly"
    }, reorderScene);
    ASSERT_TRUE(reorderedInstance.instance.has_value());
    auto& children = reorderedInstance.instance->instanceRoot->GetChildren();
    ASSERT_EQ(children.size(), 2u);
    std::swap(children[0], children[1]);
    const auto reorderOverrides = workflow.DiscoverOverrides(*artifact, *reorderedInstance.instance);
    const auto* moved = FindOverride(
        reorderOverrides,
        NLS::Engine::Serialize::PatchOperationType::MoveOwned,
        "children");
    ASSERT_NE(moved, nullptr);
    EXPECT_TRUE(moved->patch.hasIndex);
    EXPECT_EQ(moved->patch.index, 0u);

    NLS::Engine::SceneSystem::Scene removeScene;
    auto removedInstance = workflow.InstantiatePrefab({
        &*artifact,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("87878787-8787-4787-8787-878787878787")),
        "prefab:Assembly"
    }, removeScene);
    ASSERT_TRUE(removedInstance.instance.has_value());
    auto* firstChild = FindChildByName(*removedInstance.instance->instanceRoot, "First");
    ASSERT_NE(firstChild, nullptr);
    firstChild->DetachFromParent();
    const auto removeOverrides = workflow.DiscoverOverrides(*artifact, *removedInstance.instance);

    ASSERT_NE(FindOverride(
        removeOverrides,
        NLS::Engine::Serialize::PatchOperationType::RemoveOwned,
        "children"), nullptr);
    const auto removedObject = std::find_if(
        removeOverrides.begin(),
        removeOverrides.end(),
        [](const NLS::Editor::Assets::PrefabOverrideRecord& overrideRecord)
        {
            return overrideRecord.patch.type == NLS::Engine::Serialize::PatchOperationType::RemoveObject;
        });
    EXPECT_NE(removedObject, removeOverrides.end());
}

TEST(PrefabEditorWorkflowTests, DiscoversChildOverridesByInstanceMappingWhenSiblingNamesMatch)
{
    NLS::Engine::GameObject root("Assembly", "Prop");
    NLS::Engine::GameObject first("Bolt", "Part");
    NLS::Engine::GameObject second("Bolt", "Part");
    first.SetParent(root);
    second.SetParent(root);
    auto artifact = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &root,
        {},
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("88888888-8888-4888-8888-888888888888")),
        "Assets/Prefabs/Assembly.prefab"
    }).artifact;
    first.DetachFromParent();
    second.DetachFromParent();
    ASSERT_TRUE(artifact.has_value());
    const auto* rootRecord = FindObjectRecord(artifact->graph, artifact->graph.root);
    ASSERT_NE(rootRecord, nullptr);
    const auto* rootChildren = FindProperty(*rootRecord, "children");
    ASSERT_NE(rootChildren, nullptr);
    ASSERT_EQ(rootChildren->value.GetKind(), NLS::Engine::Serialize::PropertyValue::Kind::Array);
    ASSERT_EQ(rootChildren->value.GetArray().size(), 2u);
    const auto secondSourceChild = rootChildren->value.GetArray()[1].GetObjectId();

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::PrefabEditorWorkflow workflow;
    auto instantiate = workflow.InstantiatePrefab({
        &*artifact,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("88888888-8888-4888-8888-888888888888")),
        "prefab:Assembly"
    }, scene);
    ASSERT_TRUE(instantiate.instance.has_value());
    auto& children = instantiate.instance->instanceRoot->GetChildren();
    ASSERT_EQ(children.size(), 2u);
    std::swap(children[0], children[1]);

    const auto overrides = workflow.DiscoverOverrides(*artifact, *instantiate.instance);
    const auto* moved = FindOverride(
        overrides,
        NLS::Engine::Serialize::PatchOperationType::MoveOwned,
        "children");

    ASSERT_NE(moved, nullptr);
    EXPECT_EQ(moved->patch.object, secondSourceChild);
    EXPECT_EQ(moved->patch.index, 0u);
}

TEST(PrefabEditorWorkflowTests, DiscoversOverridesRecursivelyInChildHierarchy)
{
    NLS::Engine::GameObject root("Rig", "Prop");
    NLS::Engine::GameObject arm("Arm", "Part");
    NLS::Engine::GameObject hand("Hand", "Part");
    arm.SetParent(root);
    hand.SetParent(arm);
    auto artifact = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &root,
        {},
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("88888888-8888-4888-9888-888888888888")),
        "Assets/Prefabs/Rig.prefab"
    }).artifact;
    hand.DetachFromParent();
    arm.DetachFromParent();
    ASSERT_TRUE(artifact.has_value());

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::PrefabEditorWorkflow workflow;
    auto instantiate = workflow.InstantiatePrefab({
        &*artifact,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("88888888-8888-4888-9888-888888888888")),
        "prefab:Rig"
    }, scene);
    ASSERT_TRUE(instantiate.instance.has_value());
    auto* liveHand = FindDescendantByName(*instantiate.instance->instanceRoot, "Hand");
    ASSERT_NE(liveHand, nullptr);
    liveHand->SetName("OpenHand");

    const auto overrides = workflow.DiscoverOverrides(*artifact, *instantiate.instance);
    const auto found = std::find_if(
        overrides.begin(),
        overrides.end(),
        [](const NLS::Editor::Assets::PrefabOverrideRecord& overrideRecord)
        {
            return overrideRecord.patch.type == NLS::Engine::Serialize::PatchOperationType::ReplaceProperty &&
                overrideRecord.propertyPath == "name" &&
                overrideRecord.patch.value.GetKind() == NLS::Engine::Serialize::PropertyValue::Kind::String &&
                overrideRecord.patch.value.GetString() == "OpenHand";
        });

    EXPECT_NE(found, overrides.end());
}

TEST(PrefabEditorWorkflowTests, AppliesSelectedPropertyOverrideToEditablePrefab)
{
    NLS::Engine::GameObject root("Crate", "Prop");
    auto artifact = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &root,
        {},
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("89898989-8989-4989-8989-898989898989")),
        "Assets/Prefabs/Crate.prefab"
    }).artifact;
    ASSERT_TRUE(artifact.has_value());

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::PrefabEditorWorkflow workflow;
    auto instantiate = workflow.InstantiatePrefab({
        &*artifact,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("89898989-8989-4989-8989-898989898989")),
        "prefab:Crate"
    }, scene);
    ASSERT_TRUE(instantiate.instance.has_value());
    instantiate.instance->instanceRoot->SetName("AppliedCrate");
    const auto overrides = workflow.DiscoverOverrides(*artifact, *instantiate.instance);
    const auto* nameOverride = FindOverride(
        overrides,
        NLS::Engine::Serialize::PatchOperationType::ReplaceProperty,
        "name");
    ASSERT_NE(nameOverride, nullptr);

    const auto result = workflow.ApplySelectedOverride(*artifact, *nameOverride);

    ASSERT_EQ(result.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);
    ASSERT_FALSE(result.artifact->Validate().HasErrors());
    const auto* name = FindRootProperty(*artifact, "name");
    ASSERT_NE(name, nullptr);
    ASSERT_EQ(name->value.GetKind(), NLS::Engine::Serialize::PropertyValue::Kind::String);
    EXPECT_EQ(name->value.GetString(), "AppliedCrate");
}

TEST(PrefabEditorWorkflowTests, AppliesSelectedComponentOverrideWithObjectRecord)
{
    NLS::Engine::GameObject root("Lamp", "Prop");
    auto artifact = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &root,
        {},
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("8a8a8a8a-8a8a-4a8a-8a8a-8a8a8a8a8a8a")),
        "Assets/Prefabs/Lamp.prefab"
    }).artifact;
    ASSERT_TRUE(artifact.has_value());

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::PrefabEditorWorkflow workflow;
    auto instantiate = workflow.InstantiatePrefab({
        &*artifact,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("8a8a8a8a-8a8a-4a8a-8a8a-8a8a8a8a8a8a")),
        "prefab:Lamp"
    }, scene);
    ASSERT_TRUE(instantiate.instance.has_value());
    instantiate.instance->instanceRoot->AddComponent<NLS::Engine::Components::LightComponent>()->SetIntensity(6.0f);
    const auto overrides = workflow.DiscoverOverrides(*artifact, *instantiate.instance);
    const auto* componentOverride = FindOverride(
        overrides,
        NLS::Engine::Serialize::PatchOperationType::InsertOwned,
        "components");
    ASSERT_NE(componentOverride, nullptr);

    const auto result = workflow.ApplySelectedOverride(*artifact, *componentOverride);

    ASSERT_EQ(result.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);
    EXPECT_NE(
        FindObjectRecord(
            artifact->graph,
            "NLS::Engine::Components::LightComponent",
            "NLS::Engine::Components::LightComponent"),
        nullptr);
    const auto* components = FindRootProperty(*artifact, "components");
    ASSERT_NE(components, nullptr);
    ASSERT_EQ(components->value.GetKind(), NLS::Engine::Serialize::PropertyValue::Kind::Array);
    EXPECT_EQ(components->value.GetArray().size(), 2u);
}

TEST(PrefabEditorWorkflowTests, AppliesAllChildHierarchyOverridesAndRollsBackFailedApply)
{
    NLS::Engine::GameObject root("Assembly", "Prop");
    auto artifact = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &root,
        {},
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("8b8b8b8b-8b8b-4b8b-8b8b-8b8b8b8b8b8b")),
        "Assets/Prefabs/Assembly.prefab"
    }).artifact;
    ASSERT_TRUE(artifact.has_value());

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::PrefabEditorWorkflow workflow;
    auto instantiate = workflow.InstantiatePrefab({
        &*artifact,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("8b8b8b8b-8b8b-4b8b-8b8b-8b8b8b8b8b8b")),
        "prefab:Assembly"
    }, scene);
    ASSERT_TRUE(instantiate.instance.has_value());
    NLS::Engine::GameObject child("Socket", "Part");
    child.SetParent(*instantiate.instance->instanceRoot);
    const auto overrides = workflow.DiscoverOverrides(*artifact, *instantiate.instance);
    child.DetachFromParent();

    const auto result = workflow.ApplyAllOverrides(*artifact, overrides);

    ASSERT_EQ(result.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);
    const auto* children = FindRootProperty(*artifact, "children");
    ASSERT_NE(children, nullptr);
    ASSERT_EQ(children->value.GetKind(), NLS::Engine::Serialize::PropertyValue::Kind::Array);
    ASSERT_EQ(children->value.GetArray().size(), 1u);
    const auto* childRecord = FindObjectRecord(artifact->graph, "Socket", "NLS::Engine::GameObject");
    ASSERT_NE(childRecord, nullptr);
    const auto* parent = FindProperty(*childRecord, "parent");
    ASSERT_NE(parent, nullptr);
    ASSERT_EQ(parent->value.GetKind(), NLS::Engine::Serialize::PropertyValue::Kind::ObjectReference);
    const auto resolvedParent = artifact->graph.ResolveObjectReference(parent->value.GetObjectReference());
    ASSERT_TRUE(resolvedParent.has_value());
    EXPECT_EQ(*resolvedParent, artifact->graph.root);

    const auto nameBeforeFailure = ReadStringProperty(*FindObjectRecord(artifact->graph, artifact->graph.root), "name");
    NLS::Editor::Assets::PrefabOverrideRecord invalid;
    invalid.patch = NLS::Engine::Serialize::PatchOperation::ReplaceProperty(
        NLS::Engine::Serialize::ObjectId(NLS::Guid::Parse("8c8c8c8c-8c8c-4c8c-8c8c-8c8c8c8c8c8c")),
        "name",
        NLS::Engine::Serialize::PropertyValue::String("ShouldNotCommit"));
    const auto failed = workflow.ApplySelectedOverride(*artifact, invalid);

    EXPECT_EQ(failed.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Failed);
    EXPECT_EQ(ReadStringProperty(*FindObjectRecord(artifact->graph, artifact->graph.root), "name"), nameBeforeFailure);
}

TEST(PrefabEditorWorkflowTests, AppliesRemovedChildOverrideWithoutInstantiatingRemovedObject)
{
    NLS::Engine::GameObject root("Assembly", "Prop");
    NLS::Engine::GameObject child("Socket", "Part");
    child.SetParent(root);
    auto artifact = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &root,
        {},
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("8b8b8b8b-8b8b-4b8b-9b8b-8b8b8b8b8b8b")),
        "Assets/Prefabs/Assembly.prefab"
    }).artifact;
    child.DetachFromParent();
    ASSERT_TRUE(artifact.has_value());

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::PrefabEditorWorkflow workflow;
    auto instantiate = workflow.InstantiatePrefab({
        &*artifact,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("8b8b8b8b-8b8b-4b8b-9b8b-8b8b8b8b8b8b")),
        "prefab:Assembly"
    }, scene);
    ASSERT_TRUE(instantiate.instance.has_value());
    auto* socket = FindChildByName(*instantiate.instance->instanceRoot, "Socket");
    ASSERT_NE(socket, nullptr);
    socket->DetachFromParent();

    const auto overrides = workflow.DiscoverOverrides(*artifact, *instantiate.instance);
    const auto result = workflow.ApplyAllOverrides(*artifact, overrides);

    ASSERT_EQ(result.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);
    EXPECT_FALSE(artifact->Validate().HasErrors());

    NLS::Engine::SceneSystem::Scene instantiatedScene;
    auto afterApply = workflow.InstantiatePrefab({
        &*artifact,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("8b8b8b8b-8b8b-4b8b-9b8b-8b8b8b8b8b8b")),
        "prefab:Assembly"
    }, instantiatedScene);

    ASSERT_EQ(afterApply.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);
    ASSERT_TRUE(afterApply.instance.has_value());
    EXPECT_EQ(FindChildByName(*afterApply.instance->instanceRoot, "Socket"), nullptr);
}

TEST(PrefabEditorWorkflowTests, RevertsSelectedOverrideWhilePreservingSiblingOverrides)
{
    NLS::Engine::GameObject root("Crate", "Prop");
    auto artifact = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &root,
        {},
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("8d8d8d8d-8d8d-4d8d-8d8d-8d8d8d8d8d8d")),
        "Assets/Prefabs/Crate.prefab"
    }).artifact;
    ASSERT_TRUE(artifact.has_value());

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::PrefabEditorWorkflow workflow;
    auto instantiate = workflow.InstantiatePrefab({
        &*artifact,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("8d8d8d8d-8d8d-4d8d-8d8d-8d8d8d8d8d8d")),
        "prefab:Crate"
    }, scene);
    ASSERT_TRUE(instantiate.instance.has_value());
    instantiate.instance->localPatches.push_back(NLS::Engine::Serialize::PatchOperation::ReplaceProperty(
        artifact->graph.root,
        "name",
        NLS::Engine::Serialize::PropertyValue::String("LocalName")));
    instantiate.instance->localPatches.push_back(NLS::Engine::Serialize::PatchOperation::ReplaceProperty(
        artifact->graph.root,
        "tag",
        NLS::Engine::Serialize::PropertyValue::String("LocalTag")));

    const auto result = workflow.RevertSelectedOverride(
        *instantiate.instance,
        instantiate.instance->localPatches.front());

    ASSERT_EQ(result.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);
    ASSERT_EQ(instantiate.instance->localPatches.size(), 1u);
    EXPECT_EQ(instantiate.instance->localPatches.front().property, "tag");
}

TEST(PrefabEditorWorkflowTests, RejectsUnknownSelectedOverrideWithoutMutatingLiveInstance)
{
    NLS::Engine::GameObject root("Crate", "Prop");
    auto artifact = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &root,
        {},
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("8d8d8d8d-8d8d-4d8d-8d8d-8d8d8d8d8d8e")),
        "Assets/Prefabs/Crate.prefab"
    }).artifact;
    ASSERT_TRUE(artifact.has_value());

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::PrefabEditorWorkflow workflow;
    auto instantiate = workflow.InstantiatePrefab({
        &*artifact,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("8d8d8d8d-8d8d-4d8d-8d8d-8d8d8d8d8d8e")),
        "prefab:Crate"
    }, scene);
    ASSERT_TRUE(instantiate.instance.has_value());
    instantiate.instance->instanceRoot->SetName("EditedCrate");

    const auto missingPatch = NLS::Engine::Serialize::PatchOperation::ReplaceProperty(
        artifact->graph.root,
        "name",
        NLS::Engine::Serialize::PropertyValue::String("EditedCrate"));
    const auto result = workflow.RevertSelectedOverride(*instantiate.instance, missingPatch);

    EXPECT_EQ(result.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Rejected);
    EXPECT_TRUE(HasDiagnosticCode(result, "prefab-revert-override-not-found"));
    EXPECT_EQ(instantiate.instance->instanceRoot->GetName(), "EditedCrate");
    EXPECT_TRUE(instantiate.instance->localPatches.empty());
}

TEST(PrefabEditorWorkflowTests, RevertsFreshLivePropertyOverrideWhenLocalPatchHasStaleValue)
{
    NLS::Engine::GameObject root("Crate", "Prop");
    auto artifact = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &root,
        {},
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("8d8d8d8d-8d8d-4d8d-8d8d-8d8d8d8d8d8f")),
        "Assets/Prefabs/Crate.prefab"
    }).artifact;
    ASSERT_TRUE(artifact.has_value());

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::PrefabEditorWorkflow workflow;
    auto instantiate = workflow.InstantiatePrefab({
        &*artifact,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("8d8d8d8d-8d8d-4d8d-8d8d-8d8d8d8d8d8f")),
        "prefab:Crate"
    }, scene);
    ASSERT_TRUE(instantiate.instance.has_value());
    instantiate.instance->localPatches.push_back(NLS::Engine::Serialize::PatchOperation::ReplaceProperty(
        artifact->graph.root,
        "name",
        NLS::Engine::Serialize::PropertyValue::String("StaleName")));
    instantiate.instance->instanceRoot->SetName("FreshName");

    const auto overrides = workflow.DiscoverOverrides(*artifact, *instantiate.instance);
    const auto* nameOverride = FindOverride(
        overrides,
        NLS::Engine::Serialize::PatchOperationType::ReplaceProperty,
        "name");
    ASSERT_NE(nameOverride, nullptr);
    ASSERT_EQ(nameOverride->patch.value.GetKind(), NLS::Engine::Serialize::PropertyValue::Kind::String);
    ASSERT_EQ(nameOverride->patch.value.GetString(), "FreshName");

    const auto result = workflow.RevertSelectedOverride(*instantiate.instance, nameOverride->patch);

    EXPECT_EQ(result.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);
    EXPECT_EQ(instantiate.instance->instanceRoot->GetName(), "Crate");
    EXPECT_TRUE(instantiate.instance->localPatches.empty());
}

TEST(PrefabEditorWorkflowTests, RevertingPropertyOverrideRemovesDuplicateLocalPatchIdentities)
{
    NLS::Engine::GameObject root("Crate", "Prop");
    auto artifact = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &root,
        {},
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("8d8d8d8d-8d8d-4d8d-8d8d-8d8d8d8d8d90")),
        "Assets/Prefabs/Crate.prefab"
    }).artifact;
    ASSERT_TRUE(artifact.has_value());

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::PrefabEditorWorkflow workflow;
    auto instantiate = workflow.InstantiatePrefab({
        &*artifact,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("8d8d8d8d-8d8d-4d8d-8d8d-8d8d8d8d8d90")),
        "prefab:Crate"
    }, scene);
    ASSERT_TRUE(instantiate.instance.has_value());
    instantiate.instance->localPatches.push_back(NLS::Engine::Serialize::PatchOperation::ReplaceProperty(
        artifact->graph.root,
        "name",
        NLS::Engine::Serialize::PropertyValue::String("StaleNameA")));
    instantiate.instance->localPatches.push_back(NLS::Engine::Serialize::PatchOperation::ReplaceProperty(
        artifact->graph.root,
        "name",
        NLS::Engine::Serialize::PropertyValue::String("StaleNameB")));
    instantiate.instance->instanceRoot->SetName("FreshName");

    const auto overrides = workflow.DiscoverOverrides(*artifact, *instantiate.instance);
    const auto* nameOverride = FindOverride(
        overrides,
        NLS::Engine::Serialize::PatchOperationType::ReplaceProperty,
        "name");
    ASSERT_NE(nameOverride, nullptr);

    const auto result = workflow.RevertSelectedOverride(*instantiate.instance, nameOverride->patch);

    EXPECT_EQ(result.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);
    EXPECT_EQ(instantiate.instance->instanceRoot->GetName(), "Crate");
    EXPECT_TRUE(instantiate.instance->localPatches.empty());
}

TEST(PrefabEditorWorkflowTests, RevertsAllOverridesFromInstance)
{
    NLS::Engine::GameObject root("Crate", "Prop");
    auto artifact = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &root,
        {},
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("8e8e8e8e-8e8e-4e8e-8e8e-8e8e8e8e8e8e")),
        "Assets/Prefabs/Crate.prefab"
    }).artifact;
    ASSERT_TRUE(artifact.has_value());

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::PrefabEditorWorkflow workflow;
    auto instantiate = workflow.InstantiatePrefab({
        &*artifact,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("8e8e8e8e-8e8e-4e8e-8e8e-8e8e8e8e8e8e")),
        "prefab:Crate"
    }, scene);
    ASSERT_TRUE(instantiate.instance.has_value());
    instantiate.instance->localPatches.push_back(NLS::Engine::Serialize::PatchOperation::ReplaceProperty(
        artifact->graph.root,
        "name",
        NLS::Engine::Serialize::PropertyValue::String("LocalName")));
    instantiate.instance->localPatches.push_back(NLS::Engine::Serialize::PatchOperation::ReplaceProperty(
        artifact->graph.root,
        "tag",
        NLS::Engine::Serialize::PropertyValue::String("LocalTag")));

    const auto result = workflow.RevertAllOverrides(*instantiate.instance);

    ASSERT_EQ(result.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);
    EXPECT_TRUE(instantiate.instance->localPatches.empty());
}

TEST(PrefabEditorWorkflowTests, RevertsStructuralOverridesFromLiveInstance)
{
    NLS::Engine::GameObject root("Assembly", "Prop");
    NLS::Engine::GameObject sourceChild("Socket", "Part");
    sourceChild.SetParent(root);
    auto artifact = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &root,
        {},
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("8e8e8e8e-8e8e-4e8e-9e8e-8e8e8e8e8e8e")),
        "Assets/Prefabs/Assembly.prefab"
    }).artifact;
    sourceChild.DetachFromParent();
    ASSERT_TRUE(artifact.has_value());

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::PrefabEditorWorkflow workflow;
    auto instantiate = workflow.InstantiatePrefab({
        &*artifact,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("8e8e8e8e-8e8e-4e8e-9e8e-8e8e8e8e8e8e")),
        "prefab:Assembly"
    }, scene);
    ASSERT_TRUE(instantiate.instance.has_value());
    auto* existingSocket = FindChildByName(*instantiate.instance->instanceRoot, "Socket");
    ASSERT_NE(existingSocket, nullptr);
    existingSocket->DetachFromParent();
    auto* addedLight = instantiate.instance->instanceRoot->AddComponent<NLS::Engine::Components::LightComponent>();
    ASSERT_NE(addedLight, nullptr);
    NLS::Engine::GameObject addedChild("Extra", "Part");
    addedChild.SetParent(*instantiate.instance->instanceRoot);

    const auto overrides = workflow.DiscoverOverrides(*artifact, *instantiate.instance);
    for (const auto& overrideRecord : overrides)
        instantiate.instance->localPatches.push_back(overrideRecord.patch);

    const auto result = workflow.RevertAllOverrides(*instantiate.instance);

    addedChild.DetachFromParent();
    ASSERT_EQ(result.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);
    EXPECT_TRUE(instantiate.instance->localPatches.empty());
    EXPECT_EQ(instantiate.instance->instanceRoot->GetComponent<NLS::Engine::Components::LightComponent>(), nullptr);
    EXPECT_NE(FindChildByName(*instantiate.instance->instanceRoot, "Socket"), nullptr);
    EXPECT_EQ(FindChildByName(*instantiate.instance->instanceRoot, "Extra"), nullptr);
}

TEST(PrefabEditorWorkflowTests, RevertsSelectedNestedOverrideWithoutRemovingOuterOverride)
{
    NLS::Engine::GameObject root("Root", "Prop");
    NLS::Engine::GameObject child("Child", "Part");
    child.SetParent(root);
    auto artifact = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &root,
        {},
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("8f8f8f8f-8f8f-4f8f-8f8f-8f8f8f8f8f8f")),
        "Assets/Prefabs/Root.prefab"
    }).artifact;
    child.DetachFromParent();
    ASSERT_TRUE(artifact.has_value());
    const auto* rootChildren = FindRootProperty(*artifact, "children");
    ASSERT_NE(rootChildren, nullptr);
    ASSERT_EQ(rootChildren->value.GetKind(), NLS::Engine::Serialize::PropertyValue::Kind::Array);
    ASSERT_EQ(rootChildren->value.GetArray().size(), 1u);
    const auto childSourceId = rootChildren->value.GetArray()[0].GetObjectId();

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::PrefabEditorWorkflow workflow;
    auto instantiate = workflow.InstantiatePrefab({
        &*artifact,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("8f8f8f8f-8f8f-4f8f-8f8f-8f8f8f8f8f8f")),
        "prefab:Root"
    }, scene);
    ASSERT_TRUE(instantiate.instance.has_value());
    const auto outerPatch = NLS::Engine::Serialize::PatchOperation::ReplaceProperty(
        artifact->graph.root,
        "name",
        NLS::Engine::Serialize::PropertyValue::String("OuterName"));
    const auto nestedPatch = NLS::Engine::Serialize::PatchOperation::ReplaceProperty(
        childSourceId,
        "name",
        NLS::Engine::Serialize::PropertyValue::String("NestedName"));
    instantiate.instance->localPatches.push_back(outerPatch);
    instantiate.instance->localPatches.push_back(nestedPatch);

    const auto result = workflow.RevertSelectedOverride(*instantiate.instance, nestedPatch);

    ASSERT_EQ(result.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);
    ASSERT_EQ(instantiate.instance->localPatches.size(), 1u);
    EXPECT_EQ(instantiate.instance->localPatches.front().target, artifact->graph.root);
    EXPECT_EQ(instantiate.instance->localPatches.front().property, "name");
}

TEST(PrefabEditorWorkflowTests, OpensSavesAndDiscardsEditablePrefabStage)
{
    NLS::Engine::GameObject root("Workbench", "Prop");
    auto artifact = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &root,
        {},
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("90909090-9090-4090-8090-909090909090")),
        "Assets/Prefabs/Workbench.prefab"
    }).artifact;
    ASSERT_TRUE(artifact.has_value());
    const auto originalRootId = artifact->graph.root;

    NLS::Editor::Assets::PrefabEditorWorkflow workflow;
    auto open = workflow.OpenPrefabStage({
        &*artifact,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("90909090-9090-4090-8090-909090909090")),
        "prefab:Workbench",
        false
    });

    ASSERT_EQ(open.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);
    ASSERT_TRUE(open.stage.has_value());
    EXPECT_TRUE(open.stage->editable);
    EXPECT_FALSE(open.stage->dirty);
    ASSERT_NE(open.stage->stageRoot, nullptr);
    open.stage->stageRoot->SetName("SavedWorkbench");
    workflow.MarkStageDirty(*open.stage);
    EXPECT_TRUE(open.stage->dirty);

    const auto save = workflow.SavePrefabStage(*open.stage, *artifact);

    ASSERT_EQ(save.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);
    EXPECT_FALSE(open.stage->dirty);
    EXPECT_EQ(artifact->graph.root, originalRootId);
    const auto* savedName = FindRootProperty(*artifact, "name");
    ASSERT_NE(savedName, nullptr);
    EXPECT_EQ(savedName->value.GetString(), "SavedWorkbench");

    open.stage->stageRoot->SetName("DiscardedName");
    workflow.MarkStageDirty(*open.stage);
    const auto discard = workflow.DiscardPrefabStage(*open.stage);

    EXPECT_EQ(discard.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);
    EXPECT_FALSE(open.stage->dirty);
    EXPECT_EQ(open.stage->stageRoot->GetName(), "SavedWorkbench");
}

TEST(PrefabEditorWorkflowTests, SavingVariantPrefabStagePreservesBaseReference)
{
    NLS::Engine::GameObject root("BaseCrate", "Prop");
    NLS::Editor::Assets::PrefabEditorWorkflow workflow;
    auto base = workflow.CreatePrefabFromSelection({
        &root,
        {},
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("90909090-9090-4090-9090-909090909090")),
        "Assets/Prefabs/BaseCrate.prefab"
    }).artifact;
    ASSERT_TRUE(base.has_value());

    auto variant = workflow.CreateEditableVariant({
        &*base,
        base->assetId,
        "prefab:BaseCrate",
        "Assets/Prefabs/BaseCrateVariant.prefab",
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("90909090-9090-4090-9190-909090909090")),
        false,
        false
    });
    ASSERT_EQ(variant.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);
    ASSERT_TRUE(variant.artifact.has_value());
    ASSERT_TRUE(variant.artifact->graph.basePrefab.has_value());

    auto open = workflow.OpenPrefabStage({
        &*variant.artifact,
        variant.artifact->assetId,
        "prefab:BaseCrateVariant",
        false
    });
    ASSERT_EQ(open.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);
    ASSERT_TRUE(open.stage.has_value());
    ASSERT_NE(open.stage->stageRoot, nullptr);
    open.stage->stageRoot->SetName("EditedVariant");
    workflow.MarkStageDirty(*open.stage);

    const auto save = workflow.SavePrefabStage(*open.stage, *variant.artifact);

    ASSERT_EQ(save.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);
    ASSERT_TRUE(variant.artifact->graph.basePrefab.has_value());
    EXPECT_EQ(variant.artifact->graph.basePrefab->guid, base->assetId.GetGuid());
    ASSERT_EQ(variant.artifact->baseChain.size(), 1u);
    EXPECT_EQ(variant.artifact->baseChain.front(), base->assetId);
}

TEST(PrefabEditorWorkflowTests, RejectsGeneratedReadOnlyPrefabStageSave)
{
    NLS::Engine::GameObject root("ImportedModel", "Model");
    auto artifact = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &root,
        {},
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("91919191-9191-4191-8191-919191919191")),
        "Library/Artifacts/ImportedModel.prefab"
    }).artifact;
    ASSERT_TRUE(artifact.has_value());

    NLS::Editor::Assets::PrefabEditorWorkflow workflow;
    auto open = workflow.OpenPrefabStage({
        &*artifact,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("91919191-9191-4191-8191-919191919191")),
        "prefab:ImportedModel",
        true
    });

    ASSERT_EQ(open.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);
    ASSERT_TRUE(open.stage.has_value());
    EXPECT_FALSE(open.stage->editable);
    EXPECT_TRUE(open.stage->generatedReadOnly);
    open.stage->stageRoot->SetName("ShouldNotSave");
    workflow.MarkStageDirty(*open.stage);

    const auto save = workflow.SavePrefabStage(*open.stage, *artifact);

    EXPECT_EQ(save.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Rejected);
    const auto* name = FindRootProperty(*artifact, "name");
    ASSERT_NE(name, nullptr);
    EXPECT_EQ(name->value.GetString(), "ImportedModel");
}

TEST(PrefabEditorWorkflowTests, CreatesEditableVariantFromSourceAndGeneratedPrefab)
{
    NLS::Engine::GameObject root("BaseCrate", "Prop");
    auto baseArtifact = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &root,
        {},
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("92929292-9292-4292-8292-929292929292")),
        "Assets/Prefabs/BaseCrate.prefab"
    }).artifact;
    ASSERT_TRUE(baseArtifact.has_value());

    NLS::Editor::Assets::PrefabEditorWorkflow workflow;
    const auto sourceVariant = workflow.CreateEditableVariant({
        &*baseArtifact,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("92929292-9292-4292-8292-929292929292")),
        "prefab:BaseCrate",
        "Assets/Prefabs/BaseCrateVariant.prefab",
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("93939393-9393-4393-8393-939393939393")),
        false,
        false
    });

    ASSERT_EQ(sourceVariant.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);
    ASSERT_TRUE(sourceVariant.artifact.has_value());
    ASSERT_TRUE(sourceVariant.artifact->graph.basePrefab.has_value());
    EXPECT_EQ(sourceVariant.artifact->graph.basePrefab->guid.ToString(), "92929292-9292-4292-8292-929292929292");
    EXPECT_EQ(sourceVariant.artifact->graph.basePrefab->filePath, "prefab:BaseCrate");
    ASSERT_EQ(sourceVariant.artifact->baseChain.size(), 1u);
    EXPECT_EQ(sourceVariant.artifact->baseChain.front().GetGuid().ToString(), "92929292-9292-4292-8292-929292929292");
    EXPECT_EQ(sourceVariant.createdPrefabAssetId.GetGuid().ToString(), "93939393-9393-4393-8393-939393939393");
    EXPECT_EQ(sourceVariant.createdPrefabPath, std::filesystem::path("Assets/Prefabs/BaseCrateVariant.prefab"));

    const auto generatedVariant = workflow.CreateEditableVariant({
        &*baseArtifact,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("92929292-9292-4292-8292-929292929292")),
        "prefab:GeneratedModel",
        "Assets/Prefabs/GeneratedModelVariant.prefab",
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("94949494-9494-4494-8494-949494949494")),
        true,
        false
    });

    ASSERT_EQ(generatedVariant.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);
    ASSERT_TRUE(generatedVariant.artifact.has_value());
    ASSERT_TRUE(generatedVariant.artifact->graph.basePrefab.has_value());
    EXPECT_EQ(generatedVariant.artifact->graph.basePrefab->filePath, "prefab:GeneratedModel");
}

TEST(PrefabEditorWorkflowTests, RejectsEditableVariantDestinationConflict)
{
    NLS::Engine::GameObject root("BaseCrate", "Prop");
    auto baseArtifact = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &root,
        {},
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("95959595-9595-4595-8595-959595959595")),
        "Assets/Prefabs/BaseCrate.prefab"
    }).artifact;
    ASSERT_TRUE(baseArtifact.has_value());

    NLS::Editor::Assets::PrefabEditorWorkflow workflow;
    const auto result = workflow.CreateEditableVariant({
        &*baseArtifact,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("95959595-9595-4595-8595-959595959595")),
        "prefab:BaseCrate",
        "Assets/Prefabs/Existing.prefab",
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("96969696-9696-4696-8696-969696969696")),
        false,
        true
    });

    EXPECT_EQ(result.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Rejected);
    EXPECT_TRUE(HasDiagnosticCode(result, "prefab-variant-destination-conflict"));
    EXPECT_FALSE(result.artifact.has_value());
}

TEST(PrefabEditorWorkflowTests, AggregatesNestedPrefabDiagnostics)
{
    NLS::Engine::Assets::PrefabArtifact parent;
    parent.assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("97979797-9797-4797-8797-979797979797"));
    parent.graph.format = "Nullus.ObjectGraph.Prefab";
    parent.graph.version = 1;
    parent.graph.documentId = NLS::Guid::NewDeterministic("Editor.Nested.Parent.Document");
    parent.graph.root = NLS::Engine::Serialize::ObjectId(NLS::Guid::NewDeterministic("Editor.Nested.Parent.Root"));
    parent.graph.objects.push_back({
        parent.graph.root,
        "NLS::Engine::GameObject",
        "Parent",
        "",
        NLS::Engine::Serialize::ObjectRecordState::Alive,
        {
            {"name", NLS::Engine::Serialize::PropertyValue::String("Parent")},
            {"tag", NLS::Engine::Serialize::PropertyValue::String({})},
            {"components", NLS::Engine::Serialize::PropertyValue::Array({})},
            {"children", NLS::Engine::Serialize::PropertyValue::Array({})},
            {"parent", NLS::Engine::Serialize::PropertyValue::Null()},
            {"nestedPrefab", NLS::Engine::Serialize::PropertyValue::ObjectReference(
                NLS::Engine::Serialize::ObjectIdentifier::Asset(
                    NLS::Engine::Serialize::AssetId(NLS::Guid::Parse("98989898-9898-4898-8898-989898989898")),
                    NLS::Engine::Serialize::MakeLocalIdentifierInFile(
                        NLS::Guid::Parse("98989898-9898-4898-8898-989898989898"),
                        "prefab:Missing"),
                    "prefab:Missing"))}
        },
        NLS::Engine::Serialize::MakeLocalIdentifierInFile(parent.graph.root)
    });

    const auto result = NLS::Editor::Assets::PrefabEditorWorkflow().ValidateNestedPrefabs({parent});

    EXPECT_EQ(result.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Failed);
    EXPECT_TRUE(HasDiagnosticCode(result, "prefab-nested-dependency-diagnostic"));
}

TEST(PrefabEditorWorkflowTests, UnpacksPrefabInstanceToSceneOwnedObjects)
{
    NLS::Engine::GameObject root("Crate", "Prop");
    auto artifact = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &root,
        {},
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("99999999-9999-4999-8999-999999999999")),
        "Assets/Prefabs/Crate.prefab"
    }).artifact;
    ASSERT_TRUE(artifact.has_value());

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::PrefabEditorWorkflow workflow;
    auto instantiate = workflow.InstantiatePrefab({
        &*artifact,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("99999999-9999-4999-8999-999999999999")),
        "prefab:Crate"
    }, scene);
    ASSERT_TRUE(instantiate.instance.has_value());

    const auto result = workflow.UnpackPrefabInstance(*instantiate.instance);

    ASSERT_EQ(result.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);
    ASSERT_TRUE(result.unpack.has_value());
    EXPECT_EQ(result.unpack->root, instantiate.instance->instanceRoot);
    EXPECT_FALSE(result.unpack->sceneOwnedObjects.empty());
    EXPECT_FALSE(instantiate.instance->prefabAssetId.IsValid());
    EXPECT_TRUE(instantiate.instance->prefabSubAssetKey.empty());
    EXPECT_TRUE(instantiate.instance->sourceToInstance.empty());
    EXPECT_TRUE(instantiate.instance->localPatches.empty());
}

TEST(PrefabEditorWorkflowTests, UnpacksGeneratedModelPrefabInstancePreservingAssetReferences)
{
    NLS::Engine::Assets::PrefabArtifact artifact;
    artifact.assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("9a9a9a9a-9a9a-4a9a-8a9a-9a9a9a9a9a9a"));
    artifact.graph.format = "Nullus.ObjectGraph.Prefab";
    artifact.graph.version = 1;
    artifact.graph.documentId = NLS::Guid::NewDeterministic("Generated.Unpack.Document");
    artifact.graph.root = NLS::Engine::Serialize::ObjectId(NLS::Guid::NewDeterministic("Generated.Unpack.Root"));
    artifact.graph.objects.push_back({
        artifact.graph.root,
        "NLS::Engine::GameObject",
        "GeneratedModel",
        "",
        NLS::Engine::Serialize::ObjectRecordState::Alive,
        {
            {"name", NLS::Engine::Serialize::PropertyValue::String("GeneratedModel")},
            {"tag", NLS::Engine::Serialize::PropertyValue::String("Model")},
            {"components", NLS::Engine::Serialize::PropertyValue::Array({})},
            {"children", NLS::Engine::Serialize::PropertyValue::Array({})},
            {"parent", NLS::Engine::Serialize::PropertyValue::Null()},
            {"material", NLS::Engine::Serialize::PropertyValue::ObjectReference(
                NLS::Engine::Serialize::ObjectIdentifier::Asset(
                    NLS::Engine::Serialize::AssetId(NLS::Guid::Parse("9b9b9b9b-9b9b-4b9b-8b9b-9b9b9b9b9b9b")),
                    NLS::Engine::Serialize::MakeLocalIdentifierInFile(
                        NLS::Guid::Parse("9b9b9b9b-9b9b-4b9b-8b9b-9b9b9b9b9b9b"),
                        "material:Body"),
                    "material:Body"))}
        },
        NLS::Engine::Serialize::MakeLocalIdentifierInFile(artifact.graph.root)
    });
    artifact.resolvedAssets.push_back({
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("9b9b9b9b-9b9b-4b9b-8b9b-9b9b9b9b9b9b")),
        "Material",
        "material:Body",
        {}
    });

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::PrefabEditorWorkflow workflow;
    auto instantiate = workflow.InstantiatePrefab({
        &artifact,
        artifact.assetId,
        "prefab:GeneratedModel"
    }, scene);
    ASSERT_TRUE(instantiate.instance.has_value());
    ASSERT_EQ(instantiate.instance->preservedResolvedAssets.size(), 1u);

    const auto result = workflow.UnpackPrefabInstance(*instantiate.instance);

    ASSERT_EQ(result.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);
    ASSERT_TRUE(result.unpack.has_value());
    ASSERT_EQ(result.unpack->preservedAssetReferences.size(), 1u);
    EXPECT_EQ(result.unpack->preservedAssetReferences.front().guid.ToString(), "9b9b9b9b-9b9b-4b9b-8b9b-9b9b9b9b9b9b");
    EXPECT_EQ(result.unpack->preservedAssetReferences.front().filePath, "material:Body");
    EXPECT_TRUE(instantiate.instance->preservedResolvedAssets.empty())
        << "Unpacked scene-owned objects must not retain prefab-instance renderer resource ownership metadata.";
    ASSERT_TRUE(result.instance.has_value());
    EXPECT_TRUE(result.instance->preservedResolvedAssets.empty());
}

TEST(PrefabEditorWorkflowTests, AggregatesEditorDiagnosticsAcrossPrefabOperations)
{
    NLS::Editor::Assets::PrefabEditorWorkflow workflow;

    auto missingBase = workflow.CreateEditableVariant({
        nullptr,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("9c9c9c9c-9c9c-4c9c-8c9c-9c9c9c9c9c9c")),
        "prefab:Missing",
        "Assets/Prefabs/MissingVariant.prefab",
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("9d9d9d9d-9d9d-4d9d-8d9d-9d9d9d9d9d9d")),
        false,
        false
    });

    NLS::Engine::Assets::PrefabArtifact broken;
    broken.assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("9e9e9e9e-9e9e-4e9e-8e9e-9e9e9e9e9e9e"));
    broken.graph.format = "Nullus.ObjectGraph.Prefab";
    broken.graph.version = 1;
    broken.graph.documentId = NLS::Guid::NewDeterministic("Editor.Diagnostics.Broken.Document");
    broken.graph.root = NLS::Engine::Serialize::ObjectId(NLS::Guid::NewDeterministic("Editor.Diagnostics.Broken.Root"));
    broken.graph.objects.push_back({
        broken.graph.root,
        "NLS::Engine::UnknownPrefabOnlyRecord",
        "Unknown",
        "",
        NLS::Engine::Serialize::ObjectRecordState::Alive,
        {
            {"asset", NLS::Engine::Serialize::PropertyValue::ObjectReference(
                NLS::Engine::Serialize::ObjectIdentifier::Asset(
                    NLS::Engine::Serialize::AssetId(NLS::Guid::Parse("9f9f9f9f-9f9f-4f9f-8f9f-9f9f9f9f9f9f")),
                    NLS::Engine::Serialize::MakeLocalIdentifierInFile(
                        NLS::Guid::Parse("9f9f9f9f-9f9f-4f9f-8f9f-9f9f9f9f9f9f"),
                        "material:Missing"),
                    "material:Missing"))},
            {"nestedPrefab", NLS::Engine::Serialize::PropertyValue::ObjectReference(
                NLS::Engine::Serialize::ObjectIdentifier::Asset(
                    NLS::Engine::Serialize::AssetId(NLS::Guid::Parse("a0a0a0a0-a0a0-4a0a-8a0a-a0a0a0a0a0a0")),
                    NLS::Engine::Serialize::MakeLocalIdentifierInFile(
                        NLS::Guid::Parse("a0a0a0a0-a0a0-4a0a-8a0a-a0a0a0a0a0a0"),
                        "prefab:Missing"),
                    "prefab:Missing"))}
        },
        NLS::Engine::Serialize::MakeLocalIdentifierInFile(broken.graph.root)
    });

    NLS::Editor::Assets::PrefabOverrideRecord invalidOverride;
    invalidOverride.patch = NLS::Engine::Serialize::PatchOperation::ReplaceProperty(
        NLS::Engine::Serialize::ObjectId(NLS::Guid::Parse("a1a1a1a1-a1a1-4a1a-8a1a-a1a1a1a1a1a1")),
        "name",
        NLS::Engine::Serialize::PropertyValue::String("Invalid"));
    auto invalidApply = workflow.ApplySelectedOverride(broken, invalidOverride);
    auto nested = workflow.ValidateNestedPrefabs({broken});

    const auto diagnostics = workflow.AggregatePrefabEditorDiagnostics({
        missingBase,
        invalidApply,
        nested
    }, {broken});

    EXPECT_TRUE(HasDiagnosticCode(diagnostics, "prefab-variant-missing-base"));
    EXPECT_TRUE(HasDiagnosticCode(diagnostics, "prefab-apply-override-failed"));
    EXPECT_TRUE(HasDiagnosticCode(diagnostics, "prefab-nested-dependency-diagnostic"));
    EXPECT_TRUE(HasDiagnosticCode(diagnostics, "prefab-unresolved-asset-reference"));
    EXPECT_TRUE(HasDiagnosticCode(diagnostics, "prefab-unknown-editor-record"));
}

TEST(PrefabEditorWorkflowTests, AssetBrowserPrefabCommandSurfaceExposesCreateOpenVariantAndUnpack)
{
    using namespace NLS::Editor::Assets;

    const auto prefabAsset = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("a2a2a2a2-a2a2-4a2a-8a2a-a2a2a2a2a2a2"));

    EditorAssetDatabase database;
    PrefabCommandSurfaceRequest sourceRequest;
    sourceRequest.surface = PrefabCommandSurface::AssetBrowser;
    sourceRequest.subject = PrefabCommandSubject::SourcePrefabAsset;
    sourceRequest.prefabAssetId = prefabAsset;
    sourceRequest.prefabSubAssetKey = "prefab:Crate";
    sourceRequest.assetExists = true;
    const auto sourceCommands = database.GetPrefabCommandSurface(sourceRequest);

    EXPECT_TRUE(HasCommand(sourceCommands, "prefab.create-from-selection", true));
    EXPECT_TRUE(HasCommand(sourceCommands, "prefab.open", true));
    EXPECT_TRUE(HasCommand(sourceCommands, "prefab.create-variant", true));
    EXPECT_TRUE(HasCommand(sourceCommands, "prefab.unpack", false));

    database.RegisterGeneratedPrefab({
        prefabAsset,
        "prefab:GeneratedModel",
        GeneratedPrefabEditPolicy::ReadOnlyGenerated
    });
    PrefabCommandSurfaceRequest generatedRequest;
    generatedRequest.surface = PrefabCommandSurface::AssetBrowser;
    generatedRequest.subject = PrefabCommandSubject::GeneratedModelPrefabAsset;
    generatedRequest.prefabAssetId = prefabAsset;
    generatedRequest.prefabSubAssetKey = "prefab:GeneratedModel";
    generatedRequest.assetExists = true;
    generatedRequest.generatedReadOnly = true;
    const auto generatedCommands = database.GetPrefabCommandSurface(generatedRequest);

    EXPECT_TRUE(HasCommand(generatedCommands, "prefab.open", true));
    EXPECT_TRUE(HasCommand(generatedCommands, "prefab.create-variant", true));
    EXPECT_TRUE(HasCommand(generatedCommands, "prefab.unpack", true));
    EXPECT_TRUE(HasCommand(generatedCommands, "prefab.apply-overrides", false));
}

TEST(PrefabEditorWorkflowTests, InspectorPrefabCommandSurfaceRequiresEditableSourceContextForApply)
{
    using namespace NLS::Editor::Assets;

    const auto prefabAsset = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("a3a3a3a3-a3a3-4a3a-8a3a-a3a3a3a3a3a3"));

    EditorAssetDatabase database;
    PrefabCommandSurfaceRequest instanceRequest;
    instanceRequest.surface = PrefabCommandSurface::Inspector;
    instanceRequest.subject = PrefabCommandSubject::PrefabInstance;
    instanceRequest.prefabAssetId = prefabAsset;
    instanceRequest.prefabSubAssetKey = "prefab:Crate";
    instanceRequest.assetExists = true;
    instanceRequest.connectedInstance = true;
    instanceRequest.overrideCount = 3u;
    const auto instanceCommands = database.GetPrefabCommandSurface(instanceRequest);

    EXPECT_TRUE(HasCommand(instanceCommands, "prefab.apply-overrides", false));
    EXPECT_TRUE(HasCommand(instanceCommands, "prefab.revert-overrides", true));
    EXPECT_TRUE(HasCommand(instanceCommands, "prefab.unpack", true));
    EXPECT_TRUE(HasCommand(instanceCommands, "prefab.open", true));

    instanceRequest.editableSourceArtifactContext = true;
    const auto editableSourceCommands = database.GetPrefabCommandSurface(instanceRequest);
    EXPECT_TRUE(HasCommand(editableSourceCommands, "prefab.apply-overrides", true));

    PrefabCommandSurfaceRequest missingRequest;
    missingRequest.surface = PrefabCommandSurface::Inspector;
    missingRequest.subject = PrefabCommandSubject::MissingPrefabInstance;
    missingRequest.connectedInstance = true;
    missingRequest.overrideCount = 1u;
    const auto missingCommands = database.GetPrefabCommandSurface(missingRequest);

    EXPECT_TRUE(HasCommand(missingCommands, "prefab.apply-overrides", false));
    EXPECT_TRUE(HasCommand(missingCommands, "prefab.revert-overrides", true));
    EXPECT_TRUE(HasCommand(missingCommands, "prefab.unpack", true));
    EXPECT_TRUE(HasCommand(missingCommands, "prefab.open", false));
}
