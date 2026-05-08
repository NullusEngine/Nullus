
#include "Windowing/Context/Device.h"

#include <stdexcept>
#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <cmath>
#include <unordered_set>
#include <vector>

#include <GLFW/glfw3.h>
#include <Image.h>
#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #ifdef APIENTRY
        #undef APIENTRY
    #endif
    #include <Windows.h>
    #ifdef min
        #undef min
    #endif
    #ifdef max
        #undef max
    #endif
#endif

NLS::Event<NLS::EDeviceError, std::string> NLS::Context::Device::ErrorEvent;

namespace
{
    struct CursorSize
    {
        int width = 32;
        int height = 32;
    };

    CursorSize ResolveSystemCursorSize()
    {
#ifdef _WIN32
        const int width = GetSystemMetrics(SM_CXCURSOR);
        const int height = GetSystemMetrics(SM_CYCURSOR);
        if (width > 0 && height > 0)
            return { width, height };
#endif
        return {};
    }

	void EnsureWslgEnvironment()
	{
#if defined(__linux__)
		const bool hasWslg = std::filesystem::exists("/mnt/wslg") && std::filesystem::exists("/mnt/wslg/runtime-dir");
		if (!hasWslg)
			return;

		if (!std::getenv("DISPLAY"))
			setenv("DISPLAY", ":0", 0);

		if (!std::getenv("WAYLAND_DISPLAY"))
			setenv("WAYLAND_DISPLAY", "wayland-0", 0);

		if (!std::getenv("XDG_RUNTIME_DIR"))
			setenv("XDG_RUNTIME_DIR", "/mnt/wslg/runtime-dir", 0);

		if (!std::getenv("PULSE_SERVER"))
			setenv("PULSE_SERVER", "unix:/mnt/wslg/PulseServer", 0);
#endif
	}

    std::filesystem::path ResolveEditorCursorAssetPath(const std::string& filename)
    {
        const std::filesystem::path relativePath =
            std::filesystem::path("App") / "Assets" / "Editor" / "Icon" / "cursors" / "windows" / filename;

        auto current = std::filesystem::current_path();
        for (int i = 0; i < 6; ++i)
        {
            const auto candidate = current / relativePath;
            if (std::filesystem::exists(candidate))
                return candidate;

            if (!current.has_parent_path() || current == current.parent_path())
                break;

            current = current.parent_path();
        }

        return relativePath;
    }

    GLFWcursor* CreateImageCursor(const std::string& filename)
    {
        const auto path = ResolveEditorCursorAssetPath(filename);
        NLS::Image image(path.string(), false);
        if (image.GetData() == nullptr || image.GetWidth() <= 0 || image.GetHeight() <= 0)
            return nullptr;

        std::vector<unsigned char> rgbaPixels(static_cast<size_t>(image.GetWidth()) * static_cast<size_t>(image.GetHeight()) * 4u);
        const unsigned char* source = image.GetData();
        const int channels = image.GetChannels();

        for (int i = 0; i < image.GetWidth() * image.GetHeight(); ++i)
        {
            const int src = i * channels;
            const int dst = i * 4;

            switch (channels)
            {
            case 4:
                rgbaPixels[dst + 0] = source[src + 0];
                rgbaPixels[dst + 1] = source[src + 1];
                rgbaPixels[dst + 2] = source[src + 2];
                rgbaPixels[dst + 3] = source[src + 3];
                break;
            case 3:
                rgbaPixels[dst + 0] = source[src + 0];
                rgbaPixels[dst + 1] = source[src + 1];
                rgbaPixels[dst + 2] = source[src + 2];
                rgbaPixels[dst + 3] = 255;
                break;
            case 1:
                rgbaPixels[dst + 0] = source[src + 0];
                rgbaPixels[dst + 1] = source[src + 0];
                rgbaPixels[dst + 2] = source[src + 0];
                rgbaPixels[dst + 3] = 255;
                break;
            default:
                return nullptr;
            }
        }

        const CursorSize cursorSize = ResolveSystemCursorSize();
        if (cursorSize.width != image.GetWidth() || cursorSize.height != image.GetHeight())
        {
            std::vector<unsigned char> scaledPixels(static_cast<size_t>(cursorSize.width) * static_cast<size_t>(cursorSize.height) * 4u, 0u);
            for (int y = 0; y < cursorSize.height; ++y)
            {
                const float sourceY = (static_cast<float>(y) + 0.5f) * static_cast<float>(image.GetHeight()) / static_cast<float>(cursorSize.height) - 0.5f;
                const int y0 = std::clamp(static_cast<int>(std::floor(sourceY)), 0, image.GetHeight() - 1);
                const int y1 = std::clamp(y0 + 1, 0, image.GetHeight() - 1);
                const float fy = std::clamp(sourceY - static_cast<float>(y0), 0.0f, 1.0f);

                for (int x = 0; x < cursorSize.width; ++x)
                {
                    const float sourceX = (static_cast<float>(x) + 0.5f) * static_cast<float>(image.GetWidth()) / static_cast<float>(cursorSize.width) - 0.5f;
                    const int x0 = std::clamp(static_cast<int>(std::floor(sourceX)), 0, image.GetWidth() - 1);
                    const int x1 = std::clamp(x0 + 1, 0, image.GetWidth() - 1);
                    const float fx = std::clamp(sourceX - static_cast<float>(x0), 0.0f, 1.0f);
                    const int dstIndex = (y * cursorSize.width + x) * 4;

                    for (int channel = 0; channel < 4; ++channel)
                    {
                        const float c00 = static_cast<float>(rgbaPixels[(y0 * image.GetWidth() + x0) * 4 + channel]);
                        const float c10 = static_cast<float>(rgbaPixels[(y0 * image.GetWidth() + x1) * 4 + channel]);
                        const float c01 = static_cast<float>(rgbaPixels[(y1 * image.GetWidth() + x0) * 4 + channel]);
                        const float c11 = static_cast<float>(rgbaPixels[(y1 * image.GetWidth() + x1) * 4 + channel]);
                        const float top = c00 + (c10 - c00) * fx;
                        const float bottom = c01 + (c11 - c01) * fx;
                        scaledPixels[dstIndex + channel] = static_cast<unsigned char>(std::round(top + (bottom - top) * fy));
                    }
                }
            }

            rgbaPixels = std::move(scaledPixels);
            GLFWimage cursorImage{};
            cursorImage.width = cursorSize.width;
            cursorImage.height = cursorSize.height;
            cursorImage.pixels = rgbaPixels.data();
            return glfwCreateCursor(&cursorImage, cursorSize.width / 2, cursorSize.height / 2);
        }

        GLFWimage cursorImage{};
        cursorImage.width = image.GetWidth();
        cursorImage.height = image.GetHeight();
        cursorImage.pixels = rgbaPixels.data();
        return glfwCreateCursor(&cursorImage, image.GetWidth() / 2, image.GetHeight() / 2);
    }

