#include <gtest/gtest.h>

#include "Assets/EditorAssetDatabase.h"
#include "Engine/Assets/ModelPrefabBuilder.h"
#include "Engine/Assets/RuntimeAssetDatabase.h"
#include "Rendering/SceneRendererMaterialBinding.h"
#include "Guid.h"
#include "Rendering/Assets/MaterialConversion.h"
#include "Rendering/Assets/SceneImportPipeline.h"

#include <cstdint>
#include <string>
#include <vector>

namespace
{
using NLS::Core::Assets::ArtifactManifest;
using NLS::Core::Assets::ArtifactType;
using NLS::Core::Assets::AssetId;
using NLS::Core::Assets::ImportedArtifact;
using NLS::Engine::Assets::RuntimeAssetDatabase;
using NLS::Engine::Assets::RuntimeManifestBuilder;
using NLS::Engine::Rendering::SceneRendererMaterialArtifactBinding;
using NLS::Render::Assets::ImportedScene;
using NLS::Render::Assets::ImportedSceneNamedRecord;
using NLS::Render::Assets::ImportedScenePrimitive;
using NLS::Render::Assets::MaterialSourceModel;

AssetId MakeAssetId(const char* guid)
{
    return AssetId(NLS::Guid::Parse(guid));
}

ImportedArtifact MakeArtifact(
    AssetId owner,
    std::string subAssetKey,
    ArtifactType artifactType,
    std::string loaderId,
    std::string artifactPath)
{
    return {
        owner,
        std::move(subAssetKey),
        artifactType,
        std::move(loaderId),
        "editor-windows",
        std::move(artifactPath),
        "sha256:" + owner.ToString()
    };
}

std::string MakeTestArtifactPath(const AssetId owner, const std::string& subAssetKey)
{
    return (std::filesystem::path("Library") /
        "Artifacts" /
        NLS::Core::Assets::BuildArtifactStorageRelativePath(
            NLS::Core::Assets::BuildArtifactStorageFileName(owner.ToString() + ":" + subAssetKey))).generic_string();
}

const SceneRendererMaterialArtifactBinding* FindSceneBinding(
    const std::vector<SceneRendererMaterialArtifactBinding>& bindings,
    const std::string& rendererDebugName,
    const uint32_t slotIndex)
{
    for (const auto& binding : bindings)
    {
        if (binding.rendererDebugName == rendererDebugName && binding.slotIndex == slotIndex)
            return &binding;
    }
    return nullptr;
}

const NLS::Editor::Assets::EditorMaterialViewportBinding* FindEditorBinding(
    const std::vector<NLS::Editor::Assets::EditorMaterialViewportBinding>& bindings,
    const std::string& rendererDebugName,
    const uint32_t slotIndex)
{
    for (const auto& binding : bindings)
    {
        if (binding.rendererDebugName == rendererDebugName && binding.slotIndex == slotIndex)
            return &binding;
    }
    return nullptr;
}

ArtifactManifest MakeGeneratedModelManifest(
    const ImportedScene& scene,
    const std::vector<std::string>& materialSubAssetKeys)
{
    ArtifactManifest manifest;
    manifest.sourceAssetId = scene.sourceAssetId;
    manifest.importerId = "scene-model";
    manifest.targetPlatform = "editor-windows";
    manifest.primarySubAssetKey = "prefab:" + scene.sceneKey;
    manifest.subAssets.push_back(MakeArtifact(
        scene.sourceAssetId,
        "prefab:" + scene.sceneKey,
        ArtifactType::Prefab,
        "prefab",
        MakeTestArtifactPath(scene.sourceAssetId, "prefab:" + scene.sceneKey)));

    for (const auto& mesh : scene.meshes)
    {
        manifest.subAssets.push_back(MakeArtifact(
            scene.sourceAssetId,
            "mesh:" + mesh.sourceKey,
            ArtifactType::Mesh,
            "mesh",
            MakeTestArtifactPath(scene.sourceAssetId, "mesh:" + mesh.sourceKey)));
    }

    for (const auto& materialKey : materialSubAssetKeys)
    {
        manifest.subAssets.push_back(MakeArtifact(
            scene.sourceAssetId,
            materialKey,
            ArtifactType::Material,
            "material",
            MakeTestArtifactPath(scene.sourceAssetId, materialKey)));
    }

    return manifest;
}

RuntimeAssetDatabase MakeRuntimeDatabase(const ArtifactManifest& manifest)
{
    RuntimeManifestBuilder builder;
    builder.AddArtifactManifest(manifest);
    const auto result = builder.Build({{manifest.sourceAssetId, manifest.primarySubAssetKey}}, "editor-windows");
    EXPECT_FALSE(result.diagnostics.HasErrors());
    return RuntimeAssetDatabase(result.manifest);
}

ImportedScene MakeSingleMaterialScene(
    const char* guid,
    std::string sceneKey,
    std::string meshKey,
    std::string materialKey,
    std::string materialName)
{
    ImportedScene scene;
    scene.sourceAssetId = MakeAssetId(guid);
    scene.sceneKey = std::move(sceneKey);
    scene.nodes.push_back({"node/root", "Root", "", std::move(meshKey), ""});

    ImportedScenePrimitive primitive;
    primitive.materialKey = materialKey;

    ImportedSceneNamedRecord mesh;
    mesh.sourceKey = scene.nodes.front().meshKey;
    mesh.name = "Body";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));

    ImportedSceneNamedRecord material;
    material.sourceKey = std::move(materialKey);
    material.name = std::move(materialName);
    material.materialChannels.push_back({"diffuse", {}, {0.8, 0.4, 0.2, 1.0}, false, 0.0});
    scene.materials.push_back(std::move(material));
    return scene;
}
}

