#include "SaveFileDialog.h"

namespace NLS::Dialogs
{
SaveFileDialog::SaveFileDialog(std::string const& title, std::string const& initial_path /*= ""*/, std::vector<std::string> filters /*= {"All Files", "*"}*/, bool confirm_overwrite)
    : sv(title, initial_path, filters, confirm_overwrite)
{
}

std::string SaveFileDialog::Result()
{
    return sv.result();
}

bool SaveFileDialog::Ready(int timeout /*= 20*/) const
{
    return sv.ready(timeout);
}

bool SaveFileDialog::Kill() const
{
    return sv.kill();
}

} // namespace NLS::Dialogs