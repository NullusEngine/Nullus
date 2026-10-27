#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <chrono>
#include <thread>
#include <cstring>
#include <Guid.h>
#include <Json/json.hpp>
#include <imgui.h>

#include <Core/Assets/NativeArtifactContainer.h>
#include <Core/ServiceLocator.h>
#include <Core/ResourceManagement/ShaderManager.h>
#include <Core/ResourceManagement/MaterialManager.h>
#include <Core/ResourceManagement/TextureManager.h>
#include <Engine/Rendering/ParticleSpriteMaterial.h>
#include <Math/Vector2.h>
#include <Math/Vector4.h>
#include <Engine/Assets/ParticleSystemAsset.h>
#include <Engine/Assets/RuntimeAssetDatabase.h>
#include <Engine/Components/ParticleSystemComponent.h>
#include <Engine/SceneSystem/Scene.h>
#include <Engine/Serialize/ObjectGraphSerializer.h>
#include <Engine/Serialize/ObjectGraphInstantiator.h>
#include <VFX/CompiledSystem.h>
#include "Assets/AssetDatabaseFacade.h"
#include "Assets/EditorAssetPath.h"
#include "Assets/AssetThumbnailService.h"
#include "Core/Context.h"
#include <VFXCompiler/VfxCompiler.h>
#include "VfxStageTestAsset.h"
#include <Rendering/Context/DriverAccess.h>
#include <Rendering/Context/Driver.h>
#include <Rendering/Context/RenderCommandPipe.h>
#include <Rendering/Context/RhiCommandListExecutor.h>
#include <Rendering/RHI/Backends/RHIDeviceFactory.h>
#include <Rendering/Assets/ShaderArtifact.h>
#include <Rendering/Resources/Mesh.h>
#include <Rendering/Settings/DriverSettings.h>

namespace
{
struct TestDirectory
{
    std::filesystem::path root = std::filesystem::temp_directory_path()/("NullusVfxEditor_"+NLS::Guid::New().ToString());
    TestDirectory() { std::filesystem::create_directories(root/"Assets"); }
    ~TestDirectory()
    {
        if (NLS::Core::ServiceLocator::Contains<NLS::Editor::Assets::AssetThumbnailService>())
            NLS::Core::ServiceLocator::Get<NLS::Editor::Assets::AssetThumbnailService>().Shutdown();
        std::error_code error; std::filesystem::remove_all(root,error);
    }
};

struct RuntimeAssetsScope
{
    NLS::Engine::Assets::RuntimeAssetDatabase* previous = nullptr;
    explicit RuntimeAssetsScope(NLS::Engine::Assets::RuntimeAssetDatabase& database)
    {
        using namespace NLS;
        if (Core::ServiceLocator::Contains<Engine::Assets::RuntimeAssetDatabase>())
            previous = &Core::ServiceLocator::Get<Engine::Assets::RuntimeAssetDatabase>();
        Core::ServiceLocator::Provide(database);
    }
    ~RuntimeAssetsScope()
    {
        if (previous) NLS::Core::ServiceLocator::Provide(*previous);
        else NLS::Core::ServiceLocator::Remove<NLS::Engine::Assets::RuntimeAssetDatabase>();
    }
};
}

