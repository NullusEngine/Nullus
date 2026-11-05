#include "ProjectPluginState.h"
#include "Core/Assets/ArtifactManifest.h"
#include "Platform/Process/InterprocessFileLock.h"
#include "Guid.h"
#include <Json/json.hpp>
#include <fstream>
#include <set>

namespace NLS::Plugins
{
namespace
{
const std::set<std::string> files{"manifest.json","packages-lock.json","plugins.json","resolved-plugins.json","build-trust.json"};
std::string Read(const std::filesystem::path& path)
{
    std::ifstream input(path,std::ios::binary); if (!input) throw std::runtime_error("cannot read project plugin state: " + path.generic_string());
    return std::string((std::istreambuf_iterator<char>(input)),{});
}
}
bool IsProjectPluginStateFile(const std::string& name) { return files.count(name) != 0; }
void RecoverProjectPluginStateLocked(const std::filesystem::path& project)
{
    using Platform::Process::WriteDurableFile; using Platform::Process::ReplaceFileAtomically;
    const auto packages = project / "Packages", pointer = packages / ".transaction.json";
    if (!std::filesystem::exists(pointer)) return;
    const auto id = nlohmann::json::parse(Read(pointer)).at("id").get<std::string>();
    if (!Guid::TryParse(id)) throw std::runtime_error("invalid package recovery transaction id");
    const auto transaction = packages / ".transactions" / id;
    const auto journal = nlohmann::json::parse(Read(transaction / "state.json"));
    const auto phase = journal.at("phase").get<std::string>();
    if (phase != "committed" && phase != "prepared") throw std::runtime_error("invalid package recovery phase");
    const bool committed = phase == "committed";
    for (const auto& item : journal.at("files"))
    {
        const auto name = item.at("name").get<std::string>();
        if (!IsProjectPluginStateFile(name)) throw std::runtime_error("invalid recovery file name");
        if (!committed && !item.at("existed").get<bool>()) { std::filesystem::remove(packages / name); continue; }
        const auto data = Read(transaction / (committed ? "next" : "old") / name);
        const auto temporary = packages / (".recover-" + id + "-" + name);
        WriteDurableFile(temporary,data); ReplaceFileAtomically(temporary,packages / name);
    }
    std::filesystem::remove(pointer); std::filesystem::remove_all(transaction);
}
std::string ProjectPluginStateFingerprintLocked(const std::filesystem::path& project)
{
    Core::Assets::Sha256Hasher hash;
    for (const auto& name : files)
    {
        const auto path = project / "Packages" / name;
        const auto data = std::filesystem::is_regular_file(path) ? Read(path) : "<missing>";
        const auto input = name + ":" + std::to_string(data.size()) + ":" + data;
        if (!hash.Update(reinterpret_cast<const uint8_t*>(input.data()),input.size())) throw std::runtime_error("project plugin fingerprint failed");
    }
    return Core::Assets::ToHexString(hash.Finalize());
}
}
