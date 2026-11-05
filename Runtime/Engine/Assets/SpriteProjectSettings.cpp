#include "Assets/SpriteProjectSettings.h"
#include <Core/Assets/ContentSource.h>
#include <Core/ResourceManagement/ShaderManager.h>
#include <chrono>
#include <filesystem>
#include <mutex>

namespace NLS::Engine::Assets
{
namespace
{
std::mutex mutex;
std::string cachedRoot,diagnostic;
std::shared_ptr<const Core::Assets::SpriteSortingLayers> cached;
std::chrono::steady_clock::time_point nextRefresh{};
}
std::shared_ptr<const Core::Assets::SpriteSortingLayers> CaptureSpriteSortingLayers(std::string* error)
{
    const auto root=Core::ResourceManagement::ShaderManager::ProjectAssetsRoot();
    std::lock_guard lock(mutex);const auto now=std::chrono::steady_clock::now();
    if(!cached||root!=cachedRoot||now>=nextRefresh)
    {
        if(root!=cachedRoot||!cached) cached=std::make_shared<Core::Assets::SpriteSortingLayers>();
        cachedRoot=root;nextRefresh=now+std::chrono::milliseconds(250);diagnostic.clear();
        const auto path=std::filesystem::path(root)/Core::Assets::SpriteSortingLayers::RelativePath;
        auto& sources=Core::Assets::ContentSourceTable::Get();
        if(sources.Exists(path))
        {
            std::vector<uint8_t> bytes;const auto size=sources.GetSize(path);
            Core::Assets::SpriteSortingLayers parsed;
            if(!size||*size>128*1024) diagnostic="Sorting layer settings are too large";
            else if(sources.ReadAll(path,bytes,&diagnostic)&&Core::Assets::SpriteSortingLayers::Parse(
                std::string_view(reinterpret_cast<const char*>(bytes.data()),bytes.size()),parsed,diagnostic))
                cached=std::make_shared<Core::Assets::SpriteSortingLayers>(std::move(parsed));
        }
        else cached=std::make_shared<Core::Assets::SpriteSortingLayers>();
    }
    if(error)*error=diagnostic;return cached;
}
void InvalidateSpriteSortingLayers(){std::lock_guard lock(mutex);nextRefresh={};}
}