TEST(VfxEditorTests, SourceImportProducesExecutableNativeArtifactAndInvalidReimportPreservesIt)
{
    using namespace NLS::Editor::Assets;
    TestDirectory directory;
    const auto source = directory.root/"Assets"/"Particles.vfx";
    auto asset = NLS::VFX::CreateDefaultSystem();
    std::string error;
    ASSERT_TRUE(NLS::VFX::SaveSystem(source,asset,error));
    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(directory.root));
    ASSERT_TRUE(database.Refresh());
    const std::string editorPath = "Assets/Particles.vfx";
    const bool imported = database.ImportAsset(editorPath);
    if (!imported)
        for (const auto& diagnostic : database.GetDiagnostics()) ADD_FAILURE() << diagnostic.message;
    ASSERT_TRUE(imported);
    EXPECT_TRUE(database.IsArtifactManifestCurrentForAssetPath(editorPath));
    const auto manifest = database.GetArtifactManifestForAssetPath(editorPath);
    ASSERT_TRUE(manifest.has_value());
    ASSERT_NE(manifest->FindPrimaryArtifact(),nullptr);
    const auto artifact = *manifest->FindPrimaryArtifact();
    EXPECT_EQ(artifact.artifactType,NLS::Core::Assets::ArtifactType::VfxSystem);
    const auto artifactPath = database.ResolveArtifactPathAtPath(editorPath,artifact.subAssetKey);
    const auto payload = NLS::Core::Assets::ReadNativeArtifactPayloadTextFromFile(artifactPath,NLS::Core::Assets::ArtifactType::VfxSystem,1);
    ASSERT_TRUE(payload.has_value()) << artifactPath.string();
    NLS::VFX::CompiledSystem program;
    ASSERT_TRUE(NLS::VFX::DeserializeCompiledSystem(payload->payload,program,error)) << error;
    EXPECT_TRUE(program.asset.emitters[0].update.nodes.empty());
    EXPECT_TRUE(program.emitters[0].spawnHlsl.empty()); EXPECT_TRUE(program.emitters[0].updateHlsl.empty()); EXPECT_TRUE(program.emitters[0].compactionHlsl.empty());
    EXPECT_EQ(program.platforms.size(),3u);
    for (const auto backend : {NLS::Render::RHI::NativeBackendType::DX12,NLS::Render::RHI::NativeBackendType::Vulkan,NLS::Render::RHI::NativeBackendType::Metal})
        EXPECT_TRUE(NLS::VFX::ValidatePlatformPrograms(program,backend,error)) << error;
    EXPECT_TRUE(std::any_of(manifest->dependencies.begin(),manifest->dependencies.end(),[](const auto& dependency)
        { return dependency.kind == NLS::Core::Assets::AssetDependencyKind::PostprocessorVersion && !dependency.hashOrVersion.empty(); }));
    NLS::VFX::CpuEmitter emitter;
    ASSERT_TRUE(emitter.Initialize(program.asset.emitters[0],program.emitters[0].spawn,program.emitters[0].update,error)) << error;
    ASSERT_TRUE(emitter.Tick(0.1));
    EXPECT_EQ(emitter.Data().Count(),10u);

    using namespace NLS::Engine;
    Assets::RuntimeManifestBuilder builder; builder.AddArtifactManifest(*manifest);
    const auto built = builder.Build({{manifest->sourceAssetId,artifact.subAssetKey}},"editor");
    ASSERT_EQ(built.manifest.entries.size(),1u);
    auto runtimeManifest = built.manifest; runtimeManifest.entries[0].artifactPath = artifactPath.string();
    Assets::RuntimeAssetDatabase runtimeAssets(runtimeManifest);
    RuntimeAssetsScope scope(runtimeAssets);
    const auto identifier = Serialize::ObjectIdentifier::Asset(Serialize::AssetId(manifest->sourceAssetId.GetGuid()),
        Serialize::MakeLocalIdentifierInFile(manifest->sourceAssetId.GetGuid(),artifact.subAssetKey));
    const auto loadedAsset = Assets::LoadParticleSystemAsset(identifier,error); ASSERT_NE(loadedAsset,nullptr) << error;
    EXPECT_EQ(Assets::LoadParticleSystemAsset(identifier,error),loadedAsset);
    {
        const auto previous = NLS::Core::ResourceManagement::ShaderManager::ProjectAssetsRoot();
        const auto engineAssets = (std::filesystem::path(NLS_ROOT_DIR)/"App"/"Assets"/"Engine").string();
        NLS::Core::ResourceManagement::ShaderManager::ProvideAssetPaths((directory.root/"Assets").string(),engineAssets);
        auto portableManifest = runtimeManifest;
        portableManifest.entries[0].artifactPath = std::filesystem::relative(artifactPath,directory.root).generic_string();
        runtimeAssets = Assets::RuntimeAssetDatabase(portableManifest);
        EXPECT_EQ(Assets::LoadParticleSystemAsset(identifier,error),loadedAsset) << error;
        runtimeAssets = Assets::RuntimeAssetDatabase(runtimeManifest);
        NLS::Core::ResourceManagement::ShaderManager::ProvideAssetPaths(previous,engineAssets);
    }
    SceneSystem::Scene scene;
    auto& owner = scene.CreateGameObject("Particles");
    auto* component = owner.AddComponent<Components::ParticleSystemComponent>(); ASSERT_NE(component,nullptr);
    component->SetSystemReference(Serialize::PPtr<Assets::ParticleSystemAsset>(
        Serialize::PersistentManager::Instance().ObjectIdentifierToInstanceID(identifier)));
    component->OnStart(); component->OnUpdate(0.1f);
    ASSERT_NE(component->CaptureFrame(),nullptr) << component->LastError();
    EXPECT_EQ(component->CaptureFrame()->cpu[0]->Count(),10u);
    EXPECT_EQ(scene.GetFastAccessComponents().particleSystems.size(),1u);
    auto frozen = component->CaptureFrame();
    component->SetPaused(true); component->OnUpdate(0.5f); EXPECT_FLOAT_EQ(component->GetAge(),0.1f);
    component->SetPaused(false); component->Restart(); EXPECT_FLOAT_EQ(component->GetAge(),0);
    const auto document = Serialize::ObjectGraphSerializer::SerializeScene(scene);
    ASSERT_FALSE(document.Validate().HasErrors());
    bool hasParticleReference = false;
    for (const auto& object : document.objects)
        if (object.typeName == "NLS::Engine::Components::ParticleSystemComponent")
            for (const auto& property : object.properties)
                if (property.name == "system")
                { hasParticleReference = true; EXPECT_EQ(property.value.GetObjectReference(),identifier); }
    EXPECT_TRUE(hasParticleReference);
    auto restoredScene = Serialize::ObjectGraphInstantiator::InstantiateScene(document); ASSERT_NE(restoredScene,nullptr);
    const auto& restoredComponents = restoredScene->GetFastAccessComponents().particleSystems;
    ASSERT_EQ(restoredComponents.size(),1u);
    EXPECT_EQ(restoredComponents[0]->GetSystemReference(),component->GetSystemReference());
    const auto references = Assets::CollectPrefabAssetReferences(document);
    ASSERT_EQ(references.size(),1u); EXPECT_EQ(references[0],identifier);
    const auto resolved = Assets::BuildPrefabResolvedAssetsFromReferences(document);
    ASSERT_EQ(resolved.size(),1u); EXPECT_EQ(resolved[0].expectedType,"ParticleSystem");
    EXPECT_EQ(Assets::PrefabResolvedAssetTypeForArtifactType(NLS::Core::Assets::ArtifactType::VfxSystem),"ParticleSystem");

    // A broken replacement must leave a running component and asset on the last good program.
    const auto runningProgram = component->CaptureFrame()->system;
    ASSERT_TRUE(component->SetFloatParameter("User.Speed",3));
    auto brokenManifest = runtimeManifest; brokenManifest.entries[0].artifactPath = source.string(); brokenManifest.entries[0].contentHash = "broken";
    runtimeAssets = Assets::RuntimeAssetDatabase(brokenManifest);
    component->PrepareForRendering(); EXPECT_EQ(component->CaptureFrame()->system,runningProgram); EXPECT_FALSE(component->LastError().empty());
    EXPECT_EQ(loadedAsset->Program(),runningProgram);
    auto invalid = nlohmann::json::parse(NLS::VFX::SerializeSystem(asset));
    invalid["emitters"][0]["update"]["nodes"][3]["inputs"][0] = 999;
    { std::ofstream file(source); file << invalid.dump(); }
    EXPECT_FALSE(database.ImportAsset(editorPath));
    const auto preserved = database.GetArtifactManifestForAssetPath(editorPath);
    ASSERT_TRUE(preserved.has_value());
    ASSERT_NE(preserved->FindPrimaryArtifact(),nullptr);
    EXPECT_EQ(preserved->FindPrimaryArtifact()->contentHash,artifact.contentHash);
    asset.emitters[0].spawnRate = 200;
    ASSERT_TRUE(NLS::VFX::SaveSystem(source,asset,error)); ASSERT_TRUE(database.ImportAsset(editorPath));
    const auto replacement = database.GetArtifactManifestForAssetPath(editorPath); ASSERT_TRUE(replacement.has_value());
    const auto* replacementArtifact = replacement->FindPrimaryArtifact(); ASSERT_NE(replacementArtifact,nullptr);
    auto replacementManifest = runtimeManifest;
    replacementManifest.entries[0].artifactPath = database.ResolveArtifactPathAtPath(editorPath,replacementArtifact->subAssetKey).string();
    replacementManifest.entries[0].contentHash = replacementArtifact->contentHash;
    runtimeAssets = Assets::RuntimeAssetDatabase(replacementManifest);
    component->PrepareForRendering(); ASSERT_NE(component->CaptureFrame()->system,runningProgram) << component->LastError();
    component->OnUpdate(0.1f); EXPECT_EQ(component->CaptureFrame()->cpu[0]->Count(),20u);
    EXPECT_EQ(component->CaptureFrame()->parameters.at("User.Speed")[0],3);
    EXPECT_EQ(Assets::LoadParticleSystemAsset(identifier,error),loadedAsset);
    EXPECT_EQ(component->GetSystemReference().Get(),loadedAsset.get());
    EXPECT_EQ(frozen->cpu[0]->Count(),10u); EXPECT_EQ(frozen->system,runningProgram);
    component->SetSystemReference({}); EXPECT_EQ(component->CaptureFrame(),nullptr); EXPECT_EQ(component->RenderProxy(),nullptr);
}

