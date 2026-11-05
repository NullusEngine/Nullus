#include <gtest/gtest.h>
#include <Assets/AssetDatabaseFacade.h>
#include <Assets/NativeArtifactContainer.h>
#include <Core/ServiceLocator.h>
#include <Engine/Assets/AudioClip.h>
#include <Engine/Assets/RuntimeAssetDatabase.h>
#include <Engine/Components/AudioSourceComponent.h>
#include <Scripting/RuntimePlaySession.h>
#include <filesystem>
#include <fstream>

TEST(PlayAssetIntegrationTests, AudioImportsCooksAndRetainsItsReferenceAcrossSandboxSessions)
{
    namespace A=NLS::Core::Assets;namespace E=NLS::Engine;namespace S=E::Serialize;
    const auto root=std::filesystem::temp_directory_path()/("NullusPlayAudio_"+NLS::Guid::New().ToString());
    std::filesystem::create_directories(root/"Assets");
    struct Cleanup {std::filesystem::path root;~Cleanup(){std::error_code error;std::filesystem::remove_all(root,error);}} cleanup{root};
    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    for(const auto* extension:{"wav","mp3","ogg"})
    {
        const auto name=std::string("tone.")+extension;
        std::filesystem::copy_file(std::filesystem::path(NLS_ROOT_DIR)/"Tests/Fixtures/PlayAudio"/name,root/"Assets"/name);
    }
    ASSERT_TRUE(database.Refresh());
    for(const auto* extension:{"wav","mp3","ogg"})
    {
        const auto source="Assets/tone."+std::string(extension);
        const bool imported=database.ImportAsset(source);
        std::string diagnostics;
        for(const auto& diagnostic:database.GetDiagnostics())
            diagnostics += diagnostic.code + ": " + diagnostic.message + "\n";
        ASSERT_TRUE(imported)<<source<<"\n"<<diagnostics;
        const auto records=database.LoadAllAssetsAtPath(source);ASSERT_EQ(records.size(),1u);
        EXPECT_EQ(records.front().artifactType,A::ArtifactType::Audio);
        const auto artifactPath=database.ResolveArtifactPathAtPath(source,records.front().subAssetKey);
        std::ifstream file(artifactPath,std::ios::binary);const std::vector<uint8_t> bytes(std::istreambuf_iterator<char>(file),{});
        const auto container=A::ReadNativeArtifactContainer(bytes,A::ArtifactType::Audio,1);ASSERT_TRUE(container);
        std::string error;ASSERT_TRUE(E::Audio::DecodeAudio(container->payload,error))<<error;
        auto manifest=database.GetArtifactManifestForAssetPath(source);ASSERT_TRUE(manifest);
        E::Assets::RuntimeManifestBuilder builder;builder.AddArtifactManifest(*manifest);
        const auto cooked=builder.Build({{records.front().assetId,records.front().subAssetKey}},"win64-dx12");
        ASSERT_FALSE(cooked.manifest.entries.empty());
        auto runtimeManifest=cooked.manifest;
        runtimeManifest.entries.front().artifactPath=artifactPath.string();
        E::Assets::RuntimeAssetDatabase runtimeDatabase(std::move(runtimeManifest));
        auto* previous=NLS::Core::ServiceLocator::Contains<E::Assets::RuntimeAssetDatabase>()?&NLS::Core::ServiceLocator::Get<E::Assets::RuntimeAssetDatabase>():nullptr;
        NLS::Core::ServiceLocator::Provide(runtimeDatabase);
        struct Restore {E::Assets::RuntimeAssetDatabase* previous;~Restore(){if(previous)NLS::Core::ServiceLocator::Provide(*previous);else NLS::Core::ServiceLocator::Remove<E::Assets::RuntimeAssetDatabase>();}} restore{previous};
        const auto id=S::ObjectIdentifier::Asset(S::AssetId(records.front().assetId.GetGuid()),S::MakeLocalIdentifierInFile(records.front().assetId.GetGuid(),records.front().subAssetKey),source);
        auto clip=E::Assets::LoadAudioClip(id,error);ASSERT_TRUE(clip)<<error;ASSERT_TRUE(clip->GetPcm());
        E::SceneSystem::SceneManager scenes("");scenes.LoadEmptyScene();
        auto* authored=scenes.GetCurrentScene()->CreateGameObject("Audio").AddComponent<E::Components::AudioSourceComponent>();
        authored->SetClip(S::PPtr<E::Assets::AudioClip>(clip.get()));authored->SetLoop(true);
        auto device=E::Audio::AudioDevice::CreateOffline();NLS::Scripting::ScriptRuntime scripts;ASSERT_TRUE(scripts.Initialize({}).Succeeded());
        for(uint64_t sessionId=400;sessionId<402;++sessionId)
        {
            ASSERT_TRUE(scenes.BeginPlaySandbox());NLS::Scripting::RuntimePlaySession session;
            auto* runtimeSource=scenes.GetCurrentScene()->GetGameObjects().front()->GetComponent<E::Components::AudioSourceComponent>();
            ASSERT_NE(runtimeSource,nullptr);
            EXPECT_NE(runtimeSource->GetClip().GetInstanceID(),NLS::InstanceID_None);
            ASSERT_TRUE(session.Begin(scripts,scenes,sessionId,device).Succeeded());scenes.GetCurrentScene()->Play();session.Tick(.02f);
            EXPECT_EQ(device->GetVoiceCount(sessionId),1u)<<runtimeSource->LastError();
            session.End();EXPECT_EQ(device->GetVoiceCount(sessionId),0u);ASSERT_TRUE(scenes.DiscardPlaySandbox());
            EXPECT_EQ(authored->GetClip().Get(),clip.get());EXPECT_EQ(scenes.GetCurrentScene()->GetGameObjects().size(),1u);
        }
    }
}
