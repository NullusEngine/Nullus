#pragma once

#include <memory>

#include "MessageBox.h"

namespace pfd
{
class notify;
}

namespace NLS::Dialogs
{
class NLS_PLATFORM_API DesktopNotify
{
public:
    DesktopNotify(std::string const& title,
                  std::string const& message,
                  MessageBox::EMessageType _icon = MessageBox::EMessageType::INFORMATION);
    ~DesktopNotify();

private:
    std::unique_ptr<pfd::notify> nt;
};
} // namespace NLS::Dialogs
