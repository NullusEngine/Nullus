#include "Windowing/Dialogs/DesktopNotify.h"
namespace NLS::Dialogs
{
DesktopNotify::DesktopNotify(std::string const& title, std::string const& message, MessageBox::EMessageType _icon /*= MessageBox::EMessageType::INFORMATION*/)
    : nt(title, message, static_cast<pfd::icon>(_icon))
{
}
} // namespace NLS::Dialogs