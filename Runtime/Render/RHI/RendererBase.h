/******************************************************************************
Class:RendererBase
Implements:
Author:Rich Davison
Description:TODO

-_-_-_-_-_-_-_,------,
_-_-_-_-_-_-_-|   /\_/\   NYANYANYAN
-_-_-_-_-_-_-~|__( ^ .^) /
_-_-_-_-_-_-_-""  ""

*/
/////////////////////////////////////////////////////////////////////////////
#pragma once

#include "RenderDef.h"

#include <string>

#include "Vector2.h"
#include "Vector3.h"
#include "Vector4.h"

namespace NLS
{
class Window;

namespace Rendering
{
class ShaderBase;

enum class NLS_RENDER_API VerticalSyncState
{
    VSync_ON,
    VSync_OFF,
    VSync_ADAPTIVE
};
class NLS_RENDER_API RendererBase
{
public:
    RendererBase(Window& w);
    virtual ~RendererBase();

    virtual bool HasInitialised() const { return true; }

    virtual void Update(float dt) {}

    void Render()
    {
        BeginFrame();
        RenderFrame();
        EndFrame();
        SwapBuffers();
    }

    virtual bool SetVerticalSync(VerticalSyncState s)
    {
        return false;
    }

    virtual void OnWindowResize(int w, int h) = 0;
    virtual void OnWindowDetach(){}; // Most renderers won't care about this

    virtual ShaderBase* CreateShader(const std::string& vertex, const std::string& fragment) = 0;

    virtual void DrawString(const std::string& text, const Maths::Vector2& pos, const Maths::Vector4& colour, float size) = 0;
    virtual void DrawLine(const Maths::Vector3& start, const Maths::Vector3& end, const Maths::Vector4& colour) = 0;

    void DrawStringGray(const std::string& text, const Maths::Vector2& pos);

protected:
    virtual void BeginFrame() = 0;
    virtual void RenderFrame() = 0;
    virtual void EndFrame() = 0;
    virtual void SwapBuffers() = 0;
    Window& hostWindow;

    int currentWidth = 0;
    int currentHeight = 0;
};
} // namespace Rendering
} // namespace NLS
