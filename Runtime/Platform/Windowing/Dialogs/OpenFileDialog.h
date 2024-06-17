#pragma once
#include "PlatformDef.h"
#include "portable-file-dialogs.h"
namespace NLS::Dialogs
{
/**
 * Dialog window that asks the user to select a file from the disk
 */
class NLS_PLATFORM_API OpenFileDialog
{
public:
    OpenFileDialog(std::string const& title,
                   std::string const& default_path = "",
                   std::vector<std::string> const& filters = {"All Files", "*"},
                   bool allow_multiselecte = false);
    std::vector<std::string> Result();
    bool Ready(int timeout = 20) const;
    bool Kill() const;

private:
    pfd::open_file op;
};
} // namespace NLS::Dialogs