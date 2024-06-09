
#include "Windowing/Context/Device.h"

#include <stdexcept>

#include <GLFW/glfw3.h>

NLS::Event<NLS::EDeviceError, std::string> NLS::Context::Device::ErrorEvent;

NLS::Context::Device::Device(const NLS::Windowing::Settings::DeviceSettings& p_deviceSettings)
{
	BindErrorCallback();

	int initializationCode = glfwInit();

	if (initializationCode == GLFW_FALSE)
	{
		throw std::runtime_error("Failed to Init GLFW"); // TODO: Replace with Debug assertion
		glfwTerminate();
	}
	else
	{
		CreateCursors();

		if (p_deviceSettings.debugProfile)
			glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GL_TRUE);

		glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, p_deviceSettings.contextMajorVersion);
		glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, p_deviceSettings.contextMinorVersion);

		glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
		glfwWindowHint(GLFW_SAMPLES, p_deviceSettings.samples);

		m_isAlive = true;
	}
}

NLS::Context::Device::~Device()
{
	if (m_isAlive)
	{
		DestroyCursors();
		glfwTerminate();
	}
}

NLS::Maths::Vector2 NLS::Context::Device::GetMonitorSize() const
{
	const GLFWvidmode * mode = glfwGetVideoMode(glfwGetPrimaryMonitor());

	return NLS::Maths::Vector2(mode->width, mode->height);
}

GLFWcursor * NLS::Context::Device::GetCursorInstance(Cursor::ECursorShape p_cursorShape) const
{
	return m_cursors.at(p_cursorShape);
}

bool NLS::Context::Device::HasVsync() const
{
	return m_vsync;
}

void NLS::Context::Device::SetVsync(bool p_value)
{
	glfwSwapInterval(p_value ? 1 : 0);
	m_vsync = p_value;
}

void NLS::Context::Device::PollEvents() const
{
	glfwPollEvents();
}

float NLS::Context::Device::GetElapsedTime() const
{
	return static_cast<float>(glfwGetTime());
}

void NLS::Context::Device::BindErrorCallback()
{
	auto errorCallback = [](int p_code, const char* p_description)
	{
		ErrorEvent.Invoke(static_cast<EDeviceError>(p_code), p_description);
	};

	glfwSetErrorCallback(errorCallback);
}

void NLS::Context::Device::CreateCursors()
{
	m_cursors[Cursor::ECursorShape::ARROW] = glfwCreateStandardCursor(static_cast<int>(Cursor::ECursorShape::ARROW));
	m_cursors[Cursor::ECursorShape::IBEAM] = glfwCreateStandardCursor(static_cast<int>(Cursor::ECursorShape::IBEAM));
	m_cursors[Cursor::ECursorShape::CROSSHAIR] = glfwCreateStandardCursor(static_cast<int>(Cursor::ECursorShape::CROSSHAIR));
	m_cursors[Cursor::ECursorShape::HAND] = glfwCreateStandardCursor(static_cast<int>(Cursor::ECursorShape::HAND));
	m_cursors[Cursor::ECursorShape::HRESIZE] = glfwCreateStandardCursor(static_cast<int>(Cursor::ECursorShape::HRESIZE));
	m_cursors[Cursor::ECursorShape::VRESIZE] = glfwCreateStandardCursor(static_cast<int>(Cursor::ECursorShape::VRESIZE));
}

void NLS::Context::Device::DestroyCursors()
{
	glfwDestroyCursor(m_cursors[Cursor::ECursorShape::ARROW]);
	glfwDestroyCursor(m_cursors[Cursor::ECursorShape::IBEAM]);
	glfwDestroyCursor(m_cursors[Cursor::ECursorShape::CROSSHAIR]);
	glfwDestroyCursor(m_cursors[Cursor::ECursorShape::HAND]);
	glfwDestroyCursor(m_cursors[Cursor::ECursorShape::HRESIZE]);
	glfwDestroyCursor(m_cursors[Cursor::ECursorShape::VRESIZE]);
}
