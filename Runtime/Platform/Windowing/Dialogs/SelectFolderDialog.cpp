#include "Windowing/Dialogs/SelectFolderDialog.h"

#include "Windowing/Dialogs/portable-file-dialogs.h"

namespace NLS::Dialogs
{
SelectFolderDialog::SelectFolderDialog(std::string const& title, std::string const& default_path /* = "" */)
    : fd(std::make_unique<pfd::select_folder>(title, default_path))
{

}

SelectFolderDialog::~SelectFolderDialog() = default;

std::string SelectFolderDialog::Result()
{
    return fd->result();
}

bool SelectFolderDialog::Ready(int timeout /*= 20*/) const
{
    return fd->ready(timeout);
}

bool SelectFolderDialog::Kill() const
{
    return fd->kill();
}

} // namespace NLS::Dialogs
