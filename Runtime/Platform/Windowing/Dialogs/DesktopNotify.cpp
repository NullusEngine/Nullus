#include "Windowing/Dialogs/DesktopNotify.h"

#include "Windowing/Dialogs/portable-file-dialogs.h"

#undef MessageBox
#undef ERROR
#undef IGNORE

namespace NLS::Dialogs
{
DesktopNotify::DesktopNotify(std::string const& title, std::string const& message, MessageBox::EMessageType _icon /*= MessageBox::EMessageType::INFORMATION*/)
    : nt(std::make_unique<pfd::notify>(title, message, static_cast<pfd::icon>(_icon)))
{
}

DesktopNotify::~DesktopNotify() = default;
} // namespace NLS::Dialogs
