#include "Windowing/Dialogs/SelectFolderDialog.h"
namespace NLS::Dialogs
{
SelectFolderDialog::SelectFolderDialog(std::string const& title, std::string const& default_path /* = "" */)
    : fd(title, default_path)
{

}

std::string SelectFolderDialog::Result()
{
    return fd.result();
}

bool SelectFolderDialog::Ready(int timeout /*= 20*/) const
{
    return fd.ready(timeout);
}

bool SelectFolderDialog::Kill() const
{
    return fd.kill();
}

} // namespace NLS::Dialogs