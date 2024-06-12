#include "Windowing/Window.h"
#ifdef _WIN32
    #include "Windows.h"
#endif

#include <GLFW/glfw3.h>

#include <stdexcept>
std::unordered_map<GLFWwindow*, NLS::Windowing::Window*> NLS::Windowing::Window::__WINDOWS_MAP;

NLS::Windowing::Window::Window(Context::Device& p_device, const Settings::WindowSettings& p_windowSettings) :
	m_device(p_device),
	m_title(p_windowSettings.title),
	m_size{ static_cast<float>(p_windowSettings.width), static_cast<float>(p_windowSettings.height) },
	m_minimumSize { static_cast<float>(p_windowSettings.minimumWidth), static_cast<float>(p_windowSettings.minimumHeight) },
	m_maximumSize { static_cast<float>(p_windowSettings.maximumWidth), static_cast<float>(p_windowSettings.maximumHeight) },
	m_fullscreen(p_windowSettings.fullscreen),
	m_refreshRate(p_windowSettings.refreshRate),
	m_cursorMode(p_windowSettings.cursorMode),
	m_cursorShape(p_windowSettings.cursorShape)
{
	/* Window creation */
	CreateGlfwWindow(p_windowSettings);

	/* Window settings */
	SetCursorMode(p_windowSettings.cursorMode);
	SetCursorShape(p_windowSettings.cursorShape);

	/* Callback binding */
	BindKeyCallback();
	BindMouseCallback();
	BindScrollsCallback();
	BindIconifyCallback();
	BindCloseCallback();
	BindResizeCallback();
	BindCursorMoveCallback();
	BindFramebufferResizeCallback();
	BindMoveCallback();
	BindFocusCallback();

	/* Event listening */
	ResizeEvent.AddListener(std::bind(&Window::OnResize, this, std::placeholders::_1, std::placeholders::_2));
	MoveEvent.AddListener(std::bind(&Window::OnMove, this, std::placeholders::_1, std::placeholders::_2));
}

NLS::Windowing::Window::~Window()
{
	glfwDestroyWindow(m_glfwWindow);
}

//void NLS::Windowing::Window::SetIcon(const std::string & p_filePath)
//{
//	GLFWimage images[1];
//	images[0].pixels = stbi_load(p_filePath.c_str(), &images[0].width, &images[0].height, 0, 4);
//	glfwSetWindowIcon(m_glfwWindow, 1, images);
//}

void NLS::Windowing::Window::SetIconFromMemory(uint8_t* p_data, uint32_t p_width, uint32_t p_height)
{
	GLFWimage images[1];
	images[0].pixels = p_data;
	images[0].height = p_width;
	images[0].width = p_height;
	glfwSetWindowIcon(m_glfwWindow, 1, images);
}

NLS::Windowing::Window* NLS::Windowing::Window::FindInstance(GLFWwindow* p_glfwWindow)
{
	return __WINDOWS_MAP.find(p_glfwWindow) != __WINDOWS_MAP.end() ? __WINDOWS_MAP[p_glfwWindow] : nullptr;
}

NLS::Windowing::Window* NLS::Windowing::Window::GetWindow()
{
	return __WINDOWS_MAP.begin()->second;
}

void NLS::Windowing::Window::SetSize(uint16_t p_width, uint16_t p_height)
{
	glfwSetWindowSize(m_glfwWindow, static_cast<int>(p_width), static_cast<int>(p_height));
}

void NLS::Windowing::Window::SetMinimumSize(int16_t p_minimumWidth, int16_t p_minimumHeight)
{
	m_minimumSize.x = p_minimumWidth;
	m_minimumSize.y = p_minimumHeight;

	UpdateSizeLimit();
}

void NLS::Windowing::Window::SetMaximumSize(int16_t p_maximumWidth, int16_t p_maximumHeight)
{
	m_maximumSize.x = p_maximumWidth;
	m_maximumSize.y = p_maximumHeight;

	UpdateSizeLimit();
}

void NLS::Windowing::Window::SetPosition(int16_t p_x, int16_t p_y)
{
	glfwSetWindowPos(m_glfwWindow, static_cast<int>(p_x), static_cast<int>(p_y));
}

void NLS::Windowing::Window::Minimize() const
{
	glfwIconifyWindow(m_glfwWindow);
}

void NLS::Windowing::Window::Maximize() const
{
	glfwMaximizeWindow(m_glfwWindow);
}

void NLS::Windowing::Window::Restore() const
{
	glfwRestoreWindow(m_glfwWindow);
}