TEST(VfxEditorTests, MeshRendererImportTracksTypedDependencyAndPreservesCookOnBadReference)
{
    using namespace NLS;
    TestDirectory directory; const auto templates = std::filesystem::path(NLS_ROOT_DIR)/"Templates/VFX/Meshes";
    for (const auto* name : {"ParticleShard.gltf","ParticleShard.gltf.meta"})
        std::filesystem::copy_file(templates/name,directory.root/"Assets"/name);
    Editor::Assets::AssetDatabaseFacade database(Editor::Assets::MakeProjectEditorAssetRoots(directory.root));
    ASSERT_TRUE(database.Refresh()); ASSERT_TRUE(database.ImportAsset("Assets/ParticleShard.gltf"));
    VFX::AssetReference mesh;
    for (const auto& item : database.LoadAllAssetsAtPath("Assets/ParticleShard.gltf"))
        if (item.artifactType == Core::Assets::ArtifactType::Mesh)
        {
            mesh = {item.assetId.ToString(),Engine::Serialize::MakeLocalIdentifierInFile(item.assetId.GetGuid(),item.subAssetKey)};
            RecordProperty("meshGuid",mesh.guid); RecordProperty("meshLocalId",std::to_string(mesh.localId)); RecordProperty("meshKey",item.subAssetKey);
            break;
        }
    ASSERT_FALSE(mesh.Empty());
    VFX::SystemAsset sample; std::string sampleError;
    ASSERT_TRUE(VFX::LoadSystem(std::filesystem::path(NLS_ROOT_DIR)/"Templates/VFX/MeshShards.vfx",sample,sampleError)) << sampleError;
    ASSERT_EQ(sample.emitters[0].mesh.meshes.size(),1u);
    EXPECT_EQ(sample.emitters[0].mesh.meshes[0].guid,mesh.guid); EXPECT_EQ(sample.emitters[0].mesh.meshes[0].localId,mesh.localId);
    VFX::CompiledSystem sampleProgram; std::vector<VFX::Diagnostic> sampleDiagnostics;
    EXPECT_TRUE(VFX::CompileSystem(sample,sampleProgram,sampleDiagnostics)) << (sampleDiagnostics.empty() ? "" : sampleDiagnostics[0].message);
    auto asset = VFX::CreateDefaultSystem(); asset.emitters[0].renderer = VFX::RendererType::Mesh; asset.emitters[0].mesh.meshes = {mesh};
    const auto source = directory.root/"Assets/MeshParticles.vfx"; std::string error;
    ASSERT_TRUE(VFX::SaveSystem(source,asset,error)); ASSERT_TRUE(database.Refresh()); ASSERT_TRUE(database.ImportAsset("Assets/MeshParticles.vfx"));
    const auto manifest = database.GetArtifactManifestForAssetPath("Assets/MeshParticles.vfx"); ASSERT_TRUE(manifest);
    EXPECT_TRUE(std::any_of(manifest->dependencies.begin(),manifest->dependencies.end(),[&](const auto& dependency)
        { return dependency.kind == Core::Assets::AssetDependencyKind::ImportedArtifact && dependency.value == mesh.guid; }));
    Engine::Assets::RuntimeManifestBuilder builder; builder.AddArtifactManifest(*manifest);
    const auto modelManifest = database.GetArtifactManifestForAssetPath("Assets/ParticleShard.gltf"); ASSERT_TRUE(modelManifest); builder.AddArtifactManifest(*modelManifest);
    const auto runtime = builder.Build({{manifest->sourceAssetId,manifest->primarySubAssetKey}},"editor");
    ASSERT_FALSE(runtime.diagnostics.HasErrors()); EXPECT_EQ(runtime.manifest.entries.size(),2u);
    const auto path = database.ResolveArtifactPathAtPath("Assets/MeshParticles.vfx",manifest->primarySubAssetKey);
    const auto payload = Core::Assets::ReadNativeArtifactPayloadTextFromFile(path,Core::Assets::ArtifactType::VfxSystem,1); ASSERT_TRUE(payload);
    VFX::CompiledSystem program; ASSERT_TRUE(VFX::DeserializeCompiledSystem(payload->payload,program,error));
    EXPECT_EQ(program.asset.emitters[0].renderer,VFX::RendererType::Mesh); EXPECT_EQ(program.asset.emitters[0].mesh.meshes[0].localId,mesh.localId);
    EXPECT_TRUE(program.emitters[0].spawnHlsl.empty());
    for (const auto backend : {Render::RHI::NativeBackendType::DX12,Render::RHI::NativeBackendType::Vulkan,Render::RHI::NativeBackendType::Metal})
    {
        EXPECT_TRUE(VFX::ValidatePlatformPrograms(program,backend,error)) << error;
        EXPECT_EQ(program.FindPlatform(backend)->emitters[0].spriteVertex.entryPoint,"MeshVS");
    }
    for (const auto* name : {"ParticleChip.gltf","ParticleChip.gltf.meta"})
        std::filesystem::copy_file(templates/name,directory.root/"Assets"/name);
    for (const auto* name : {"ParticleMaterial.shader","ParticleMaterial.shader.meta","ParticleMaterial.mat","ParticleMaterial.mat.meta"})
        std::filesystem::copy_file(templates.parent_path()/name,directory.root/"Assets"/name);
    ASSERT_TRUE(database.Refresh());
    for (const auto* name : {"Assets/ParticleChip.gltf","Assets/ParticleMaterial.shader","Assets/ParticleMaterial.mat"})
        ASSERT_TRUE(database.ImportAsset(name)) << name;
    VFX::AssetReference chip;
    for (const auto& item : database.LoadAllAssetsAtPath("Assets/ParticleChip.gltf"))
        if (item.artifactType == Core::Assets::ArtifactType::Mesh)
            chip = {item.assetId.ToString(),Engine::Serialize::MakeLocalIdentifierInFile(item.assetId.GetGuid(),item.subAssetKey)};
    ASSERT_FALSE(chip.Empty());
    const auto material = database.LoadMainAssetAtPath("Assets/ParticleMaterial.mat"); ASSERT_TRUE(material);
    const VFX::AssetReference materialReference{material->assetId.ToString(),Engine::Serialize::MakeLocalIdentifierInFile(material->assetId.GetGuid(),material->subAssetKey)};
    ASSERT_TRUE(VFX::LoadSystem(templates.parent_path()/"MultiMeshShards.vfx",asset,error)) << error;
    ASSERT_EQ(asset.emitters.size(),2u);
    for (auto& emitter : asset.emitters)
    {
        ASSERT_EQ(emitter.mesh.meshes.size(),2u);
        EXPECT_EQ(emitter.mesh.meshes[1].guid,chip.guid); EXPECT_EQ(emitter.mesh.meshes[1].localId,chip.localId);
        emitter.mesh.materials = {{},materialReference}; emitter.mesh.lodIndex = 1;
    }
    ASSERT_TRUE(VFX::SaveSystem(source,asset,error)); ASSERT_TRUE(database.ImportAsset("Assets/MeshParticles.vfx"));
    const auto multiManifest = database.GetArtifactManifestForAssetPath("Assets/MeshParticles.vfx"); ASSERT_TRUE(multiManifest);
    for (const auto& reference : {mesh,chip,materialReference})
        EXPECT_TRUE(std::any_of(multiManifest->dependencies.begin(),multiManifest->dependencies.end(),[&](const auto& dependency)
            { return dependency.kind == Core::Assets::AssetDependencyKind::ImportedArtifact && dependency.value == reference.guid; }));
    Engine::Assets::RuntimeManifestBuilder multiBuilder; multiBuilder.AddArtifactManifest(*multiManifest); multiBuilder.AddArtifactManifest(*modelManifest);
    for (const auto* name : {"Assets/ParticleChip.gltf","Assets/ParticleMaterial.mat","Assets/ParticleMaterial.shader"})
    { const auto dependency = database.GetArtifactManifestForAssetPath(name); ASSERT_TRUE(dependency); multiBuilder.AddArtifactManifest(*dependency); }
    const auto multiRuntime = multiBuilder.Build({{multiManifest->sourceAssetId,multiManifest->primarySubAssetKey}},"editor");
    ASSERT_FALSE(multiRuntime.diagnostics.HasErrors()); EXPECT_EQ(multiRuntime.manifest.entries.size(),5u);
    const auto multiPayload = Core::Assets::ReadNativeArtifactPayloadTextFromFile(
        database.ResolveArtifactPathAtPath("Assets/MeshParticles.vfx",multiManifest->primarySubAssetKey),Core::Assets::ArtifactType::VfxSystem,1);
    ASSERT_TRUE(multiPayload); ASSERT_TRUE(VFX::DeserializeCompiledSystem(multiPayload->payload,program,error)) << error;
    EXPECT_EQ(program.asset.emitters[0].mesh.materials[1].guid,materialReference.guid); EXPECT_EQ(program.asset.emitters[1].mesh.lodIndex,1);
    asset.emitters[0].mesh.materials[1] = chip;
    ASSERT_TRUE(VFX::SaveSystem(source,asset,error)); EXPECT_FALSE(database.ImportAsset("Assets/MeshParticles.vfx"));
    const auto materialRollback = database.GetArtifactManifestForAssetPath("Assets/MeshParticles.vfx"); ASSERT_TRUE(materialRollback);
    EXPECT_EQ(materialRollback->FindPrimaryArtifact()->contentHash,multiManifest->FindPrimaryArtifact()->contentHash);
    asset.emitters[0].mesh.materials[1] = materialReference;
    asset.emitters[0].mesh.meshes[0].localId += 1; ASSERT_TRUE(VFX::SaveSystem(source,asset,error));
    EXPECT_FALSE(database.ImportAsset("Assets/MeshParticles.vfx"));
    const auto preserved = database.GetArtifactManifestForAssetPath("Assets/MeshParticles.vfx"); ASSERT_TRUE(preserved);
    EXPECT_EQ(preserved->FindPrimaryArtifact()->contentHash,multiManifest->FindPrimaryArtifact()->contentHash);
}

