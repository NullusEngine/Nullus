#pragma once

#include <memory>
#include <string>

#include "PlatformDef.h"
#ifdef APIENTRY
#undef APIENTRY
#endif

namespace pfd
{
class select_folder;
}

namespace NLS::Dialogs
{
class NLS_PLATFORM_API SelectFolderDialog
{
public:
    SelectFolderDialog(std::string const& title,
                       std::string const& default_path  = "" );
    ~SelectFolderDialog();

    std::string Result();
    bool Ready(int timeout = 20) const;
    bool Kill() const;

private:
    std::unique_ptr<pfd::select_folder> fd;
};
} // namespace NLS::Dialogs