void NLS::Windowing::Window::Hide() const
{
	glfwHideWindow(m_glfwWindow);
}

void NLS::Windowing::Window::Show() const
{
	glfwShowWindow(m_glfwWindow);
}

void NLS::Windowing::Window::Focus() const
{
	glfwFocusWindow(m_glfwWindow);
}

void NLS::Windowing::Window::SetShouldClose(bool p_value) const
{
	glfwSetWindowShouldClose(m_glfwWindow, p_value);
}

bool NLS::Windowing::Window::ShouldClose() const
{
	return glfwWindowShouldClose(m_glfwWindow);
}

void NLS::Windowing::Window::SetFullscreen(bool p_value)
{
	if (p_value)
		m_fullscreen = true;

	glfwSetWindowMonitor
	(
		m_glfwWindow,
		p_value ? glfwGetPrimaryMonitor() : nullptr,
		static_cast<int>(m_position.x),
		static_cast<int>(m_position.y),
		static_cast<int>(m_size.x),
		static_cast<int>(m_size.y),
		m_refreshRate
	);

	if (!p_value)
		m_fullscreen = false;

}

void NLS::Windowing::Window::ToggleFullscreen()
{
	SetFullscreen(!m_fullscreen);
}

bool NLS::Windowing::Window::IsFullscreen() const
{
	return m_fullscreen;
}

bool NLS::Windowing::Window::IsHidden() const
{
	return glfwGetWindowAttrib(m_glfwWindow, GLFW_VISIBLE) == GLFW_FALSE;
}

bool NLS::Windowing::Window::IsVisible() const
{
	return glfwGetWindowAttrib(m_glfwWindow, GLFW_VISIBLE) == GLFW_TRUE;
}

bool NLS::Windowing::Window::IsMaximized() const
{
	return glfwGetWindowAttrib(m_glfwWindow, GLFW_MAXIMIZED) == GLFW_TRUE;
}

bool NLS::Windowing::Window::IsMinimized() const
{
	return glfwGetWindowAttrib(m_glfwWindow, GLFW_MAXIMIZED) == GLFW_FALSE;
}

bool NLS::Windowing::Window::IsFocused() const
{
	return glfwGetWindowAttrib(m_glfwWindow, GLFW_FOCUSED) == GLFW_TRUE;
}

bool NLS::Windowing::Window::IsResizable() const
{
	return glfwGetWindowAttrib(m_glfwWindow, GLFW_RESIZABLE) == GLFW_TRUE;
}

bool NLS::Windowing::Window::IsDecorated() const
{
	return glfwGetWindowAttrib(m_glfwWindow, GLFW_DECORATED) == GLFW_TRUE;;
}

void NLS::Windowing::Window::MakeCurrentContext() const
{
	glfwMakeContextCurrent(m_glfwWindow);
}

void NLS::Windowing::Window::SwapBuffers() const
{
	glfwSwapBuffers(m_glfwWindow);
}

void NLS::Windowing::Window::SetCursorMode(Cursor::ECursorMode p_cursorMode)
{
	m_cursorMode = p_cursorMode;
	glfwSetInputMode(m_glfwWindow, GLFW_CURSOR, static_cast<int>(p_cursorMode));
}

void NLS::Windowing::Window::SetCursorShape(Cursor::ECursorShape p_cursorShape)
{
	m_cursorShape = p_cursorShape;
	glfwSetCursor(m_glfwWindow, m_device.GetCursorInstance(p_cursorShape));
}

void NLS::Windowing::Window::SetCursorPosition(int16_t p_x, int16_t p_y)
{
	glfwSetCursorPos(m_glfwWindow, static_cast<double>(p_x), static_cast<double>(p_y));
}

void NLS::Windowing::Window::SetTitle(const std::string& p_title)
{
	m_title = p_title;
	glfwSetWindowTitle(m_glfwWindow, p_title.c_str());
}

void NLS::Windowing::Window::SetRefreshRate(int32_t p_refreshRate)
{
	m_refreshRate = p_refreshRate;
}

std::string NLS::Windowing::Window::GetTitle() const
{
	return m_title;
}

NLS::Maths::Vector2 NLS::Windowing::Window::GetSize() const
{
	int width, height;
	glfwGetWindowSize(m_glfwWindow, &width, &height);
	return NLS::Maths::Vector2(width, height);
}

NLS::Maths::Vector2 NLS::Windowing::Window::GetMinimumSize() const
{
	return m_minimumSize;
}

