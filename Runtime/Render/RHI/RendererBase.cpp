#include "RendererBase.h"
using namespace NLS;
using namespace Rendering;

RendererBase::RendererBase(Window& window)
    : hostWindow(window)
{
}


RendererBase::~RendererBase()
{
}

void RendererBase::DrawStringGray(const std::string& text, const Maths::Vector2& pos)
{
    DrawString(text, pos, Maths::Vector4(0.75f, 0.75f, 0.75f, 1), 20.f);
}
