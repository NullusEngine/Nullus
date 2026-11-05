#include "Packages/PackageManager.h"
#include "Platform/Process/InterprocessFileLock.h"
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv)
{
    if (argc < 3) return 2;
    try
    {
        if (std::string(argv[1]) == "lock")
        { NLS::Platform::Process::InterprocessFileLock lock(std::filesystem::u8path(argv[2]),true,std::chrono::milliseconds(50)); return 0; }
        if (std::string(argv[1]) == "install" && argc >= 4)
        {
            NLS::Packages::PackageManager manager(std::filesystem::u8path(argv[2]));
            if (argc >= 5)
            {
                const std::string point = argv[4];
                manager.SetTransactionObserver([point](const std::string& current) { if (point == current) std::_Exit(87); });
            }
            const auto result = manager.Install({NLS::Packages::PackageSourceKind::Directory,argv[3]});
            if (!result.succeeded) std::fprintf(stderr,"%s\n",result.error.c_str());
            return result.succeeded ? 0 : 1;
        }
        return 2;
    }
    catch (const std::exception& e) { std::fprintf(stderr,"%s\n",e.what()); return 6; }
}