NLS::Maths::Vector2 NLS::Windowing::Window::GetMaximumSize() const
{
	return m_maximumSize;
}

NLS::Maths::Vector2 NLS::Windowing::Window::GetPosition() const
{
	int x, y;
	glfwGetWindowPos(m_glfwWindow, &x, &y);
	return NLS::Maths::Vector2(static_cast<int16_t>(x), static_cast<int16_t>(y));
}

NLS::Maths::Vector2 NLS::Windowing::Window::GetFramebufferSize() const
{
	int width, height;
	glfwGetFramebufferSize(m_glfwWindow, &width, &height);
	return NLS::Maths::Vector2(static_cast<uint16_t>(width), static_cast<uint16_t>(height));
}

NLS::Cursor::ECursorMode NLS::Windowing::Window::GetCursorMode() const
{
	return m_cursorMode;
}

NLS::Cursor::ECursorShape NLS::Windowing::Window::GetCursorShape() const
{
	return m_cursorShape;
}

int32_t NLS::Windowing::Window::GetRefreshRate() const
{
	return m_refreshRate;
}

GLFWwindow* NLS::Windowing::Window::GetGlfwWindow() const
{
	return m_glfwWindow;
}

void NLS::Windowing::Window::ShowConsole(bool state)
{
#ifdef _WIN32
    HWND consoleWindow = GetConsoleWindow();

    ShowWindow(consoleWindow, state ? SW_RESTORE : SW_HIDE);

	Focus();
#endif
}

void NLS::Windowing::Window::CreateGlfwWindow(const Settings::WindowSettings& p_windowSettings)
{
	GLFWmonitor* selectedMonitor = nullptr;

	if (m_fullscreen)
		selectedMonitor = glfwGetPrimaryMonitor();

	glfwWindowHint(GLFW_RESIZABLE,		p_windowSettings.resizable);
	glfwWindowHint(GLFW_DECORATED,		p_windowSettings.decorated);
	glfwWindowHint(GLFW_FOCUSED,		p_windowSettings.focused);
	glfwWindowHint(GLFW_MAXIMIZED,		p_windowSettings.maximized);
	glfwWindowHint(GLFW_FLOATING,		p_windowSettings.floating);
	glfwWindowHint(GLFW_VISIBLE,		p_windowSettings.visible);
	glfwWindowHint(GLFW_AUTO_ICONIFY,	p_windowSettings.autoIconify);
	glfwWindowHint(GLFW_REFRESH_RATE,	p_windowSettings.refreshRate);
	glfwWindowHint(GLFW_SAMPLES,		p_windowSettings.samples);

	m_glfwWindow = glfwCreateWindow(static_cast<int>(m_size.x), static_cast<int>(m_size.y), m_title.c_str(), selectedMonitor, nullptr);

	if (!m_glfwWindow)
	{
		throw std::runtime_error("Failed to create GLFW window"); // TODO: Replace with Debug assertion
	}
	else
	{
		UpdateSizeLimit();

		auto pos = GetPosition();
		m_position.x = pos.x;
		m_position.y = pos.y;

		__WINDOWS_MAP[m_glfwWindow] = this;
	}
}

void NLS::Windowing::Window::BindKeyCallback() const
{
	auto keyCallback = [](GLFWwindow* p_window, int p_key, int p_scancode, int p_action, int p_mods)
	{
		Window* windowInstance = FindInstance(p_window);

		if (windowInstance)
		{
			if (p_action == GLFW_PRESS)
				windowInstance->KeyPressedEvent.Invoke(p_key);

			if (p_action == GLFW_RELEASE)
				windowInstance->KeyReleasedEvent.Invoke(p_key);
		}
	};

	glfwSetKeyCallback(m_glfwWindow, keyCallback);
}

void NLS::Windowing::Window::BindMouseCallback() const
{
	auto mouseCallback = [](GLFWwindow* p_window, int p_button, int p_action, int p_mods)
	{
		Window* windowInstance = FindInstance(p_window);

		if (windowInstance)
		{
			if (p_action == GLFW_PRESS)
				windowInstance->MouseButtonPressedEvent.Invoke(p_button);

			if (p_action == GLFW_RELEASE)
				windowInstance->MouseButtonReleasedEvent.Invoke(p_button);
		}
	};

	glfwSetMouseButtonCallback(m_glfwWindow, mouseCallback);
}


