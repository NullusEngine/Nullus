#include "Windowing/Dialogs/MessageBox.h"

NLS::Dialogs::MessageBox::MessageBox(std::string p_title, std::string p_message, EMessageType p_messageType, EButtonLayout p_buttonLayout)
    :msg(p_title, p_message, static_cast<pfd::choice>(p_buttonLayout), static_cast<pfd::icon>(p_messageType))
{
}

const NLS::Dialogs::MessageBox::EUserAction NLS::Dialogs::MessageBox::GetUserAction()
{
    return static_cast<NLS::Dialogs::MessageBox::EUserAction>(msg.result());
}

bool NLS::Dialogs::MessageBox::Kill() const
{
    return msg.kill();
}

bool NLS::Dialogs::MessageBox::Ready(int timeout /*= 20*/) const
{
    return msg.ready(timeout);
}