    void AddImageCursor(
        std::unordered_map<NLS::Cursor::ECursorShape, GLFWcursor*>& cursors,
        const NLS::Cursor::ECursorShape shape,
        const std::string& filename,
        GLFWcursor* fallback)
    {
        if (GLFWcursor* cursor = CreateImageCursor(filename))
            cursors[shape] = cursor;
        else
            cursors[shape] = fallback;
    }
}

NLS::Context::Device::Device(const NLS::Windowing::Settings::DeviceSettings& p_deviceSettings)
{
	EnsureWslgEnvironment();
	BindErrorCallback();

#ifdef _WIN32
    glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_WIN32);
#endif

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
			glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);

		glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, p_deviceSettings.contextMajorVersion);
		glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, p_deviceSettings.contextMinorVersion);

		// Use ANY profile for better compatibility on WSL/virtualized GL drivers.
		glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_ANY_PROFILE);
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

NLS::Maths::Vector4 NLS::Context::Device::GetMonitorWorkarea() const
{
    GLFWmonitor* primaryMonitor = glfwGetPrimaryMonitor();
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    glfwGetMonitorWorkarea(primaryMonitor, &x, &y, &width, &height);

    if (width <= 0 || height <= 0)
    {
        const GLFWvidmode* mode = glfwGetVideoMode(primaryMonitor);
        return NLS::Maths::Vector4(0.0f, 0.0f, static_cast<float>(mode->width), static_cast<float>(mode->height));
    }

    return NLS::Maths::Vector4(
        static_cast<float>(x),
        static_cast<float>(y),
        static_cast<float>(width),
        static_cast<float>(height));
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
    AddImageCursor(m_cursors, Cursor::ECursorShape::FPS_VIEW, "FPSView.png", m_cursors[Cursor::ECursorShape::ARROW]);
    AddImageCursor(m_cursors, Cursor::ECursorShape::PAN_VIEW, "PanView.png", m_cursors[Cursor::ECursorShape::HAND]);
    AddImageCursor(m_cursors, Cursor::ECursorShape::ORBIT_VIEW, "OrbitView.png", m_cursors[Cursor::ECursorShape::HAND]);
    AddImageCursor(m_cursors, Cursor::ECursorShape::SLIDE_ARROW, "SlideArrow.png", m_cursors[Cursor::ECursorShape::HRESIZE]);
}

void NLS::Context::Device::DestroyCursors()
{
    std::unordered_set<GLFWcursor*> destroyedCursors;
    for (auto& [shape, cursor] : m_cursors)
    {
        (void)shape;
        if (cursor != nullptr && !destroyedCursors.contains(cursor))
        {
            glfwDestroyCursor(cursor);
            destroyedCursors.insert(cursor);
        }
    }
    m_cursors.clear();
}
