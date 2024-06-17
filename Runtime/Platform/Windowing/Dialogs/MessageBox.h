
#pragma once

#include <string>
#include "PlatformDef.h"
#include "portable-file-dialogs.h"
/* Prevent enum and class name to be replaced by standard macros */
#undef MessageBox
#undef ERROR
#undef IGNORE

namespace NLS::Dialogs
{
/**
 * Displays a modal dialog box that contains a system icon,
 * a set of buttons, and a brief application-specific message,
 * such as status or error information
 */
class NLS_PLATFORM_API MessageBox
{
public:
    /**
     * Defines some severity levels for MessageBox instances
     */
    enum class EMessageType
    {
        INFORMATION = 0,
        WARNING,
        ERROR,
        QUESTION,
    };

    /**
     * Defines some button layouts for MessageBox instances
     */
    enum class EButtonLayout
    {
        OK = 0,
        OK_CANCEL,
        YES_NO,
        YES_NO_CANCEL,
        RETRY_CANCEL,
        ABORT_RETRY_IGNORE
    };

    /**
     * Defines some actions that the MessageBox should provide
     */
    enum class EUserAction
    {
        CANCEL = -1,
        OK,
        YES,
        NO,
        ABORT,
        RETRY,
        IGNORE,
    };

    /**
     * Create the MessageBox
     * @param p_title
     * @param p_message
     * @param p_messageType
     * @param p_buttonLayout
     * @param p_autoSpawn
     */
    MessageBox(std::string p_title, std::string p_message, EMessageType p_messageType = EMessageType::INFORMATION, EButtonLayout p_buttonLayout = EButtonLayout::OK);

    bool Ready(int timeout = 20) const;
    bool Kill() const;
    /**
     * Return the user action
     */
    const EUserAction GetUserAction();

private:
    pfd::message msg;
};
} // namespace NLS::Dialogs