TEST(VfxEditorTests, SpriteTextureReferencesSurviveMovesAndEnterTheRuntimeDependencyClosure)
{
    using namespace NLS;
    using namespace NLS::Editor::Assets;
    TestDirectory directory;
    const auto templates = std::filesystem::path(NLS_ROOT_DIR)/"Templates/VFX";
    std::filesystem::copy_file(templates/"Textures/SpriteAtlas.tga",directory.root/"Assets/Atlas.tga");
    std::filesystem::copy_file(templates/"Textures/SpriteAtlas.tga.meta",directory.root/"Assets/Atlas.tga.meta");
    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(directory.root)); ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Atlas.tga"));
    const auto texture = database.LoadMainAssetAtPath("Assets/Atlas.tga"); ASSERT_TRUE(texture);
    ASSERT_EQ(texture->artifactType,Core::Assets::ArtifactType::Texture);
    std::string error; const auto source = directory.root/"Assets/Sprite.vfx";
    VFX::SystemAsset asset; ASSERT_TRUE(VFX::LoadSystem(templates/"AnimatedSpriteAtlas.vfx",asset,error)) << error;
    EXPECT_EQ(asset.emitters[0].sprite.texture.guid,texture->assetId.ToString());
    EXPECT_EQ(asset.emitters[0].sprite.texture.localId,Engine::Serialize::MakeLocalIdentifierInFile(texture->assetId.GetGuid(),texture->subAssetKey));
    ASSERT_TRUE(VFX::SaveSystem(source,asset,error)); ASSERT_TRUE(database.Refresh()); ASSERT_TRUE(database.ImportAsset("Assets/Sprite.vfx"));
    const auto manifest = database.GetArtifactManifestForAssetPath("Assets/Sprite.vfx"); ASSERT_TRUE(manifest);
    const auto textureManifest = database.GetArtifactManifestForAssetPath("Assets/Atlas.tga"); ASSERT_TRUE(textureManifest);
    Engine::Assets::RuntimeManifestBuilder builder; builder.AddArtifactManifest(*manifest); builder.AddArtifactManifest(*textureManifest);
    const auto runtime = builder.Build({{manifest->sourceAssetId,manifest->primarySubAssetKey}},"editor");
    for (const auto& diagnostic : runtime.diagnostics.GetItems())
        if (diagnostic.IsError()) ADD_FAILURE() << diagnostic.code << " asset=" << diagnostic.assetId.ToString() << " subasset=" << diagnostic.subAssetKey << " path=" << diagnostic.path
            << " vfx target=" << manifest->targetPlatform << " texture target=" << textureManifest->targetPlatform;
    ASSERT_FALSE(runtime.diagnostics.HasErrors()); ASSERT_EQ(runtime.manifest.entries.size(),2u);
    const auto vfxEntry = std::find_if(runtime.manifest.entries.begin(),runtime.manifest.entries.end(),[](const auto& entry) { return entry.artifactType == Core::Assets::ArtifactType::VfxSystem; });
    ASSERT_NE(vfxEntry,runtime.manifest.entries.end()); ASSERT_EQ(vfxEntry->dependencies.size(),1u);
    EXPECT_EQ(vfxEntry->dependencies[0].assetId,texture->assetId);
    const auto editorDatabase = NLS::Editor::Core::LoadEditorRuntimeAssetDatabaseForTesting(directory.root,true);
    ASSERT_TRUE(editorDatabase);
    const auto* resolvedTexture = editorDatabase->ResolveByLocalIdentifierInFile(texture->assetId,asset.emitters[0].sprite.texture.localId);
    ASSERT_TRUE(resolvedTexture); EXPECT_EQ(resolvedTexture->artifactType,Core::Assets::ArtifactType::Texture);
    EXPECT_FALSE(Engine::Assets::IsRuntimeAssetPlatformCompatible(Core::Assets::ArtifactType::Texture,"win64-dx12","linux-vulkan"));
    EXPECT_EQ(database.GetDependencies("Assets/Sprite.vfx",false),std::vector<std::string>{"Assets/Atlas.tga"});
    ASSERT_TRUE(database.MoveAsset("Assets/Atlas.tga","Assets/MovedAtlas.tga"));
    ASSERT_TRUE(database.ImportAsset("Assets/Sprite.vfx"));
    const auto moved = database.GetArtifactManifestForAssetPath("Assets/Sprite.vfx"); ASSERT_TRUE(moved);
    // An asset of the wrong type must never replace the last valid executable program.
    asset.emitters[0].sprite.texture = {manifest->sourceAssetId.ToString(),Engine::Serialize::MakeLocalIdentifierInFile(manifest->sourceAssetId.GetGuid(),manifest->primarySubAssetKey)};
    ASSERT_TRUE(VFX::SaveSystem(source,asset,error)); EXPECT_FALSE(database.ImportAsset("Assets/Sprite.vfx"));
    const auto preserved = database.GetArtifactManifestForAssetPath("Assets/Sprite.vfx"); ASSERT_TRUE(preserved);
    EXPECT_EQ(preserved->FindPrimaryArtifact()->contentHash,moved->FindPrimaryArtifact()->contentHash);
}