TEST(AssetMaterialViewportTests, PreviewAndSceneBindingsResolveConvertedMaterialArtifactsForGltfFbxAndObj)
{
    struct Fixture
    {
        const char* guid;
        const char* sceneKey;
        const char* meshKey;
        const char* materialKey;
        MaterialSourceModel sourceModel;
    };

    const Fixture fixtures[] = {
        {
            "a1010101-0101-4101-8101-010101010101",
            "GltfHero",
            "mesh/0",
            "material/0",
            MaterialSourceModel::GltfPbrMetallicRoughness
        },
        {
            "a2010101-0101-4101-8101-010101010101",
            "FbxHero",
            "parser/mesh/0",
            "parser/material/0",
            MaterialSourceModel::FbxParserMaterial
        },
        {
            "a3010101-0101-4101-8101-010101010101",
            "ObjHero",
            "parser/mesh/0",
            "parser/material/0",
            MaterialSourceModel::ObjMtl
        }
    };

    for (const auto& fixture : fixtures)
    {
        auto scene = MakeSingleMaterialScene(
            fixture.guid,
            fixture.sceneKey,
            fixture.meshKey,
            fixture.materialKey,
            "BodyMaterial");
        const auto converted = NLS::Render::Assets::ConvertImportedSceneMaterial(
            scene,
            scene.materials.front(),
            fixture.sourceModel);
        const auto manifest = MakeGeneratedModelManifest(scene, {converted.subAssetKey});
        const auto prefab = NLS::Engine::Assets::BuildGeneratedModelPrefab(
            scene,
            NLS::Render::Assets::GenerateSceneSubAssets(scene),
            manifest);
        ASSERT_FALSE(prefab.diagnostics.HasErrors()) << fixture.sceneKey;
        const auto runtimeDatabase = MakeRuntimeDatabase(manifest);

        const auto sceneBindings = NLS::Engine::Rendering::ResolveSceneRendererMaterialBindings(
            prefab.artifact,
            runtimeDatabase);
        const auto* sceneBinding = FindSceneBinding(sceneBindings, "Root MeshRenderer", 0u);
        ASSERT_NE(sceneBinding, nullptr) << fixture.sceneKey;
        EXPECT_TRUE(sceneBinding->resolved) << fixture.sceneKey;
        EXPECT_EQ(sceneBinding->reference.assetId, scene.sourceAssetId);
        EXPECT_EQ(sceneBinding->reference.subAssetKey, converted.subAssetKey);
        EXPECT_EQ(sceneBinding->loaderId, "material");

        const NLS::Editor::Assets::EditorAssetDatabase editorDatabase;
        const auto previewBindings = editorDatabase.GetMaterialPreviewBindings(
            prefab.artifact,
            runtimeDatabase);
        const auto* previewBinding = FindEditorBinding(previewBindings, "Root MeshRenderer", 0u);
        ASSERT_NE(previewBinding, nullptr) << fixture.sceneKey;
        EXPECT_TRUE(previewBinding->resolved) << fixture.sceneKey;
        EXPECT_EQ(previewBinding->reference, sceneBinding->reference);
        EXPECT_EQ(previewBinding->artifactPath, sceneBinding->artifactPath);
    }
}

