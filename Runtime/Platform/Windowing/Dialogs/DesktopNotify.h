#pragma once
#include "MessageBox.h"
namespace NLS::Dialogs
{
class NLS_PLATFORM_API DesktopNotify
{
public:
    DesktopNotify(std::string const& title,
                  std::string const& message,
                  MessageBox::EMessageType _icon = MessageBox::EMessageType::INFORMATION);

private:
    pfd::notify nt;
};
} // namespace NLS::Dialogs