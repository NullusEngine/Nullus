#pragma once
#ifdef _WIN32
    #include "Mouse.h"
    #include "Win32Window.h"
    #include "PlatformDef.h"
namespace NLS
{
namespace Win32Code
{
class NLS_PLATFORM_API Win32Mouse : public NLS::Mouse
{
public:
    friend class Win32Window;

protected:
    Win32Mouse(HWND& hwnd);
    virtual ~Win32Mouse(void) {}

    void UpdateWindowPosition(const Vector2& newPos)
    {
        windowPosition = newPos;
    }

    virtual void UpdateRAW(RAWINPUT* raw);
    RAWINPUTDEVICE rid; // Windows OS hook

    bool setAbsolute;
};
} // namespace Win32Code
} // namespace NLS
#endif //_WIN32