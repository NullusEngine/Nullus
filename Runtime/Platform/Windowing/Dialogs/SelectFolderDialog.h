#pragma once

#include <string>
#include "PlatformDef.h"
#include "portable-file-dialogs.h"
namespace NLS::Dialogs
{
class NLS_PLATFORM_API SelectFolderDialog
{
public:
    SelectFolderDialog(std::string const& title,
                       std::string const& default_path  = "" );

    std::string Result();
    bool Ready(int timeout = 20) const;
    bool Kill() const;

private:
    pfd::select_folder fd;
};
} // namespace NLS::Dialogs