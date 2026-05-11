#include "SaveFileDialog.h"

#include "Windowing/Dialogs/portable-file-dialogs.h"

namespace NLS::Dialogs
{
SaveFileDialog::SaveFileDialog(std::string const& title, std::string const& initial_path /*= ""*/, std::vector<std::string> filters /*= {"All Files", "*"}*/, bool confirm_overwrite)
    : sv(std::make_unique<pfd::save_file>(title, initial_path, filters, confirm_overwrite))
{
}

SaveFileDialog::~SaveFileDialog() = default;

std::string SaveFileDialog::Result()
{
    return sv->result();
}

bool SaveFileDialog::Ready(int timeout /*= 20*/) const
{
    return sv->ready(timeout);
}

bool SaveFileDialog::Kill() const
{
    return sv->kill();
}

} // namespace NLS::Dialogs
