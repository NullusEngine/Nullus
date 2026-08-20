#include <filesystem>
#include <system_error>

#include <gtest/gtest.h>

#include "Debug/FileHandler.h"
#include "Rendering/Resources/Loaders/ShaderLoader.h"
#include "AssemblyEngine.h"
#include "Math/ExternalReflection.h"

int main(int argc, char** argv)
{
    const auto logsDirectory = std::filesystem::path(NLS_BUILD_DIR) / "Logs";
    std::error_code errorCode;
    std::filesystem::create_directories(logsDirectory, errorCode);
    NLS::Debug::FileHandler::SetLogFilePath(logsDirectory.generic_string());
    NLS::Render::Resources::Loaders::ShaderLoader::SetTrustedBuiltInShaderEngineAssetsPath(
        (std::filesystem::path(NLS_ROOT_DIR) / "App" / "Assets" / "Engine").string());
    // Keep generated and external Engine/Math reflection modules reachable in
    // the focused static-library test target before object graph loading.
    NLS::Engine::AssemblyEngine::Initialize();

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