void NLS::Windowing::Window::BindScrollsCallback() const
{
    auto scroll_callback = [](GLFWwindow* p_window, double xoffset, double yoffset)
    {
        Window* windowInstance = FindInstance(p_window);

        if (windowInstance)
        {
			windowInstance->MouseScrollEvent.Invoke(xoffset, yoffset);
        }
    };

	glfwSetScrollCallback(m_glfwWindow, scroll_callback);
}

void NLS::Windowing::Window::BindResizeCallback() const
{
	auto resizeCallback = [](GLFWwindow* p_window, int p_width, int p_height)
	{
		Window* windowInstance = FindInstance(p_window);

		if (windowInstance)
		{
			windowInstance->ResizeEvent.Invoke(static_cast<uint16_t>(p_width), static_cast<uint16_t>(p_height));
		}
	};

	glfwSetWindowSizeCallback(m_glfwWindow, resizeCallback);
}

void NLS::Windowing::Window::BindFramebufferResizeCallback() const
{
	auto framebufferResizeCallback = [](GLFWwindow* p_window, int p_width, int p_height)
	{
		Window* windowInstance = FindInstance(p_window);

		if (windowInstance)
		{
			windowInstance->FramebufferResizeEvent.Invoke(static_cast<uint16_t>(p_width), static_cast<uint16_t>(p_height));
		}
	};

	glfwSetFramebufferSizeCallback(m_glfwWindow, framebufferResizeCallback);
}

void NLS::Windowing::Window::BindCursorMoveCallback() const
{
	auto cursorMoveCallback = [](GLFWwindow* p_window, double p_x, double p_y)
	{
		Window* windowInstance = FindInstance(p_window);

		if (windowInstance)
		{
			windowInstance->CursorMoveEvent.Invoke(static_cast<int16_t>(p_x), static_cast<int16_t>(p_y));
		}
	};

	glfwSetCursorPosCallback(m_glfwWindow, cursorMoveCallback);
}

void NLS::Windowing::Window::BindMoveCallback() const
{
	auto moveCallback = [](GLFWwindow* p_window, int p_x, int p_y)
	{
		Window* windowInstance = FindInstance(p_window);

		if (windowInstance)
		{
			windowInstance->MoveEvent.Invoke(static_cast<int16_t>(p_x), static_cast<int16_t>(p_y));
		}
	};

	glfwSetWindowPosCallback(m_glfwWindow, moveCallback);
}

void NLS::Windowing::Window::BindIconifyCallback() const
{
	auto iconifyCallback = [](GLFWwindow* p_window, int p_iconified)
	{
		Window* windowInstance = FindInstance(p_window);

		if (windowInstance)
		{
			if (p_iconified == GLFW_TRUE)
				windowInstance->MinimizeEvent.Invoke();

			if (p_iconified == GLFW_FALSE)
				windowInstance->MaximizeEvent.Invoke();
		}
	};

	glfwSetWindowIconifyCallback(m_glfwWindow, iconifyCallback);
}

void NLS::Windowing::Window::BindFocusCallback() const
{
	auto focusCallback = [](GLFWwindow* p_window, int p_focused)
	{
		Window* windowInstance = FindInstance(p_window);

		if (windowInstance)
		{
			if (p_focused == GLFW_TRUE)
				windowInstance->GainFocusEvent.Invoke();

			if (p_focused == GLFW_FALSE)
				windowInstance->LostFocusEvent.Invoke();
		}
	};

	glfwSetWindowFocusCallback(m_glfwWindow, focusCallback);
}

void NLS::Windowing::Window::BindCloseCallback() const
{
	auto closeCallback = [](GLFWwindow* p_window)
	{
		Window* windowInstance = FindInstance(p_window);

		if (windowInstance)
		{
			windowInstance->CloseEvent.Invoke();
		}
	};

	glfwSetWindowCloseCallback(m_glfwWindow, closeCallback);
}

void NLS::Windowing::Window::OnResize(uint16_t p_width, uint16_t p_height)
{
	m_size.x = p_width;
	m_size.y = p_height;
}

void NLS::Windowing::Window::OnMove(int16_t p_x, int16_t p_y)
{
	if (!m_fullscreen)
	{
		m_position.x = p_x;
		m_position.y = p_y;
	}
}

void NLS::Windowing::Window::UpdateSizeLimit() const
{
	glfwSetWindowSizeLimits
	(
		m_glfwWindow,
		static_cast<int>(m_minimumSize.x),
		static_cast<int>(m_minimumSize.y),
		static_cast<int>(m_maximumSize.x),
		static_cast<int>(m_maximumSize.y)
	);
}
