#include "Windowing/Dialogs/OpenFileDialog.h"
namespace NLS::Dialogs
{
OpenFileDialog::OpenFileDialog(std::string const& title, std::string const& default_path, std::vector<std::string> const& filters, bool allow_multiselecte)
    : op(title, default_path, filters, allow_multiselecte)
{
}

std::vector<std::string> OpenFileDialog::Result()
{
    return op.result();
}

bool OpenFileDialog::Ready(int timeout /*= 20*/) const
{
    return op.ready(timeout);
}

bool OpenFileDialog::Kill() const
{
    return op.kill();
}

} // namespace NLS::Dialogs
