#include "Window.h"
#include <thread>

#ifdef __ORBIS__
#include "../Plugins/PlayStation4/PS4Window.h"
#endif

#include "RendererBase.h"

using namespace NLS;
using namespace Rendering;

Window*		Window::window		= nullptr;
Keyboard*	Window::keyboard	= nullptr;
Mouse*		Window::mouse		= nullptr;
GameTimer*	Window::timer		= nullptr;

Window::Window()	{
	renderer	= nullptr;
	window		= this;
	timer		= new GameTimer();
}

Window::~Window()	{
	delete keyboard;keyboard= nullptr;
	delete mouse;	mouse	= nullptr;
	delete timer;	timer	= nullptr;
	window = nullptr;
	delete timer;
}

void	Window::SetRenderer(RendererBase* r) {
	if (renderer && renderer != r) {
		renderer->OnWindowDetach();
	}
	renderer = r;

	if (r) {
		renderer->OnWindowResize((int)size.x, (int)size.y);
	}
}

bool	Window::UpdateWindow() {
	std::this_thread::yield();
	timer->Tick();

	if (mouse) {
		mouse->UpdateFrameState(timer->GetTimeDeltaMSec());
	}
	if (keyboard) {
		keyboard->UpdateFrameState(timer->GetTimeDeltaMSec());
	}

	return InternalUpdate();
}

void Window::ResizeRenderer() {
	if (renderer) {
		renderer->OnWindowResize((int)size.x, (int)size.y);
	}
}