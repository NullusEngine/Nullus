#include "VfxLibraryDocument.h"
#include <Json/json.hpp>
#include <fstream>

namespace NLS::Editor::VFXTools
{
namespace author = NLS::VFX::Authoring;
LibraryAsset CreateLibraryAsset(author::AssetKind kind,std::string name)
{
    LibraryAsset result; result.kind = kind;
    if (kind == author::AssetKind::Emitter) result.emitter = author::CreateEmitterSource(std::move(name));
    else result.script = author::CreateScript(kind,std::move(name));
    return result;
}
std::string SerializeLibraryAsset(const LibraryAsset& asset)
{ return asset.kind == author::AssetKind::Emitter ? author::SerializeEmitterSource(asset.emitter) : author::SerializeScript(asset.script); }
bool DeserializeLibraryAsset(std::string_view source,LibraryAsset& asset,std::string& error)
{
    try
    {
        LibraryAsset candidate;
        if (nlohmann::json::parse(source).at("kind") == "Emitter")
        { candidate.kind = author::AssetKind::Emitter; if (!author::DeserializeEmitterSource(source,candidate.emitter,error)) return false; }
        else { if (!author::DeserializeScript(source,candidate.script,error)) return false; candidate.kind = candidate.script.kind; }
        asset = std::move(candidate); return true;
    }
    catch (const std::exception& exception) { error = exception.what(); return false; }
}
bool LoadLibraryAsset(const std::filesystem::path& path,LibraryAsset& asset,std::string& error)
{
    std::ifstream file(path,std::ios::binary|std::ios::ate); const auto size = file ? file.tellg() : std::streampos(-1);
    if (size < 0 || size > 64u*1024u*1024u) { error = "Cannot read VFX library asset or file exceeds 64 MiB"; return false; }
    std::string source(static_cast<size_t>(size),'\0'); file.seekg(0); file.read(source.data(),static_cast<std::streamsize>(source.size()));
    if (!file) { error = "Cannot read complete VFX library asset"; return false; }
    return DeserializeLibraryAsset(source,asset,error);
}
bool SaveLibraryAsset(const std::filesystem::path& path,const LibraryAsset& asset,std::string& error)
{ return asset.kind == author::AssetKind::Emitter ? author::SaveEmitterSource(path,asset.emitter,error) : author::SaveScript(path,asset.script,error); }
}
