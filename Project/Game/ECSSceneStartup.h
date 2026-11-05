#pragma once
#include <Scripting/RuntimePlaySession.h>
#include <Engine/Assets/RuntimeAssetDatabase.h>
#include <filesystem>
#include <memory>
#include <string_view>

namespace NLS::Game::RuntimeAssets
{
bool IsECSStartScene(std::string_view path);

// Packaged players require the GUID written by the builder. A development
// host may explicitly supply the source path to resolve an existing .meta.
Engine::Assets::RuntimeAssetRef ResolveECSStartSceneAsset(
    std::string_view assetId, std::string_view subAssetKey,
    const std::filesystem::path& developmentSource = {});

// Configures a fresh Play-session World. Built-in Transform codecs/systems and
// assets precede game registration. The initial scene finishes before Begin
// succeeds; ordinary frame updates retain the asynchronous streaming path.
void ConfigureECSStartScene(Scripting::RuntimePlaySession& session,
    std::shared_ptr<const Engine::Assets::RuntimeAssetDatabase> database,
    std::filesystem::path contentRoot, Engine::Assets::RuntimeAssetRef scene,
    Scripting::RuntimePlaySession::ECSSetup registerGame = {});
}
