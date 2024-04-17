﻿
#pragma once
#include "RHI/RendererBase.h"
#include "OGLDef.h"
#include "Vector3.h"
#include "Vector4.h"

#include "Windowing/Window.h"

#ifdef _WIN32
    #include "windows.h"
#endif

#ifdef _DEBUG
    #define OPENGL_DEBUGGING
#endif


#include <string>
#include <vector>

namespace NLS
{
class MeshGeometry;
using namespace Maths;
namespace Maths
{
class Matrix4;
}

namespace Rendering
{
class ShaderBase;
class TextureBase;

class OGLMesh;
class OGLShader;

class SimpleFont;

class OGL_API OGLRenderer : public RendererBase
{
public:
    OGLRenderer(Window& w);
    ~OGLRenderer();

    void OnWindowResize(int w, int h) override;
    bool HasInitialised() const override
    {
        return initState;
    }

    void ForceValidDebugState(bool newState)
    {
        forceValidDebugState = newState;
    }

    virtual void SetVerticalSync(VerticalSyncState s) override;

    void DrawString(const std::string& text, const Vector2& pos, const Vector4& colour = Vector4(0.75f, 0.75f, 0.75f, 1), float size = 20.0f);
    void DrawLine(const Vector3& start, const Vector3& end, const Vector4& colour);

    virtual Matrix4 SetupDebugLineMatrix() const;
    virtual Matrix4 SetupDebugStringMatrix() const;

protected:
    void BeginFrame() override;
    void RenderFrame() override;
    void EndFrame() override;
    void SwapBuffers() override;

    void DrawDebugData();
    void DrawDebugStrings();
    void DrawDebugLines();

    void BindShader(ShaderBase* s);
    void BindTextureToShader(const TextureBase* t, const std::string& uniform, int texUnit) const;
    void BindMesh(MeshGeometry* m);
    void DrawBoundMesh(int subLayer = 0, int numInstances = 1);

private:
    struct DebugString
    {
        Maths::Vector4 colour;
        Maths::Vector2 pos;
        float size;
        std::string text;
    };

    struct DebugLine
    {
        Maths::Vector3 start;
        Maths::Vector3 end;
        Maths::Vector4 colour;
    };

    OGLMesh* debugLinesMesh;
    OGLMesh* debugTextMesh;

    OGLMesh* boundMesh;
    OGLShader* boundShader;

    OGLShader* debugShader;
    SimpleFont* font;
    std::vector<DebugString> debugStrings;
    std::vector<DebugLine> debugLines;

    bool initState;
    bool forceValidDebugState;
};
} // namespace Rendering
} // namespace NLS