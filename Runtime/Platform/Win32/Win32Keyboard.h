#pragma once
#ifdef _WIN32
#include "Keyboard.h"
#include "Win32Window.h"
#include "PlatformDef.h"
namespace NLS {
	namespace Win32Code {
		class NLS_PLATFORM_API Win32Keyboard : public Keyboard {
		public:
			friend class Win32Window;

		protected:
			Win32Keyboard(HWND &hwnd);
			virtual ~Win32Keyboard(void) {
			}

			virtual void UpdateRAW(RAWINPUT* raw);
			RAWINPUTDEVICE	rid;			//Windows OS hook 
		};
	}
}
#endif //_WIN32