TEST(VfxEditorTests, ParticleMaterialValuesAreFrozenBeforeCrossingToTheRenderThread)
{
    using namespace NLS;
    Core::ResourceManagement::MaterialManager manager;
    auto* previous = Core::ServiceLocator::Contains<Core::ResourceManagement::MaterialManager>() ? &Core::ServiceLocator::Get<Core::ResourceManagement::MaterialManager>() : nullptr;
    Core::ServiceLocator::Provide(manager);
    struct Restore { Core::ResourceManagement::MaterialManager* previous; ~Restore() { if (previous) Core::ServiceLocator::Provide(*previous); else Core::ServiceLocator::Remove<Core::ResourceManagement::MaterialManager>(); } } restore{previous};
    const auto path = (std::filesystem::temp_directory_path()/"Library/Artifacts/objects/vfx-material-snapshot.nart").string();
    auto* material = new Render::Resources::Material(); material->path = path;
    material->SetRawParameter("u_Diffuse",Maths::Vector4(0.2f,0.4f,0.6f,0.8f));
    material->SetRawParameter("u_TextureTiling",Maths::Vector2(2,3)); material->SetRawParameter("u_TextureOffset",Maths::Vector2(0.1f,0.2f));
    material->SetRawParameter("u_EmissiveIntensity",2.0f); manager.RegisterResource(path,material);
    VFX::SpriteSettings settings; settings.material = {Guid::New().ToString(),37};
    VFX::SpriteMaterial captured; std::string error;
    const Engine::Rendering::SpriteAssetResolver resolver = [&](const auto& ref,auto type,std::string& result,std::string&)
    { EXPECT_EQ(ref.guid,settings.material.guid); EXPECT_EQ(ref.localId,37); EXPECT_EQ(type,Core::Assets::ArtifactType::Material); result = path; return true; };
    ASSERT_TRUE(Engine::Rendering::CaptureParticleSpriteMaterial(settings,captured,error,resolver)) << error;
    EXPECT_EQ(captured.tint,(VFX::Value{0.2f,0.4f,0.6f,0.8f})); EXPECT_EQ(captured.uvTransform,(VFX::Value{2,3,0.1f,0.2f})); EXPECT_EQ(captured.emissive,2);
    material->SetRawParameter("u_Diffuse",Maths::Vector4(1,1,1,1)); manager.UnloadResources();
    EXPECT_EQ(captured.tint,(VFX::Value{0.2f,0.4f,0.6f,0.8f}));
    VFX::SpriteMaterial preserved = captured;
    EXPECT_FALSE(Engine::Rendering::CaptureParticleSpriteMaterial(settings,captured,error));
    EXPECT_EQ(captured.tint,preserved.tint);
}