TEST(AssetMaterialViewportTests, GeneratedModelPrefabUsesMeshPrimitiveMaterialSlotsForViewportBindings)
{
    ImportedScene scene;
    scene.sourceAssetId = MakeAssetId("b1010101-0101-4101-8101-010101010101");
    scene.sceneKey = "MultiMaterialHero";
    scene.nodes.push_back({"node/root", "Hero", "", "", ""});
    scene.nodes.push_back({"node/body", "Body", "node/root", "mesh/body", ""});
    scene.nodes.push_back({"node/blade", "Blade", "node/root", "mesh/blade", ""});

    ImportedScenePrimitive bodyPrimitive;
    bodyPrimitive.materialKey = "material/body";
    ImportedSceneNamedRecord bodyMesh;
    bodyMesh.sourceKey = "mesh/body";
    bodyMesh.name = "BodyMesh";
    bodyMesh.primitives.push_back(std::move(bodyPrimitive));
    scene.meshes.push_back(std::move(bodyMesh));

    ImportedScenePrimitive bladePrimitive;
    bladePrimitive.materialKey = "material/blade";
    ImportedSceneNamedRecord bladeMesh;
    bladeMesh.sourceKey = "mesh/blade";
    bladeMesh.name = "BladeMesh";
    bladeMesh.primitives.push_back(std::move(bladePrimitive));
    scene.meshes.push_back(std::move(bladeMesh));

    scene.materials.push_back({"material/body", "BodyPaint"});
    scene.materials.push_back({"material/blade", "BladeSteel"});

    const auto bodyMaterial = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        scene.materials[0],
        MaterialSourceModel::GltfPbrMetallicRoughness);
    const auto bladeMaterial = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        scene.materials[1],
        MaterialSourceModel::GltfPbrMetallicRoughness);
    const auto manifest = MakeGeneratedModelManifest(
        scene,
        {bodyMaterial.subAssetKey, bladeMaterial.subAssetKey});
    const auto prefab = NLS::Engine::Assets::BuildGeneratedModelPrefab(
        scene,
        NLS::Render::Assets::GenerateSceneSubAssets(scene),
        manifest);
    ASSERT_FALSE(prefab.diagnostics.HasErrors());
    const auto runtimeDatabase = MakeRuntimeDatabase(manifest);

    const auto bindings = NLS::Engine::Rendering::ResolveSceneRendererMaterialBindings(
        prefab.artifact,
        runtimeDatabase);

    ASSERT_EQ(bindings.size(), 2u);
    const auto* bodyBinding = FindSceneBinding(bindings, "Body MeshRenderer", 0u);
    ASSERT_NE(bodyBinding, nullptr);
    EXPECT_TRUE(bodyBinding->resolved);
    EXPECT_EQ(bodyBinding->reference.subAssetKey, "material:material/body");

    EXPECT_EQ(FindSceneBinding(bindings, "Blade MeshRenderer", 0u), nullptr);

    const auto* bladeBinding = FindSceneBinding(bindings, "Blade MeshRenderer", 1u);
    ASSERT_NE(bladeBinding, nullptr);
    EXPECT_TRUE(bladeBinding->resolved);
    EXPECT_EQ(bladeBinding->reference.subAssetKey, "material:material/blade");
}

TEST(AssetMaterialViewportTests, PrimitiveWithoutMaterialDoesNotFallbackToEverySceneMaterial)
{
    ImportedScene scene;
    scene.sourceAssetId = MakeAssetId("b2010101-0101-4101-8101-010101010101");
    scene.sceneKey = "UnassignedMaterialHero";
    scene.nodes.push_back({"node/body", "Body", "", "mesh/body", ""});

    ImportedSceneNamedRecord mesh;
    mesh.sourceKey = "mesh/body";
    mesh.name = "BodyMesh";
    mesh.primitives.push_back(ImportedScenePrimitive {});
    scene.meshes.push_back(std::move(mesh));
    scene.materials.push_back({"material/body", "BodyPaint"});

    const auto converted = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        scene.materials.front(),
        MaterialSourceModel::GltfPbrMetallicRoughness);
    const auto manifest = MakeGeneratedModelManifest(scene, {converted.subAssetKey});
    const auto prefab = NLS::Engine::Assets::BuildGeneratedModelPrefab(
        scene,
        NLS::Render::Assets::GenerateSceneSubAssets(scene),
        manifest);
    ASSERT_FALSE(prefab.diagnostics.HasErrors());
    const auto runtimeDatabase = MakeRuntimeDatabase(manifest);

    const auto bindings = NLS::Engine::Rendering::ResolveSceneRendererMaterialBindings(
        prefab.artifact,
        runtimeDatabase);

    EXPECT_TRUE(bindings.empty());
}

TEST(AssetMaterialViewportTests, MeshWithoutPrimitiveDetailsDoesNotFallbackToEverySceneMaterial)
{
    ImportedScene scene;
    scene.sourceAssetId = MakeAssetId("b3010101-0101-4101-8101-010101010101");
    scene.sceneKey = "NoPrimitiveDetailHero";
    scene.nodes.push_back({"node/body", "Body", "", "mesh/body", ""});

    ImportedSceneNamedRecord mesh;
    mesh.sourceKey = "mesh/body";
    mesh.name = "BodyMesh";
    scene.meshes.push_back(std::move(mesh));
    scene.materials.push_back({"material/body", "BodyPaint"});
    scene.materials.push_back({"material/trim", "TrimPaint"});

    const auto bodyMaterial = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        scene.materials[0],
        MaterialSourceModel::GltfPbrMetallicRoughness);
    const auto trimMaterial = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        scene.materials[1],
        MaterialSourceModel::GltfPbrMetallicRoughness);
    const auto manifest = MakeGeneratedModelManifest(
        scene,
        {bodyMaterial.subAssetKey, trimMaterial.subAssetKey});
    const auto prefab = NLS::Engine::Assets::BuildGeneratedModelPrefab(
        scene,
        NLS::Render::Assets::GenerateSceneSubAssets(scene),
        manifest);
    ASSERT_FALSE(prefab.diagnostics.HasErrors());
    const auto runtimeDatabase = MakeRuntimeDatabase(manifest);

    const auto bindings = NLS::Engine::Rendering::ResolveSceneRendererMaterialBindings(
        prefab.artifact,
        runtimeDatabase);

    EXPECT_TRUE(bindings.empty());
}
