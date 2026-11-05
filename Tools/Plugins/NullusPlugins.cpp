#include "Packages/PackageManager.h"
#include "Packages/RemotePackageSources.h"
#include "Plugins/RuntimeDeployment.h"
#include <Json/json.hpp>
#include <cstdio>
#include <string>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace
{
int Run(const std::vector<std::string>& args)
{
    using namespace NLS::Packages;
    if (args.size() < 3)
    {
        std::fprintf(stderr,"Usage: NullusPlugins <project> <install-dir|install-zip|list|search|resolve|enable|disable|uninstall|validate|repair|restore|approve-build|stage-player> [arguments]\n");
        return 2;
    }
    try
    {
        PackageManager manager(std::filesystem::u8path(args[1])); const auto& command = args[2];
        ConfigureRemotePackageSources(manager);
        PackageResult result;
        if ((command == "install-dir" || command == "install-zip" || command == "update-dir") && args.size() == 4)
            result = manager.Install({command == "install-zip" ? PackageSourceKind::Zip : PackageSourceKind::Directory,args[3]});
        else if (command == "install-git" && args.size() == 5)
        { PackageRequest request{PackageSourceKind::Git,args[3]}; request.revision = args[4]; result = manager.Install(request); }
        else if ((command == "install-registry" || command == "update-registry") && args.size() == 6)
            result = manager.Install({PackageSourceKind::Registry,args[3],args[4],args[5]});
        else if ((command == "enable" || command == "disable") && args.size() == 4) result = manager.SetEnabled(args[3],command == "enable");
        else if (command == "uninstall" && args.size() == 4) result = manager.Uninstall(args[3]);
        else if (command == "resolve") result = manager.Resolve();
        else if (command == "validate") result = manager.Validate();
        else if (command == "restore") result = manager.RestoreOffline();
        else if (command == "repair") result = manager.Repair();
        else if (command == "approve-build" && args.size() == 4) result = manager.ApproveSourceBuild(args[3]);
        else if (command == "stage-player" && args.size() == 4)
            result.succeeded = NLS::Plugins::StageRuntimePlugins(manager.Project(),std::filesystem::u8path(args[3]),NLS::Plugins::HostPlatform(),NLS::Plugins::HostArchitecture(),&result.error);
        else if (command == "list" || (command == "search" && args.size() == 4) || (command == "search-registry" && args.size() == 5))
        {
            auto plugins = command == "list" ? manager.List() : command == "search-registry" ? manager.SearchRegistry(args[3],args[4]) : manager.Search(args[3]); nlohmann::json output = nlohmann::json::array();
            for (const auto& p : plugins) output.push_back({{"id",p.id},{"version",p.version.String()},{"name",p.name},{"source",p.source},{"warnings",p.warnings}});
            std::puts(output.dump(2).c_str()); return 0;
        }
        else throw std::invalid_argument("unknown command or incorrect argument count");
        const nlohmann::json output{{"succeeded",result.succeeded},{"id",result.id},{"version",result.version},{"hash",result.contentHash},{"error",result.error}};
        std::puts(output.dump(2).c_str()); return result.succeeded ? 0 : 1;
    }
    catch (const std::exception& e) { std::fprintf(stderr,"[NullusPlugins] %s\n",e.what()); return 1; }
}
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
{
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i)
    {
        const auto count = WideCharToMultiByte(CP_UTF8,0,argv[i],-1,nullptr,0,nullptr,nullptr);
        std::string text(count,0); WideCharToMultiByte(CP_UTF8,0,argv[i],-1,text.data(),count,nullptr,nullptr);
        text.pop_back(); args.push_back(std::move(text));
    }
    return Run(args);
}
#else
int main(int argc, char** argv) { return Run(std::vector<std::string>(argv,argv + argc)); }
#endif
