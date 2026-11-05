#include "ApplicationPlugins.h"
#include <cstdio>

namespace NLS::Plugins
{
using namespace Core::Modules;
namespace
{
class PluginDirectorySource final : public Core::Assets::IContentSource
{
public:
    PluginDirectorySource(const std::filesystem::path& root,std::shared_ptr<const void> contentLease)
        : m_directory(root),m_contentLease(std::move(contentLease))
    {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
        {
            if (entry.is_symlink()) throw std::runtime_error("plugin content symbolic link is unsupported");
            if (!entry.is_regular_file()) continue;
            const auto utf8=entry.path().lexically_relative(root).generic_u8string();
            const std::string physical(reinterpret_cast<const char*>(utf8.data()),utf8.size());
            const auto logical=Core::Assets::NormalizePluginAssetPath(physical);
            if (!m_aliases.emplace(logical,physical).second) throw std::runtime_error("plugin content normalization conflict");
        }
    }
    const char* GetTypeName() const override { return "plugin-directory"; }
    bool Exists(std::string_view path) const override { return m_directory.Exists(Resolve(path)); }
    std::optional<uint64_t> GetSize(std::string_view path) const override { return m_directory.GetSize(Resolve(path)); }
    bool ReadAt(std::string_view path,uint64_t offset,void* output,size_t size,std::string* diagnostics) const override
    { return m_directory.ReadAt(Resolve(path),offset,output,size,diagnostics); }
private:
    std::string Resolve(std::string_view path) const
    {
        const auto normalized=Core::Assets::NormalizePluginAssetPath(path);
        const auto found=m_aliases.find(normalized); return found==m_aliases.end() ? normalized : found->second;
    }
    Core::Assets::DirectoryContentSource m_directory;
    std::shared_ptr<const void> m_contentLease;
    std::map<std::string,std::string> m_aliases;
};
}
ApplicationPlugins::ApplicationPlugins(HostKind kind, const std::filesystem::path& project,
    const std::filesystem::path& enginePlugins, bool player, std::function<void(PluginManager&)> registerSources)
    : m_host(kind), m_manager(m_host, kind), m_runtime(m_host.Extensions())
{
    if (Core::ServiceLocator::Contains<PluginHost>()) throw std::logic_error("a process plugin host is already bound");
    m_host.SetLog([](const std::string& text) { std::fprintf(stderr, "[Plugin] %s\n", text.c_str()); });
    if (registerSources) registerSources(m_manager);
    if (!(player ? m_manager.OpenPlayer(project) : m_manager.OpenProject(project, enginePlugins)))
        throw std::runtime_error(m_manager.LastError());
    for (const auto& root : m_manager.ContentRoots().roots)
    {
        auto source = std::make_shared<PluginDirectorySource>(root.physicalPath,root.contentLease);
        Core::Assets::ContentSourceTable::Get().Mount(root.logicalPath.generic_string(), source);
        m_mounts.emplace_back(root.logicalPath, std::move(source));
    }
    m_lifecycle.advance = [this](LoadingPhase phase) {
        if (phase >= LoadingPhase::PostEngine)
        {
            for (const auto& [root, source] : m_mounts) Core::Assets::ContentSourceTable::Get().Mount(root.generic_string(), source);
            m_host.SetResourceReader([this](const std::string& path, std::vector<uint8_t>& output) {
                auto normalized = Core::Assets::NormalizePluginAssetPath(path);
                if (normalized.rfind("Assets/Plugins/", 0) != 0) return false;
                const auto physical = m_manager.ProjectRoot() / std::filesystem::u8path(normalized);
                auto& table = Core::Assets::ContentSourceTable::Get();
                const auto size = table.GetSize(physical);
                return size && *size <= 64 * 1024 * 1024 && table.ReadAll(physical, output);
            });
        }
        return m_manager.Advance(phase);
    };
    m_lifecycle.close = [this] { m_runtime.End(); m_manager.Close(); };
    Core::ServiceLocator::Provide(m_host);
    Core::ServiceLocator::Provide(m_manager);
    Core::ServiceLocator::Provide(m_runtime);
    Core::ServiceLocator::Provide(m_lifecycle);
    Core::ServiceLocator::Provide(m_manager.ContentRoots());
}
ApplicationPlugins::~ApplicationPlugins()
{
    m_runtime.End(); m_manager.Close();
    m_host.SetResourceReader({});
    Core::ServiceLocator::Remove<Core::Assets::PluginContentRoots>();
    for (const auto& [root, source] : m_mounts) Core::Assets::ContentSourceTable::Get().Unmount(root, source.get());
    Core::ServiceLocator::Remove<HostLifecycle>();
    Core::ServiceLocator::Remove<RuntimeExtensionService>();
    Core::ServiceLocator::Remove<PluginManager>();
    Core::ServiceLocator::Remove<PluginHost>();
}
}
