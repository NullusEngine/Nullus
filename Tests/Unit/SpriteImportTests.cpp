#include <gtest/gtest.h>
#include "Assets/AssetDatabaseFacade.h"
#include "Assets/EditorAssetDragPayload.h"
#include "Assets/PrefabUtilityFacade.h"
#include "Properties/SpritePropertyDrawers.h"
#include "Inspector/InspectorProjectionBuilder.h"
#include "Inspector/InspectorRegistry.h"
#include "Inspector/ComponentEditor.h"
#include "Scripting/ManagedScriptProjectPaths.h"
#include "Scripting/ManagedScriptBuildService.h"
#include <Scripting/CoreClrScriptBackend.h>
#include <Scripting/ScriptApiManifest.h>
#include "Serialization/SerializedObject.h"
#include <Assets/ArtifactDatabase.h>
#include <Assets/NativeArtifactContainer.h>
#include <Assets/SpriteArtifact.h>
#include <Assets/SpriteSheet.h>
#include <CookTypes.h>
#include <Engine/Assets/RuntimeAssetDatabase.h>
#include <Engine/Components/SpriteRendererComponent.h>
#include <Engine/Components/MeshFilter.h>
#include <Engine/Components/MeshRenderer.h>
#include <Engine/Components/CameraComponent.h>
#include <Engine/GameObject.h>
#include <Engine/AssemblyEngine.h>
#include <Engine/SceneSystem/Scene.h>
#include <Engine/Assets/PrefabSceneExpansion.h>
#include <Engine/Serialize/ObjectGraphSerializer.h>
#include <Engine/Serialize/ObjectGraphInstantiator.h>
#include <Engine/Serialize/ObjectGraphWriter.h>
#include <Engine/Serialize/PPtrResourceTypes.h>
#include <ServiceLocator.h>
#include <Core/ResourceManagement/ShaderManager.h>
#include <Rendering/Assets/TextureArtifact.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unordered_set>