TEST(VfxEditorTests, ParticleMaterialPassImportsBindsParametersAndLoadsWithoutItsShaderSource)
{
    using namespace NLS;
    TestDirectory directory;
    const auto templates = std::filesystem::path(NLS_ROOT_DIR)/"Templates/VFX";
    for (const auto* file : {"ParticleMaterial.shader","ParticleMaterial.shader.meta","ParticleMaterial.mat","ParticleMaterial.mat.meta","DynamicMaterialSprite.vfx"})
        std::filesystem::copy_file(templates/file,directory.root/"Assets"/file);
    std::filesystem::create_directories(directory.root/"Assets/Textures");
    for (const auto* file : {"SpriteAtlas.tga","SpriteAtlas.tga.meta"})
        std::filesystem::copy_file(templates/"Textures"/file,directory.root/"Assets/Textures"/file);
    NLS::Editor::Assets::AssetDatabaseFacade database(NLS::Editor::Assets::MakeProjectEditorAssetRoots(directory.root));
    ASSERT_TRUE(database.Refresh());
    const auto import = [&](const char* path)
    {
        const bool succeeded = database.ImportAsset(path);
        if (!succeeded) for (const auto& diagnostic : database.GetDiagnostics()) ADD_FAILURE() << diagnostic.message;
        return succeeded;
    };
    ASSERT_TRUE(import("Assets/Textures/SpriteAtlas.tga")); ASSERT_TRUE(import("Assets/ParticleMaterial.shader"));
    ASSERT_TRUE(import("Assets/ParticleMaterial.mat")); ASSERT_TRUE(import("Assets/DynamicMaterialSprite.vfx"));
    const auto shaderManifest = database.GetArtifactManifestForAssetPath("Assets/ParticleMaterial.shader"); ASSERT_TRUE(shaderManifest);
    const auto shaderPath = database.ResolveArtifactPathAtPath("Assets/ParticleMaterial.shader",shaderManifest->primarySubAssetKey);
    const auto shaderArtifact = Render::Assets::LoadShaderArtifact(shaderPath); ASSERT_TRUE(shaderArtifact);
    EXPECT_EQ(shaderArtifact->shaderLabLightMode,"ParticleSprite");
    std::string error; ASSERT_TRUE(Render::Resources::ValidateParticleSpriteReflection(shaderArtifact->reflection,error)) << error;
    EXPECT_EQ(std::count_if(shaderArtifact->stages.begin(),shaderArtifact->stages.end(),[](const auto& stage)
        { return stage.stage == Render::ShaderCompiler::ShaderStage::Pixel && (stage.targetPlatform == Render::ShaderCompiler::ShaderTargetPlatform::DXIL || stage.targetPlatform == Render::ShaderCompiler::ShaderTargetPlatform::SPIRV); }),2);
    const auto manifest = database.GetArtifactManifestForAssetPath("Assets/DynamicMaterialSprite.vfx"); ASSERT_TRUE(manifest);
    Engine::Assets::RuntimeManifestBuilder builder;
    for (const auto* path : {"Assets/DynamicMaterialSprite.vfx","Assets/ParticleMaterial.mat","Assets/ParticleMaterial.shader","Assets/Textures/SpriteAtlas.tga"})
    { const auto item = database.GetArtifactManifestForAssetPath(path); ASSERT_TRUE(item); builder.AddArtifactManifest(*item); }
    const auto runtime = builder.Build({{manifest->sourceAssetId,manifest->primarySubAssetKey}},"editor");
    ASSERT_FALSE(runtime.diagnostics.HasErrors()); ASSERT_EQ(runtime.manifest.entries.size(),4u);
    Engine::Assets::RuntimeAssetDatabase runtimeDatabase(runtime.manifest); RuntimeAssetsScope runtimeScope(runtimeDatabase);
    VFX::SystemAsset asset; const auto source = directory.root/"Assets/DynamicMaterialSprite.vfx";
    ASSERT_TRUE(VFX::LoadSystem(source,asset,error));
    auto settings = asset.emitters[0].sprite; settings.texture = {}; // This capture validates constants without needing a GPU texture upload.
    const auto previousPaths = Core::ResourceManagement::ShaderManager::ProjectAssetsRoot();
    const auto enginePaths = (std::filesystem::path(NLS_ROOT_DIR)/"App/Assets/Engine").string();
    Core::ResourceManagement::ShaderManager shaders; Core::ResourceManagement::MaterialManager materials;
    auto* previousShaders = Core::ServiceLocator::Contains<Core::ResourceManagement::ShaderManager>() ? &Core::ServiceLocator::Get<Core::ResourceManagement::ShaderManager>() : nullptr;
    auto* previousMaterials = Core::ServiceLocator::Contains<Core::ResourceManagement::MaterialManager>() ? &Core::ServiceLocator::Get<Core::ResourceManagement::MaterialManager>() : nullptr;
    Core::ServiceLocator::Provide(shaders); Core::ServiceLocator::Provide(materials);
    shaders.ProvideAssetPaths((directory.root/"Assets").string(),enginePaths); materials.ProvideAssetPaths((directory.root/"Assets").string(),enginePaths);
    struct Restore
    {
        Core::ResourceManagement::ShaderManager* shaders; Core::ResourceManagement::MaterialManager* materials;
        std::string project,engine;
        ~Restore()
        {
            if (shaders) Core::ServiceLocator::Provide(*shaders); else Core::ServiceLocator::Remove<Core::ResourceManagement::ShaderManager>();
            if (materials) Core::ServiceLocator::Provide(*materials); else Core::ServiceLocator::Remove<Core::ResourceManagement::MaterialManager>();
            Core::ResourceManagement::ShaderManager::ProvideAssetPaths(project,engine); Core::ResourceManagement::MaterialManager::ProvideAssetPaths(project,engine);
        }
    } restore{previousShaders,previousMaterials,previousPaths,enginePaths};
    std::filesystem::remove(directory.root/"Assets/ParticleMaterial.shader"); std::filesystem::remove(directory.root/"Assets/ParticleMaterial.mat");
    std::filesystem::remove(directory.root/"Assets/ParticleMaterial.shader.meta"); std::filesystem::remove(directory.root/"Assets/ParticleMaterial.mat.meta");
    const auto artifactDatabasePath = directory.root/"Library/ArtifactDB";
    const auto savedArtifactDatabase = directory.root/"Library/ArtifactDB.capture-test";
    std::filesystem::rename(artifactDatabasePath,savedArtifactDatabase);
    VFX::SpriteMaterial first,second;
    ASSERT_TRUE(Engine::Rendering::CaptureParticleSpriteMaterial(settings,first,error,{},{{"User.Tint",{0.2f,0.4f,0.6f,0.8f}},{"User.Intensity",{2,0,0,0}},{"User.Dynamic1",{3,4,5,6}}})) << error;
    ASSERT_TRUE(first.program); ASSERT_EQ(first.constants.size(),1u); EXPECT_EQ(first.program->fragments.size(),3u);
    std::array<float,5> values{}; ASSERT_GE(first.constants[0].bytes.size(),sizeof(values)); std::memcpy(values.data(),first.constants[0].bytes.data(),sizeof(values));
    EXPECT_EQ(values,(std::array<float,5>{0.2f,0.4f,0.6f,0.8f,2})); EXPECT_EQ(first.dynamicDefaults[1],(VFX::Value{3,4,5,6}));
    ASSERT_TRUE(Engine::Rendering::CaptureParticleSpriteMaterial(settings,second,error,{},{{"User.Intensity",{0.5f,0,0,0}}})) << error;
    EXPECT_EQ(first.program,second.program); EXPECT_NE(first.constants,second.constants);
    settings.materialParameters[0].type = VFX::ValueType::Float;
    const auto preserved = first.constants;
    EXPECT_FALSE(Engine::Rendering::CaptureParticleSpriteMaterial(settings,first,error)); EXPECT_EQ(first.constants,preserved);
    materials.UnloadResources(); shaders.UnloadResources(); EXPECT_EQ(first.program->fragments.size(),3u); EXPECT_EQ(first.constants,preserved);
    std::filesystem::rename(savedArtifactDatabase,artifactDatabasePath);
    std::filesystem::copy_file(templates/"ParticleMaterial.mat",directory.root/"Assets/ParticleMaterial.mat");
    std::filesystem::copy_file(templates/"ParticleMaterial.shader",directory.root/"Assets/ParticleMaterial.shader");
    std::filesystem::copy_file(templates/"ParticleMaterial.mat.meta",directory.root/"Assets/ParticleMaterial.mat.meta");
    std::filesystem::copy_file(templates/"ParticleMaterial.shader.meta",directory.root/"Assets/ParticleMaterial.shader.meta");
    asset.emitters[0].sprite.materialParameters[0].materialParameter = "_Missing";
    ASSERT_TRUE(VFX::SaveSystem(source,asset,error)); EXPECT_FALSE(database.ImportAsset("Assets/DynamicMaterialSprite.vfx"));
    const auto failed = database.GetArtifactManifestForAssetPath("Assets/DynamicMaterialSprite.vfx"); ASSERT_TRUE(failed);
    EXPECT_EQ(failed->FindPrimaryArtifact()->contentHash,manifest->FindPrimaryArtifact()->contentHash);
    // Reject a pass that claims the domain but has no compatible Sprite interface.
    { std::ofstream file(directory.root/"Assets/ParticleMaterial.shader"); file << "Shader \"Broken\" { SubShader { Pass { Tags { \"LightMode\" = \"ParticleSprite\" } HLSLPROGRAM\n#pragma fragment ParticleMaterialPS\nfloat4 ParticleMaterialPS() : SV_Target { return 1; }\nENDHLSL } } }"; }
    EXPECT_FALSE(database.ImportAsset("Assets/ParticleMaterial.shader"));
    const auto preservedShader = database.GetArtifactManifestForAssetPath("Assets/ParticleMaterial.shader"); ASSERT_TRUE(preservedShader);
    EXPECT_EQ(preservedShader->FindPrimaryArtifact()->contentHash,shaderManifest->FindPrimaryArtifact()->contentHash);
}