namespace
{
namespace A = NLS::Core::Assets;
namespace E = NLS::Editor::Assets;
struct SpriteImportFixture : testing::Test
{
    std::filesystem::path root, source;
    A::AssetMeta meta;
    void SetUp() override
    {
        root = std::filesystem::path(NLS_BUILD_DIR) / "sprite-import-tests" / NLS::Guid::New().ToString();
        source = root / "Assets/Hero.png";
        std::filesystem::create_directories(source.parent_path());
        std::filesystem::copy_file(std::filesystem::path(NLS_ROOT_DIR) / "Templates/TwoD/content/Assets/Textures/Hero.png", source);
        { std::ofstream defaults(root / "Assets/ImportDefaults.ini"); defaults << "TEXTURE_TYPE=sprite\nSPRITE_PIXELS_PER_UNIT=100\n"; }
        meta = A::AssetMeta::CreateForAsset(source);
        ASSERT_TRUE(A::IsSpriteTexture(meta));
        ASSERT_TRUE(NLS::Guid::TryParse(meta.settings.at("SPRITE_ID")));
        ASSERT_TRUE(meta.Save(A::GetAssetMetaPath(source)));
    }
    static void Diagnostics(const E::AssetDatabaseFacade& database)
    {
        for (const auto& diagnostic : database.GetDiagnostics())
            if (diagnostic.severity == A::AssetDiagnosticSeverity::Error)
                ADD_FAILURE() << diagnostic.code << ": " << diagnostic.message;
    }
    static const A::ImportedArtifact* Sprite(const A::ArtifactManifest& manifest)
    {
        for (const auto& artifact : manifest.subAssets)
            if (artifact.artifactType == A::ArtifactType::Sprite) return &artifact;
        return nullptr;
    }
    static std::optional<A::SpriteArtifact> ReadSprite(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        const std::vector<uint8_t> bytes(std::istreambuf_iterator<char>(file), {});
        const auto native = A::ReadNativeArtifactContainer(bytes, A::ArtifactType::Sprite, A::SpriteArtifactVersion);
        if (!native) return {};
        std::string error;
        return A::DeserializeSpriteArtifact(native->payload, error);
    }
};

TEST_F(SpriteImportFixture, BordersShareSingleAndMultipleEditorCookPayloadsAcrossWarmModeChanges)
{
    E::AssetDatabaseFacade database(E::MakeProjectEditorAssetRoots(root));ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Hero.png"));
    const auto key="sprite:"+meta.settings.at("SPRITE_ID");
    const auto initial=ReadSprite(database.ResolveArtifactPathAtPath("Assets/Hero.png",key));ASSERT_TRUE(initial);
    const std::array<uint32_t,4> border{2,3,4,5};
    const char* settings[]{"SPRITE_BORDER_LEFT","SPRITE_BORDER_BOTTOM","SPRITE_BORDER_RIGHT","SPRITE_BORDER_TOP"};
    for(size_t i=0;i<4;++i)meta.settings[settings[i]]=std::to_string(border[i]);
    NLS::Cook::CookOptions options;options.projectRoot=root;options.assetsRoot=root/"Assets";
    options.artifactDatabasePath=root/"Library/BorderCookDB";options.artifactRoot=root/"Library/Artifacts";
    options.stagingRoot=root/"Library/BorderCookStaging";options.target.artifactPlatform="win64-dx12";
    for(bool multiple:{false,true})
    {
        meta.settings[A::SpriteImportModeSetting]=multiple?"multiple":"single";
        if(multiple)
        {
            const A::SpriteSliceDefinition slice{initial->id,"Bordered slice",0,0,initial->width,initial->height,initial->pivot,initial->pixelsPerUnit,border};
            std::string error;meta.settings[A::SpriteSlicesSetting]=A::SerializeSpriteSlices(std::span(&slice,1),error);
            ASSERT_TRUE(error.empty())<<error;
        }
        ASSERT_TRUE(meta.Save(A::GetAssetMetaPath(source)));ASSERT_TRUE(database.ReimportAsset("Assets/Hero.png"));
        const auto imported=ReadSprite(database.ResolveArtifactPathAtPath("Assets/Hero.png",key));ASSERT_TRUE(imported);
        EXPECT_EQ(imported->border,border);EXPECT_EQ(imported->SubAssetKey(),key);
        const auto editorManifest=database.GetArtifactManifestForAssetPath("Assets/Hero.png");ASSERT_TRUE(editorManifest);
        EXPECT_TRUE(A::HasCurrentSpriteCompilerDependency(*editorManifest,meta));
        auto stale=*editorManifest;
        std::erase_if(stale.dependencies,[](const auto& dependency){return dependency.value==A::SpriteCompilerDependency;});
        E::AssetDatabaseFacade oldCache(E::MakeProjectEditorAssetRoots(root));ASSERT_TRUE(oldCache.Refresh());
        oldCache.AddArtifactManifest(stale);EXPECT_FALSE(oldCache.IsArtifactManifestCurrentForAssetPath("Assets/Hero.png"));
        const auto cooked=NLS::Cook::AssetCooker::CookAssets(options,{source});ASSERT_TRUE(cooked.Succeeded())<<cooked.FormatSummary();
        A::ArtifactDatabase artifacts;ASSERT_TRUE(artifacts.Load(options.artifactDatabasePath));
        const auto manifest=artifacts.BuildManifestForSource(meta.id,"win64-dx12");ASSERT_TRUE(manifest);
        EXPECT_TRUE(A::HasCurrentSpriteCompilerDependency(*manifest,meta));
        const auto* sprite=manifest->FindSubAsset(key);
        if(!sprite)for(const auto& artifact:artifacts.GetRecords())
            ADD_FAILURE()<<"Cooked key="<<artifact.subAssetKey<<" type="<<int(artifact.artifactType)<<" source="<<artifact.sourceAssetId.ToString()<<" target="<<artifact.targetPlatform<<" expected="<<key<<" source="<<meta.id.ToString();
        ASSERT_NE(sprite,nullptr)<<"mode="<<multiple<<" "<<cooked.FormatSummary();
        auto path=std::filesystem::path(sprite->artifactPath);if(path.is_relative())path=root/path;
        const auto loaded=ReadSprite(path);ASSERT_TRUE(loaded);EXPECT_EQ(loaded->border,border);
        EXPECT_EQ(A::SerializeSpriteArtifact(*loaded),A::SerializeSpriteArtifact(*imported));
    }
    // Asset-only fixture for the already existing validation project. It does
    // not contain a project descriptor and is never opened as a new project.
    const auto example=root/"RuntimeBorderExample";std::filesystem::create_directories(example/"Textures");
    std::filesystem::copy_file(source,example/"Textures/BorderValidation.png");
    ASSERT_TRUE(meta.Save(example/"Textures/BorderValidation.png.meta"));
    NLS::Engine::SceneSystem::Scene scene;
    auto& cameraObject=scene.CreateGameObject("Sliced Camera");
    cameraObject.GetTransform()->SetLocalPosition({0,0,2});cameraObject.GetTransform()->SetLocalRotation({0,1,0,0});
    auto* camera=cameraObject.AddComponent<NLS::Engine::Components::CameraComponent>();
    camera->SetProjectionMode(NLS::Render::Settings::EProjectionMode::ORTHOGRAPHIC);camera->SetSize(1.25f);
    camera->SetClearColor({0,0,0});camera->SetClearMode(NLS::Engine::Components::CameraComponent::ECameraClearMode::SOLID_COLOR);
    auto& object=scene.CreateGameObject("Sliced Sprite");
    using SpriteRenderer=NLS::Engine::Components::SpriteRendererComponent;
    const auto reference=NLS::Engine::Serialize::ObjectIdentifier::Asset(NLS::Engine::Serialize::AssetId(meta.id.GetGuid()),
        NLS::Engine::Serialize::MakeLocalIdentifierInFile(meta.id.GetGuid(),key),key);
    auto* renderer=object.AddComponent<SpriteRenderer>();
    renderer->SetSpriteReference(NLS::Engine::Serialize::PPtr<NLS::Engine::Assets::SpriteAsset>(
        NLS::Engine::Serialize::PersistentManager::Instance().ObjectIdentifierToInstanceID(reference)));
    renderer->SetDrawMode(SpriteRenderer::ESpriteDrawMode::Sliced);renderer->SetSize({1.5f,1});
    const auto exampleScene=example/"Sliced.scene";
    {std::ofstream file(exampleScene);file<<NLS::Engine::Serialize::ObjectGraphWriter::Write(
        NLS::Engine::Serialize::ObjectGraphSerializer::SerializeScene(scene));ASSERT_TRUE(file.good());}
    ASSERT_TRUE(A::AssetMeta::CreateForAsset(exampleScene).Save(A::GetAssetMetaPath(exampleScene)));
    renderer->SetDrawMode(SpriteRenderer::ESpriteDrawMode::Tiled);renderer->SetSize({2,1.5f});
    const auto tiledScene=example/"Tiled.scene";
    {std::ofstream file(tiledScene);file<<NLS::Engine::Serialize::ObjectGraphWriter::Write(
        NLS::Engine::Serialize::ObjectGraphSerializer::SerializeScene(scene));ASSERT_TRUE(file.good());}
    ASSERT_TRUE(A::AssetMeta::CreateForAsset(tiledScene).Save(A::GetAssetMetaPath(tiledScene)));
    std::cout<<"[SpriteBorderFixture] "<<example.generic_string()<<'\n';
}

TEST_F(SpriteImportFixture, EditorPublishesSingleMipTextureAndSameLogicalSpriteAsCook)
{
    E::AssetDatabaseFacade database(E::MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());
    const auto imported = database.ImportAsset("Assets/Hero.png"); Diagnostics(database); ASSERT_TRUE(imported);
    const auto manifest = database.GetArtifactManifestForAssetPath("Assets/Hero.png"); ASSERT_TRUE(manifest);
    EXPECT_EQ(manifest->primarySubAssetKey, "texture:main");
    const auto* sprite = Sprite(*manifest); ASSERT_TRUE(sprite);
    NLS::Engine::Assets::RuntimeManifestBuilder builder;builder.AddArtifactManifest(*manifest);
    const auto runtime=builder.Build({{meta.id,sprite->subAssetKey}},"editor");
    EXPECT_EQ(runtime.manifest.entries.size(),2);
    const auto texture = NLS::Render::Assets::LoadTextureArtifact(database.ResolveArtifactPathAtPath("Assets/Hero.png", "texture:main"));
    ASSERT_TRUE(texture); EXPECT_EQ(texture->mips.size(), 1); EXPECT_EQ(texture->subresources.size(), 1);
    const auto logical = ReadSprite(database.ResolveArtifactPathAtPath("Assets/Hero.png", sprite->subAssetKey)); ASSERT_TRUE(logical);
    EXPECT_EQ(logical->id, meta.settings.at("SPRITE_ID")); EXPECT_EQ(logical->textureAssetId, meta.id);

    const auto cookRoot=root/"CookProject";
    std::filesystem::create_directories(cookRoot/"Assets");
    std::filesystem::copy_file(source,cookRoot/"Assets/Hero.png");
    std::filesystem::copy_file(A::GetAssetMetaPath(source),cookRoot/"Assets/Hero.png.meta");
    NLS::Cook::CookOptions options; options.projectRoot=cookRoot; options.assetsRoot=cookRoot/"Assets";
    options.artifactDatabasePath=cookRoot/"Library/ArtifactDB"; options.artifactRoot=cookRoot/"Library/Artifacts"; options.stagingRoot=cookRoot/"Library/Staging";
    options.target.artifactPlatform="win64-dx12";
    const auto cooked=NLS::Cook::AssetCooker::CookAssets(options,{cookRoot/"Assets/Hero.png"}); ASSERT_TRUE(cooked.Succeeded()) << cooked.FormatSummary();
    A::ArtifactDatabase cookedDatabase; ASSERT_TRUE(cookedDatabase.Load(options.artifactDatabasePath));
    const auto cookedManifest=cookedDatabase.BuildManifestForSource(meta.id,"win64-dx12"); ASSERT_TRUE(cookedManifest);
    const auto* cookedSprite=Sprite(*cookedManifest);
    if(!cookedSprite)
    {
        for(const auto& record:cookedDatabase.GetRecords())
            ADD_FAILURE()<<"Cook record source="<<record.sourceAssetId.ToString()<<" key="<<record.subAssetKey<<" target="<<record.targetPlatform;
        for(const auto& artifact:cookedManifest->subAssets)
            ADD_FAILURE()<<"Cook artifact "<<artifact.subAssetKey<<" type="<<int(artifact.artifactType)<<" expected Sprite="<<int(A::ArtifactType::Sprite);
    }
    ASSERT_TRUE(cookedSprite)<<cooked.FormatSummary()<<" subAssets="<<cookedManifest->subAssets.size(); EXPECT_EQ(cookedSprite->subAssetKey,sprite->subAssetKey);
    auto cookedPath=std::filesystem::path(cookedSprite->artifactPath);
    if(cookedPath.is_relative()) cookedPath=cookRoot/cookedPath;
    const auto cookedLogical=ReadSprite(cookedPath); ASSERT_TRUE(cookedLogical);
    EXPECT_EQ(A::SerializeSpriteArtifact(*logical),A::SerializeSpriteArtifact(*cookedLogical));
}

TEST_F(SpriteImportFixture, ExtractionSharesSpriteResolutionAndNextExtractionPublishesACompleteReload)
{
    namespace P = NLS::Engine::Assets;
    namespace S = NLS::Engine::Serialize;
    namespace C = NLS::Engine::Components;
    E::AssetDatabaseFacade database(E::MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh()); ASSERT_TRUE(database.ImportAsset("Assets/Hero.png"));
    auto manifest = database.GetArtifactManifestForAssetPath("Assets/Hero.png"); ASSERT_TRUE(manifest);
    ASSERT_TRUE(Sprite(*manifest)); const auto key = Sprite(*manifest)->subAssetKey;
    const auto makeRuntime = [&]
    {
        P::RuntimeManifestBuilder builder; builder.AddArtifactManifest(*manifest);
        return P::RuntimeAssetDatabase(builder.Build({{meta.id,key}},"editor").manifest);
    };
    auto runtime = makeRuntime();
    using Services = NLS::Core::ServiceLocator;
    using Shaders = NLS::Core::ResourceManagement::ShaderManager;
    struct Restore
    {
        P::RuntimeAssetDatabase* previous = Services::Contains<P::RuntimeAssetDatabase>() ? &Services::Get<P::RuntimeAssetDatabase>() : nullptr;
        std::string project = Shaders::ProjectAssetsRoot(), engine = Shaders::EngineAssetsRoot();
        ~Restore()
        {
            if (previous) Services::Provide(*previous); else Services::Remove<P::RuntimeAssetDatabase>();
            Shaders::ProvideAssetPaths(project,engine);
        }
    } restore;
    Services::Provide(runtime); Shaders::ProvideAssetPaths((root/"Assets").string(),restore.engine);
    const auto identifier = S::ObjectIdentifier::Asset(S::AssetId(meta.id.GetGuid()),
        S::MakeLocalIdentifierInFile(meta.id.GetGuid(),key));
    const S::PPtr<P::SpriteAsset> reference(S::PersistentManager::Instance().ObjectIdentifierToInstanceID(identifier));
    NLS::Engine::GameObject first("First"), second("Second");
    auto* left = first.AddComponent<C::SpriteRendererComponent>();
    auto* right = second.AddComponent<C::SpriteRendererComponent>();
    left->SetSpriteReference(reference); right->SetSpriteReference(reference);
    C::SpriteAssetCaptureContext extraction;
    const auto before = left->CaptureSprite(extraction); ASSERT_TRUE(before);
    EXPECT_EQ(right->CaptureSprite(extraction),before); EXPECT_EQ(extraction.entries.size(),1);
    EXPECT_FLOAT_EQ(before->pixelsPerUnit,100);
    auto changedMeta = A::AssetMeta::Load(A::GetAssetMetaPath(source)); ASSERT_TRUE(changedMeta);
    changedMeta->settings["SPRITE_PIXELS_PER_UNIT"] = "50";
    ASSERT_TRUE(changedMeta->Save(A::GetAssetMetaPath(source)));
    ASSERT_TRUE(database.ReimportAsset("Assets/Hero.png"));
    manifest = database.GetArtifactManifestForAssetPath("Assets/Hero.png"); ASSERT_TRUE(manifest);
    runtime = makeRuntime();
    C::SpriteAssetCaptureContext updated;
    const auto after = left->CaptureSprite(updated); ASSERT_TRUE(after);
    EXPECT_FLOAT_EQ(after->pixelsPerUnit,50); EXPECT_FLOAT_EQ(before->pixelsPerUnit,100);
    EXPECT_EQ(right->CaptureSprite(updated),after); EXPECT_EQ(updated.entries.size(),1);
    auto missing = runtime.GetManifest();
    for (auto& entry : missing.entries) if (entry.artifactType == A::ArtifactType::Sprite)
    { entry.contentHash = "missing-revision"; entry.artifactPath = (root/"Missing.sprite").string(); }
    runtime = P::RuntimeAssetDatabase(std::move(missing));
    C::SpriteAssetCaptureContext unavailable;
    EXPECT_EQ(left->CaptureSprite(unavailable),after); EXPECT_EQ(right->CaptureSprite(unavailable),after);
    EXPECT_FALSE(left->LastError().empty()); EXPECT_FALSE(right->LastError().empty());
    right->SetSpriteReference({});
    EXPECT_FALSE(right->CaptureSprite(unavailable));
    EXPECT_EQ(left->CaptureSprite(unavailable),after);
}

TEST_F(SpriteImportFixture, MultipleSpritesShareEditorCookAndRetainKeysAcrossReorderRenameAndRemoval)
{
    E::AssetDatabaseFacade database(E::MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh()); ASSERT_TRUE(database.ImportAsset("Assets/Hero.png"));
    auto manifest = database.GetArtifactManifestForAssetPath("Assets/Hero.png"); ASSERT_TRUE(manifest);
    ASSERT_TRUE(Sprite(*manifest));
    const auto original = ReadSprite(database.ResolveArtifactPathAtPath("Assets/Hero.png",Sprite(*manifest)->subAssetKey));
    ASSERT_TRUE(original); ASSERT_GE(original->sourceWidth,2); ASSERT_GE(original->sourceHeight,2);
    NLS::Cook::CookOptions options; options.projectRoot = root; options.assetsRoot = root/"Assets";
    options.artifactRoot = root/"Library/Artifacts"; options.artifactDatabasePath = root/"Library/SheetCookDB";
    options.stagingRoot = root/"Library/SheetCookStaging"; options.target.artifactPlatform = "win64-dx12";
    ASSERT_TRUE(NLS::Cook::AssetCooker::CookAssets(options,{source}).Succeeded());
    A::SpriteGridSettings grid; grid.cellWidth = original->sourceWidth/2; grid.cellHeight = original->sourceHeight/2;
    std::string error;
    auto slices = A::GenerateGridSpriteSlices(original->sourceWidth,original->sourceHeight,grid,{},error);
    ASSERT_TRUE(slices) << error; ASSERT_EQ(slices->size(),4);
    meta.settings[A::SpriteImportModeSetting] = "multiple";
    meta.settings[A::SpriteSlicesSetting] = A::SerializeSpriteSlices(*slices,error);
    ASSERT_TRUE(meta.Save(A::GetAssetMetaPath(source)));
    const auto verify = [&](const std::string& assetPath, size_t expected)
    {
        ASSERT_TRUE(database.ReimportAsset(assetPath));
        const auto editor = database.GetArtifactManifestForAssetPath(assetPath); ASSERT_TRUE(editor);
        EXPECT_EQ(editor->primarySubAssetKey,"texture:main");
        const auto cooked = NLS::Cook::AssetCooker::CookAssets(options,{root/assetPath});
        ASSERT_TRUE(cooked.Succeeded()) << cooked.FormatSummary();
        A::ArtifactDatabase cookedDatabase; ASSERT_TRUE(cookedDatabase.Load(options.artifactDatabasePath));
        const auto target = cookedDatabase.BuildManifestForSource(meta.id,"win64-dx12"); ASSERT_TRUE(target);
        size_t count = 0;
        for (const auto& entry : editor->subAssets) if (entry.artifactType == A::ArtifactType::Sprite)
        {
            ++count;
            const auto logical = ReadSprite(database.ResolveArtifactPathAtPath(assetPath,entry.subAssetKey)); ASSERT_TRUE(logical);
            const auto match = std::find_if(target->subAssets.begin(),target->subAssets.end(),
                [&](const auto& value) {return value.subAssetKey == entry.subAssetKey;});
            ASSERT_NE(match,target->subAssets.end());
            auto path = std::filesystem::path(match->artifactPath); if(path.is_relative())path=root/path;
            const auto cookedLogical = ReadSprite(path); ASSERT_TRUE(cookedLogical);
            EXPECT_EQ(A::SerializeSpriteArtifact(*logical),A::SerializeSpriteArtifact(*cookedLogical));
            EXPECT_TRUE(std::any_of(slices->begin(),slices->end(),[&](const auto& slice){return slice.id == logical->id && slice.name == logical->name;}));
        }
        EXPECT_EQ(count,expected);
        EXPECT_EQ(std::count_if(target->subAssets.begin(),target->subAssets.end(),
            [](const auto& entry){return entry.artifactType == A::ArtifactType::Sprite;}),expected);
    };
    verify("Assets/Hero.png",4);
    const auto retained = slices->front().id;
    slices->front().name = "Renamed logical frame"; std::reverse(slices->begin(),slices->end());
    meta.settings[A::SpriteSlicesSetting] = A::SerializeSpriteSlices(*slices,error);
    ASSERT_TRUE(meta.Save(A::GetAssetMetaPath(source))); verify("Assets/Hero.png",4);
    ASSERT_TRUE(database.RenameAsset("Assets/Hero.png","SheetRenamed.png"));
    source = root/"Assets/SheetRenamed.png"; verify("Assets/SheetRenamed.png",4);
    // Emit asset-only runtime input for independent package acceptance. This
    // directory has no project descriptor and is never loaded as a new project.
    const auto example = root/"RuntimeExample";
    std::filesystem::create_directories(example/"Textures");
    std::filesystem::copy_file(source,example/"Textures/Sheet.png");
    ASSERT_TRUE(meta.Save(example/"Textures/Sheet.png.meta"));
    NLS::Engine::SceneSystem::Scene scene;
    auto& cameraObject = scene.CreateGameObject("Sheet Camera");
    cameraObject.GetTransform()->SetLocalPosition({0,0,2});
    cameraObject.GetTransform()->SetLocalRotation({0,1,0,0});
    auto* camera = cameraObject.AddComponent<NLS::Engine::Components::CameraComponent>();
    camera->SetProjectionMode(NLS::Render::Settings::EProjectionMode::ORTHOGRAPHIC);
    camera->SetSize(2); camera->SetClearColor({0,0,0});
    camera->SetClearMode(NLS::Engine::Components::CameraComponent::ECameraClearMode::SOLID_COLOR);
    for (size_t index = 0; index < slices->size(); ++index)
    {
        const auto& slice = (*slices)[index];
        auto& object = scene.CreateGameObject(slice.name);
        object.GetTransform()->SetLocalPosition({index%2 ? 0.35f : -0.35f,index/2 ? -0.35f : 0.35f,0});
        const float scale = 0.5f*slice.pixelsPerUnit/std::max(slice.width,slice.height);
        object.GetTransform()->SetLocalScale({scale,scale,1});
        const auto reference = NLS::Engine::Serialize::ObjectIdentifier::Asset(
            NLS::Engine::Serialize::AssetId(meta.id.GetGuid()),
            NLS::Engine::Serialize::MakeLocalIdentifierInFile(meta.id.GetGuid(),"sprite:"+slice.id));
        object.AddComponent<NLS::Engine::Components::SpriteRendererComponent>()->SetSpriteReference(
            NLS::Engine::Serialize::PPtr<NLS::Engine::Assets::SpriteAsset>(
                NLS::Engine::Serialize::PersistentManager::Instance().ObjectIdentifierToInstanceID(reference)));
    }
    const auto exampleScene = example/"Sheet.scene";
    { std::ofstream file(exampleScene); file << NLS::Engine::Serialize::ObjectGraphWriter::Write(
        NLS::Engine::Serialize::ObjectGraphSerializer::SerializeScene(scene)); ASSERT_TRUE(file.good()); }
    ASSERT_TRUE(A::AssetMeta::CreateForAsset(exampleScene).Save(A::GetAssetMetaPath(exampleScene)));
    std::cout << "[SpriteSheetFixture] " << example.generic_string() << '\n';
    const auto removed = slices->front().id; ASSERT_NE(removed,retained);
    slices->erase(slices->begin());
    meta.settings[A::SpriteSlicesSetting] = A::SerializeSpriteSlices(*slices,error);
    ASSERT_TRUE(meta.Save(A::GetAssetMetaPath(source))); verify("Assets/SheetRenamed.png",3);
    manifest = database.GetArtifactManifestForAssetPath("Assets/SheetRenamed.png"); ASSERT_TRUE(manifest);
    EXPECT_TRUE(std::any_of(manifest->subAssets.begin(),manifest->subAssets.end(),
        [&](const auto& entry){return entry.subAssetKey == "sprite:"+retained;}));
    EXPECT_FALSE(std::any_of(manifest->subAssets.begin(),manifest->subAssets.end(),
        [&](const auto& entry){return entry.subAssetKey == "sprite:"+removed;}));
}

TEST_F(SpriteImportFixture, RenameAndReimportKeepTheChildKeyAndBadSettingsPreserveThePublishedRevision)
{
    E::AssetDatabaseFacade database(E::MakeProjectEditorAssetRoots(root)); ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Hero.png"));
    const auto before=database.GetArtifactManifestForAssetPath("Assets/Hero.png"); ASSERT_TRUE(before); ASSERT_TRUE(Sprite(*before));
    const auto key=Sprite(*before)->subAssetKey;
    ASSERT_TRUE(database.RenameAsset("Assets/Hero.png","RenamedHero.png"));
    ASSERT_TRUE(database.ReimportAsset("Assets/RenamedHero.png"));
    const auto renamed=database.GetArtifactManifestForAssetPath("Assets/RenamedHero.png"); ASSERT_TRUE(renamed); ASSERT_TRUE(Sprite(*renamed));
    EXPECT_EQ(Sprite(*renamed)->subAssetKey,key); EXPECT_EQ(Sprite(*renamed)->sourceAssetId,meta.id);
    auto changed=A::AssetMeta::Load(root/"Assets/RenamedHero.png.meta"); ASSERT_TRUE(changed);
    changed->settings["SPRITE_PIXELS_PER_UNIT"]="nan"; ASSERT_TRUE(changed->Save(root/"Assets/RenamedHero.png.meta"));
    EXPECT_FALSE(database.ReimportAsset("Assets/RenamedHero.png"));
    const auto retained=database.GetArtifactManifestForAssetPath("Assets/RenamedHero.png"); ASSERT_TRUE(retained); ASSERT_TRUE(Sprite(*retained));
    EXPECT_EQ(Sprite(*retained)->contentHash,Sprite(*renamed)->contentHash);
    EXPECT_TRUE(std::filesystem::is_regular_file(database.ResolveArtifactPathAtPath("Assets/RenamedHero.png",key)));
}

TEST_F(SpriteImportFixture, PrefabRetainsSpriteIdentityAcrossOverridesReimportAndDependencyCollection)
{
    namespace S = NLS::Engine::Serialize;
    namespace P = NLS::Engine::Assets;
    using SpriteRenderer = NLS::Engine::Components::SpriteRendererComponent;
    E::AssetDatabaseFacade database(E::MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Hero.png"));
    const auto key = "sprite:" + meta.settings.at("SPRITE_ID");
    const auto identifier = S::ObjectIdentifier::Asset(S::AssetId(meta.id.GetGuid()),
        S::MakeLocalIdentifierInFile(meta.id.GetGuid(), key), key);
    const S::PPtr<P::SpriteAsset> reference(S::PersistentManager::Instance().ObjectIdentifierToInstanceID(identifier));
    const auto prefabId = A::AssetId::New();
    const auto sceneId = A::AssetId::New();
    NLS::Engine::SceneSystem::Scene scene;
    auto& hero = scene.CreateGameObject("PrefabHero");
    auto* sprite = hero.AddComponent<SpriteRenderer>();
    ASSERT_NE(sprite, nullptr);
    sprite->SetSpriteReference(reference);
    sprite->SetColor({0.25f, 0.5f, 0.75f, 1});
    sprite->SetFlipX(true);
    sprite->SetOrderInLayer(3);
    E::PrefabUtilityFacade facade;
    auto saved = facade.SaveAsPrefabAssetAndConnect(hero, prefabId, root / "Assets/Hero.prefab", scene, sceneId);
    ASSERT_EQ(saved.status, E::PrefabOperationStatus::Committed);
    ASSERT_TRUE(saved.artifact);
    ASSERT_TRUE(saved.instance);
    const auto hasSpriteReference = [&](const P::PrefabArtifact& prefab)
    {
        const auto references = P::CollectPrefabAssetReferences(prefab.graph);
        return std::any_of(references.begin(), references.end(), [&](const auto& value) { return value == identifier; });
    };
    EXPECT_TRUE(hasSpriteReference(*saved.artifact));
    auto reloaded = P::ImportPrefabArtifact(saved.prefabSourceText, prefabId);
    ASSERT_FALSE(reloaded.diagnostics.HasErrors());
    EXPECT_TRUE(hasSpriteReference(reloaded.artifact));

    NLS::Engine::SceneSystem::Scene instanceScene;
    auto instance = facade.InstantiatePrefab({&reloaded.artifact, prefabId, "prefab:PrefabHero", sceneId}, instanceScene);
    ASSERT_EQ(instance.status, E::PrefabOperationStatus::Committed);
    ASSERT_TRUE(instance.instance);
    auto* copy = instance.instance->instanceRoot->GetComponent<SpriteRenderer>();
    ASSERT_NE(copy, nullptr);
    EXPECT_EQ(copy->GetSpriteReference(), reference);
    EXPECT_TRUE(copy->GetFlipX());
    EXPECT_FLOAT_EQ(copy->GetColor().x, 0.25f);
    copy->SetFlipX(false);
    copy->SetOrderInLayer(-7);
    copy->SetSpriteReference({});
    const auto overrides = facade.GetPrefabOverrides(reloaded.artifact, *instance.instance, false);
    const auto flip = std::find_if(overrides.begin(), overrides.end(), [](const auto& value) { return value.propertyPath == "flipX"; });
    const auto order = std::find_if(overrides.begin(), overrides.end(), [](const auto& value) { return value.propertyPath == "orderInLayer"; });
    const auto spriteOverride = std::find_if(overrides.begin(), overrides.end(), [](const auto& value) { return value.propertyPath == "sprite"; });
    ASSERT_NE(flip, overrides.end());
    ASSERT_NE(order, overrides.end());
    ASSERT_NE(spriteOverride, overrides.end());
    // Inspector reconciliation records discovered changes before Apply/Revert.
    for (const auto& value : overrides) instance.instance->localPatches.push_back(value.patch);
    const auto reverted = facade.RevertSingleOverride(*instance.instance, *flip);
    for (const auto& diagnostic : reverted.diagnostics) ADD_FAILURE() << diagnostic.code << ": " << diagnostic.message;
    ASSERT_EQ(reverted.status, E::PrefabOperationStatus::Committed);
    copy = instance.instance->instanceRoot->GetComponent<SpriteRenderer>();
    ASSERT_NE(copy, nullptr);
    EXPECT_TRUE(copy->GetFlipX());
    EXPECT_EQ(copy->GetOrderInLayer(), -7);
    ASSERT_EQ(facade.RevertSingleOverride(*instance.instance, *spriteOverride).status, E::PrefabOperationStatus::Committed);
    EXPECT_EQ(copy->GetSpriteReference(), reference);
    ASSERT_EQ(facade.ApplySingleOverride(reloaded.artifact, *order).status, E::PrefabOperationStatus::Committed);
    NLS::Engine::SceneSystem::Scene appliedScene;
    const auto applied = facade.InstantiatePrefab({&reloaded.artifact, prefabId, "prefab:PrefabHero", sceneId}, appliedScene);
    ASSERT_EQ(applied.status, E::PrefabOperationStatus::Committed);
    ASSERT_TRUE(applied.instance);
    ASSERT_NE(applied.instance->instanceRoot->GetComponent<SpriteRenderer>(), nullptr);
    EXPECT_EQ(applied.instance->instanceRoot->GetComponent<SpriteRenderer>()->GetOrderInLayer(), -7);
    EXPECT_EQ(applied.instance->instanceRoot->GetComponent<SpriteRenderer>()->GetSpriteReference(), reference);

    ASSERT_TRUE(database.RenameAsset("Assets/Hero.png", "RenamedHero.png"));
    ASSERT_TRUE(database.ReimportAsset("Assets/RenamedHero.png"));
    const auto renamed = database.GetArtifactManifestForAssetPath("Assets/RenamedHero.png");
    ASSERT_TRUE(renamed);
    ASSERT_NE(Sprite(*renamed), nullptr);
    EXPECT_EQ(Sprite(*renamed)->subAssetKey, key);
    EXPECT_TRUE(hasSpriteReference(reloaded.artifact));

    { std::ofstream prefab(root / "Assets/Hero.prefab"); prefab << saved.prefabSourceText; }
    auto prefabMeta = A::AssetMeta::CreateForAsset(root / "Assets/Hero.prefab");
    prefabMeta.id = prefabId;
    ASSERT_TRUE(prefabMeta.Save(A::GetAssetMetaPath(root / "Assets/Hero.prefab")));
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Hero.prefab"));
    const auto prefabManifest = database.GetArtifactManifestForAssetPath("Assets/Hero.prefab");
    ASSERT_TRUE(prefabManifest);
    NLS::Engine::Assets::RuntimeManifestBuilder builder;
    builder.AddArtifactManifest(*renamed);
    builder.AddArtifactManifest(*prefabManifest);
    const auto closure = builder.Build({{prefabId, prefabManifest->primarySubAssetKey}}, "editor");
    for (const auto& diagnostic : closure.diagnostics.GetItems())
        if (diagnostic.IsError()) ADD_FAILURE() << diagnostic.code << " key=" << diagnostic.subAssetKey;
    SCOPED_TRACE("Prefab target=" + prefabManifest->targetPlatform + " Sprite target=" + renamed->targetPlatform);
    EXPECT_TRUE(std::any_of(closure.manifest.entries.begin(), closure.manifest.entries.end(),
        [&](const auto& entry) { return entry.assetId == meta.id && entry.subAssetKey == key; }));
    EXPECT_TRUE(std::any_of(closure.manifest.entries.begin(), closure.manifest.entries.end(),
        [&](const auto& entry) { return entry.assetId == meta.id && entry.subAssetKey == "texture:main"; }));

    NLS::Cook::CookOptions options;
    options.projectRoot = root;
    options.assetsRoot = root / "Assets";
    options.artifactDatabasePath = root / "Cooked/ArtifactDB";
    options.artifactRoot = root / "Library/Artifacts";
    options.stagingRoot = root / "Library/CookStaging";
    options.target.artifactPlatform = "win64-dx12";
    const auto cooked = NLS::Cook::AssetCooker::CookAssets(options, {root / "Assets/RenamedHero.png", root / "Assets/Hero.prefab"});
    ASSERT_TRUE(cooked.Succeeded()) << cooked.FormatSummary();
    A::ArtifactDatabase cookedDatabase;
    ASSERT_TRUE(cookedDatabase.Load(options.artifactDatabasePath));
    const auto cookedPrefab = cookedDatabase.BuildManifestForSource(prefabId, "win64-dx12");
    const auto cookedSprite = cookedDatabase.BuildManifestForSource(meta.id, "win64-dx12");
    ASSERT_TRUE(cookedPrefab);
    ASSERT_TRUE(cookedSprite);
    NLS::Engine::Assets::RuntimeManifestBuilder cookedBuilder;
    cookedBuilder.AddArtifactManifest(*cookedPrefab);
    cookedBuilder.AddArtifactManifest(*cookedSprite);
    const auto cookedClosure = cookedBuilder.Build({{prefabId, cookedPrefab->primarySubAssetKey}}, "win64-dx12");
    ASSERT_FALSE(cookedClosure.diagnostics.HasErrors());
    EXPECT_TRUE(std::any_of(cookedClosure.manifest.entries.begin(), cookedClosure.manifest.entries.end(),
        [&](const auto& entry) { return entry.assetId == meta.id && entry.subAssetKey == key; }));
    const auto missingKey = "sprite:" + NLS::Guid::New().ToString();
    auto missingSource = saved.prefabSourceText;
    const auto keyOffset = missingSource.find(key);
    ASSERT_NE(keyOffset, std::string::npos);
    missingSource.replace(keyOffset, key.size(), missingKey);
    { std::ofstream prefab(root / "Assets/Hero.prefab"); prefab << missingSource; }
    const auto missing = NLS::Cook::AssetCooker::CookAssets(options, {root / "Assets/Hero.prefab"});
    EXPECT_FALSE(missing.Succeeded());
    EXPECT_NE(missing.FormatSummary().find(missingKey), std::string::npos);
}

TEST_F(SpriteImportFixture, ExpandsStrippedSpritePrefabsForPlayerWithoutChangingAuthoringData)
{
    namespace S = NLS::Engine::Serialize;
    using SpriteRenderer = NLS::Engine::Components::SpriteRendererComponent;
    NLS::Engine::SceneSystem::Scene scene;
    auto& first = scene.CreateGameObject("PrefabHero");
    auto* sprite = first.AddComponent<SpriteRenderer>();
    auto& meshChild = scene.CreateGameObject("MeshChild");
    meshChild.SetParent(first);
    meshChild.AddComponent<NLS::Engine::Components::MeshFilter>();
    meshChild.AddComponent<NLS::Engine::Components::MeshRenderer>();
    const auto key = "sprite:" + meta.settings.at("SPRITE_ID");
    const auto identifier = S::ObjectIdentifier::Asset(S::AssetId(meta.id.GetGuid()),
        S::MakeLocalIdentifierInFile(meta.id.GetGuid(), key), key);
    const S::PPtr<NLS::Engine::Assets::SpriteAsset> reference(S::PersistentManager::Instance().ObjectIdentifierToInstanceID(identifier));
    sprite->SetSpriteReference(reference);
    const auto prefabId = A::AssetId::New(), sceneId = A::AssetId::New();
    E::PrefabUtilityFacade facade;
    auto saved = facade.SaveAsPrefabAssetAndConnect(first, prefabId, root / "Assets/Hero.prefab", scene, sceneId);
    ASSERT_EQ(saved.status, E::PrefabOperationStatus::Committed);
    ASSERT_TRUE(saved.artifact);
    ASSERT_TRUE(saved.instance);
    E::PrefabInstanceRegistry registry;
    registry.Register(std::move(*saved.instance));
    auto second = facade.InstantiatePrefab({&*saved.artifact, prefabId, "prefab:PrefabHero", sceneId}, scene);
    ASSERT_TRUE(second.instance);
    second.instance->instanceRoot->SetName("Second");
    second.instance->instanceRoot->GetTransform()->SetLocalPosition({4, 5, 0});
    ASSERT_EQ(second.instance->instanceRoot->GetChildren().size(), 1u);
    second.instance->instanceRoot->GetChildren().front()->SetTag("ChildOverride");
    second.instance->instanceRoot->GetChildren().front()->SetLayer(7);
    second.instance->instanceRoot->GetChildren().front()->SetActive(false);
    auto* secondSprite = second.instance->instanceRoot->GetComponent<SpriteRenderer>();
    ASSERT_NE(secondSprite, nullptr);
    secondSprite->SetFlipX(true);
    secondSprite->SetOrderInLayer(-2);
    secondSprite->SetSpriteReference({});
    registry.Register(std::move(*second.instance));
    auto document = S::ObjectGraphSerializer::SerializeScene(scene);
    facade.AnnotateSceneDocumentWithPrefabInstances(document, scene, registry);
    ASSERT_EQ(document.prefabInstances.size(), 2u);
    ASSERT_TRUE(std::any_of(document.objects.begin(), document.objects.end(),
        [](const auto& record) { return record.state == S::ObjectRecordState::Stripped; }));
    const auto authoringText = S::ObjectGraphWriter::Write(document);
    { std::ofstream output(root / "PrefabAuthoring.scene"); output << authoringText; }
    auto prefab = saved.artifact->graph;
    int64_t transformFile = 0;
    for (const auto& record : prefab.objects)
        if (record.typeName.ends_with("::TransformComponent")) transformFile = record.localIdentifierInFile;
    for (auto& record : prefab.objects)
    {
        if (record.debugName == "MeshChild")
            for (auto& property : record.properties)
                if (property.name == "tag") property.value = S::PropertyValue::String("SourceRevision");
        if (record.typeName.ends_with("::SpriteRendererComponent"))
        {
            for (auto& property : record.properties)
                if (property.name == "orderInLayer") property.value = S::PropertyValue::Integer(9);
            record.properties.push_back({"probeReference", S::PropertyValue::ObjectReference(S::ObjectIdentifier::LocalObject(transformFile))});
        }
    }
    auto expanded = document;
    std::string error;
    unsigned resolutions = 0;
    ASSERT_TRUE(NLS::Engine::Assets::ExpandPrefabInstances(expanded,
        [&](const auto& requested, std::string&) -> std::optional<S::ObjectGraphDocument>
        { ++resolutions; EXPECT_EQ(requested.guid, prefabId.GetGuid()); return prefab; }, error)) << error;
    EXPECT_EQ(resolutions, 1u);
    EXPECT_EQ(S::ObjectGraphWriter::Write(document), authoringText);
    EXPECT_TRUE(expanded.prefabInstances.empty());
    { std::ofstream output(root / "PrefabExpanded.scene"); output << S::ObjectGraphWriter::Write(expanded); }
    std::unordered_set<int64_t> localTargets;
    for (const auto& record : expanded.objects)
        for (const auto& property : record.properties)
            if (property.name == "probeReference") localTargets.insert(property.value.GetObjectReference().localIdentifierInFile);
    EXPECT_EQ(localTargets.size(), 2u);
    for (const auto file : localTargets)
        EXPECT_TRUE(std::any_of(expanded.objects.begin(), expanded.objects.end(),
            [&](const auto& record) { return record.localIdentifierInFile == file && record.typeName.ends_with("::TransformComponent"); }));
    S::LoadPolicy policy;
    policy.deferAssetReferenceResolution = true;
    policy.invalidReferencePolicy = S::InvalidReferencePolicy::Preserve;
    auto loaded = S::ObjectGraphInstantiator::InstantiateScene(expanded, policy);
    ASSERT_TRUE(loaded.scene);
    ASSERT_EQ(loaded.scene->GetFastAccessComponents().sprites.size(), 2u);
    unsigned meshChildren = 0;
    for (const auto* object : loaded.scene->GetGameObjects())
    {
        const auto* value = object->GetComponent<SpriteRenderer>();
        if (!value)
        {
            ++meshChildren;
            EXPECT_EQ(object->GetName(), "MeshChild");
            EXPECT_NE(object->GetComponent<NLS::Engine::Components::MeshFilter>(), nullptr);
            EXPECT_NE(object->GetComponent<NLS::Engine::Components::MeshRenderer>(), nullptr);
            ASSERT_NE(object->GetParent(), nullptr);
            EXPECT_EQ(object->GetTag(), object->GetParent()->GetName() == "Second" ? "ChildOverride" : "SourceRevision");
            EXPECT_EQ(object->GetLayer(), object->GetParent()->GetName() == "Second" ? 7 : 0);
            EXPECT_EQ(object->IsSelfActive(), object->GetParent()->GetName() != "Second");
            continue;
        }
        if (object->GetName() == "Second")
        {
            EXPECT_TRUE(value->GetFlipX());
            EXPECT_EQ(value->GetOrderInLayer(), -2);
            EXPECT_TRUE(value->GetSpriteReference().IsNull());
            EXPECT_FLOAT_EQ(object->GetTransform()->GetLocalPosition().x, 4);
            EXPECT_FLOAT_EQ(object->GetTransform()->GetLocalPosition().y, 5);
        }
        else
        {
            EXPECT_EQ(object->GetName(), "PrefabHero");
            EXPECT_EQ(value->GetOrderInLayer(), 9);
            EXPECT_EQ(value->GetSpriteReference(), reference);
        }
    }
    EXPECT_EQ(meshChildren, 2u);
    auto failed = document;
    EXPECT_FALSE(NLS::Engine::Assets::ExpandPrefabInstances(failed, {}, error));
    EXPECT_EQ(S::ObjectGraphWriter::Write(failed), authoringText);
    auto cyclicPrefab = prefab;
    cyclicPrefab.prefabInstances = document.prefabInstances;
    EXPECT_FALSE(NLS::Engine::Assets::ExpandPrefabInstances(failed,
        [&](const auto&, std::string&) -> std::optional<S::ObjectGraphDocument> { return cyclicPrefab; }, error));
    EXPECT_NE(error.find("cycle"), std::string::npos);
    EXPECT_EQ(S::ObjectGraphWriter::Write(failed), authoringText);
}

TEST(SpriteEditorInitialization, RegistersTheCompleteSpriteInspectorWithoutThrowing)
{
    EXPECT_NO_THROW(NLS::Editor::Properties::RegisterSpritePropertyDrawers());
    EXPECT_NO_THROW(NLS::Editor::Properties::RegisterSpritePropertyDrawers());
}

TEST(SpriteEditorInitialization, SortingLayerDrawerResolvesTheLiveSerializedProjection)
{
    using namespace NLS::Editor;
    NLS::Engine::AssemblyEngine::Initialize();
    Properties::RegisterSpritePropertyDrawers();
    NLS::Engine::GameObject object("Sprite inspector");
    auto* sprite = object.AddComponent<NLS::Engine::Components::SpriteRendererComponent>();
    ASSERT_NE(sprite, nullptr);
    Serialization::SerializedTargetSet targets;
    targets.targets.push_back(Inspector::InspectorTargetHandle::FromObject(*sprite, Inspector::InspectorTargetKind::NativeObject));
    targets.Normalize();
    const auto registration = Inspector::InspectorRegistry::Get().ResolveComponentEditor(sprite->GetType());
    ASSERT_TRUE(registration);
    auto editor = registration->create();
    ASSERT_NE(editor, nullptr);
    Inspector::PropertyNode component;
    component.target = targets.targets.front();
    component.targets = targets.targets;
    editor->UpdateSerializedObject(component);
    auto& serialized = editor->GetSerializedObject();
    ASSERT_TRUE(serialized.IsValid());
    const auto projection = Inspector::InspectorProjectionBuilder::BuildPropertyProjection(serialized, targets, {});
    const auto findLayer = [](auto&& self, const std::vector<Inspector::PropertyNode>& nodes) -> const Inspector::PropertyNode*
    {
        for (const auto& node : nodes)
        {
            if (node.fieldName == "sortingLayer") return &node;
            if (const auto* child = self(self, node.children)) return child;
        }
        return nullptr;
    };
    const auto* layer = findLayer(findLayer, projection);
    ASSERT_NE(layer, nullptr);
    const auto drawer = Inspector::InspectorRegistry::Get().ResolvePropertyDrawer(*layer);
    ASSERT_TRUE(drawer);
    EXPECT_EQ(drawer->identifier, "builtin.sprite.sorting-layer");
}

TEST_F(SpriteImportFixture, ScriptBuildUsesTheActiveSdkAheadOfAContainingCheckoutAndKeepsProjectOverrides)
{
    const auto installSdk = root / "ActiveSdk";
    const auto container = root / "ContainingCheckout";
    const auto project = container / "Game";
    const auto writeSdk = [](const std::filesystem::path& sdk)
    {
        const auto path = sdk / "Managed/Nullus.GameScripts/Nullus.GameScripts.csproj";
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path) << "<Project />";
    };
    writeSdk(installSdk);
    writeSdk(container);
    std::filesystem::create_directories(project);
    auto resolved = NLS::Editor::Scripting::ResolveManagedScriptProject(project, installSdk / "App/bin");
    ASSERT_TRUE(resolved);
    EXPECT_EQ(resolved->managedProjectRoot, installSdk);
    resolved = NLS::Editor::Scripting::ResolveManagedScriptProject(project, {});
    ASSERT_TRUE(resolved);
    EXPECT_EQ(resolved->managedProjectRoot, container);
    writeSdk(project);
    resolved = NLS::Editor::Scripting::ResolveManagedScriptProject(project, installSdk / "App/bin");
    ASSERT_TRUE(resolved);
    EXPECT_EQ(resolved->managedProjectRoot, project);
}

TEST_F(SpriteImportFixture, RepeatedScriptBuildReusesTheIdenticalRevisionWhileCoreClrHasItLoaded)
{
#if NLS_ENABLE_CSHARP_SCRIPTING
    using namespace NLS::Editor::Scripting;
    ManagedScriptBuildRequest request;
    request.projectRoot = root;
    request.outputRoot = root / "Library/ScriptBuild/Managed";
    request.dotnetPath = ManagedScriptBuildService::ResolveRepositoryDotnet(root);
    const auto first = ManagedScriptBuildService::Build(request);
    ASSERT_TRUE(first.succeeded) << first.output;
    std::ifstream manifest(std::filesystem::path(NLS_ROOT_DIR) / "Library/ScriptApi/ScriptApi.json", std::ios::binary);
    NLS::Scripting::ScriptApiDatabase api;
    ASSERT_TRUE(NLS::Scripting::ScriptApiManifest::FromJson(
        std::string(std::istreambuf_iterator<char>(manifest), {}), api).Succeeded());
    NLS::Scripting::CoreClrScriptBackend backend;
    struct Shutdown { NLS::Scripting::CoreClrScriptBackend& backend; ~Shutdown() { backend.Shutdown(); } } shutdown {backend};
    backend.SetDotnetRoot(request.dotnetPath.parent_path());
    backend.SetHostArtifacts(first.runtimeConfig, first.assembly);
    const auto initialized = backend.Initialize(api);
    ASSERT_TRUE(initialized.Succeeded()) << initialized.message;
    const auto repeated = ManagedScriptBuildService::Build(request);
    EXPECT_TRUE(repeated.succeeded) << repeated.output;
    EXPECT_EQ(repeated.assembly, first.assembly);
    EXPECT_EQ(repeated.runtimeConfig, first.runtimeConfig);
#else
    GTEST_SKIP() << "CoreCLR is disabled.";
#endif
}

TEST_F(SpriteImportFixture, SourceAndExplicitPathsResolveTheSameSpriteAndSheetsRequireAChild)
{
    using RuntimeDatabase = NLS::Engine::Assets::RuntimeAssetDatabase;
    struct RestoreService
    {
        RuntimeDatabase* previous = NLS::Core::ServiceLocator::Contains<RuntimeDatabase>()
            ? &NLS::Core::ServiceLocator::Get<RuntimeDatabase>() : nullptr;
        ~RestoreService()
        {
            if (previous) NLS::Core::ServiceLocator::Provide(*previous);
            else NLS::Core::ServiceLocator::Remove<RuntimeDatabase>();
        }
    } restore;
    const auto key = "sprite:" + meta.settings.at("SPRITE_ID");
    NLS::Engine::Assets::RuntimeAssetManifest manifest;
    manifest.entries.push_back({meta.id, key, A::ArtifactType::Sprite, "sprite"});
    RuntimeDatabase runtime(manifest);
    NLS::Core::ServiceLocator::Provide(runtime);
    for (const auto& path : {std::string("Assets/Hero.png"), std::string("Hero.png"), source.string(), "Assets/Hero.png#" + key})
    {
        const auto payload = E::ResolveSpriteDragPayload(path, (root / "Assets").string());
        ASSERT_TRUE(payload) << path;
        EXPECT_EQ(E::GetEditorAssetDragPayloadAssetId(*payload), meta.id);
        EXPECT_EQ(E::GetEditorAssetDragPayloadSubAssetKey(*payload), key);
        EXPECT_EQ(E::GetEditorAssetDragPayloadPath(*payload), "Assets/Hero.png");
    }
    EXPECT_FALSE(E::ResolveSpriteDragPayload("#" + key, (root / "Assets").string()));
    EXPECT_FALSE(E::ResolveSpriteDragPayload("../Hero.png", (root / "Assets").string()));
    manifest.entries.push_back({meta.id, "sprite:" + NLS::Guid::New().ToString(), A::ArtifactType::Sprite, "sprite"});
    runtime = RuntimeDatabase(manifest);
    EXPECT_FALSE(E::ResolveSpriteDragPayload("Assets/Hero.png", (root / "Assets").string()));
    EXPECT_TRUE(E::ResolveSpriteDragPayload("Assets/Hero.png#" + key, (root / "Assets").string()));
}
}
