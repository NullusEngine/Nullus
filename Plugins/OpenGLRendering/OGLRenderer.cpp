/*
Part of Newcastle University's Game Engineering source code.

Use as you see fit!

Comments and queries to: richard-gordon.davison AT ncl.ac.uk
https://research.ncl.ac.uk/game/
*/
#include "OGLRenderer.h"
#include "OGLShader.h"
#include "OGLMesh.h"
#include "OGLTexture.h"

#include "SimpleFont.h"
#include "TextureLoader.h"

#include "Vector2.h"
#include "Vector3.h"
#include "Matrix4.h"

#include "RHI/MeshGeometry.h"

#include <GLFW/glfw3.h>
#include "Windowing/Window.h"
using namespace NLS;
using namespace NLS::Rendering;

#ifdef OPENGL_DEBUGGING
static void APIENTRY DebugCallback(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar* message, const void* userParam);
#endif

OGLRenderer::OGLRenderer(Window& w)
    : RendererBase(w)
{
    if (gladLoadGLLoader((GLADloadproc)glfwGetProcAddress))
    {
        initState = true;
    }
    else
    {
        std::cout << "Failed to initialize GLAD" << std::endl;
        initState = false;
    }
    boundMesh = nullptr;
    boundShader = nullptr;

    currentWidth = (int)w.GetSize().x;
    currentHeight = (int)w.GetSize().y;

    if (initState)
    {
        TextureLoader::RegisterAPILoadFunction(OGLTexture::RGBATextureFromFilename);

        font = new SimpleFont("PressStart2P.fnt", "PressStart2P.png");

        OGLTexture* t = (OGLTexture*)font->GetTexture();

        if (t)
        {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, t->GetObjectID());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
        glClearColor(0.2f, 0.2f, 0.2f, 1.0f);
        debugShader = new OGLShader("debugVert.glsl", "debugFrag.glsl");
    }

    forceValidDebugState = false;

    debugLinesMesh = new OGLMesh();
    debugTextMesh = new OGLMesh();


    debugLinesMesh->SetVertexPositions(std::vector<Vector3>(5000, Vector3()));
    debugLinesMesh->SetVertexColours(std::vector<Vector4>(5000, Vector3()));

    debugTextMesh->SetVertexPositions(std::vector<Vector3>(5000, Vector3()));
    debugTextMesh->SetVertexColours(std::vector<Vector4>(5000, Vector3()));
    debugTextMesh->SetVertexTextureCoords(std::vector<Vector2>(5000, Vector3()));

    debugTextMesh->UploadToGPU();
    debugLinesMesh->UploadToGPU();

    debugLinesMesh->SetPrimitiveType(GeometryPrimitive::Lines);
}

OGLRenderer::~OGLRenderer()
{
    delete font;
    delete debugShader;

}

void OGLRenderer::OnWindowResize(int w, int h)
{
    currentWidth = w;
    currentHeight = h;
    glViewport(0, 0, currentWidth, currentHeight);
}

void OGLRenderer::BeginFrame()
{
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    BindShader(nullptr);
    BindMesh(nullptr);
}

void OGLRenderer::RenderFrame()
{
}

void OGLRenderer::EndFrame()
{
    DrawDebugData();
}

void OGLRenderer::SwapBuffers()
{
    Window::GetWindow()->SwapBuffers();
}

void OGLRenderer::BindShader(ShaderBase* s)
{
    if (!s)
    {
        glUseProgram(0);
        boundShader = nullptr;
    }
    else if (OGLShader* oglShader = dynamic_cast<OGLShader*>(s))
    {
        glUseProgram(oglShader->programID);
        boundShader = oglShader;
    }
    else
    {
        std::cout << __FUNCTION__ << " has received invalid shader?!" << std::endl;
        boundShader = nullptr;
    }
}

void OGLRenderer::BindMesh(MeshGeometry* m)
{
    if (!m)
    {
        glBindVertexArray(0);
        boundMesh = nullptr;
    }
    else if (OGLMesh* oglMesh = dynamic_cast<OGLMesh*>(m))
    {
        if (oglMesh->GetVAO() == 0)
        {
            std::cout << __FUNCTION__ << " has received invalid mesh?!" << std::endl;
        }
        glBindVertexArray(oglMesh->GetVAO());
        boundMesh = oglMesh;
    }
    else
    {
        std::cout << __FUNCTION__ << " has received invalid mesh?!" << std::endl;
        boundMesh = nullptr;
    }
}

void OGLRenderer::DrawBoundMesh(int subLayer, int numInstances)
{
    if (!boundMesh)
    {
        std::cout << __FUNCTION__ << " has been called without a bound mesh!" << std::endl;
        return;
    }
    if (!boundShader)
    {
        std::cout << __FUNCTION__ << " has been called without a bound shader!" << std::endl;
        return;
    }
    GLuint mode = 0;
    int count = 0;
    int offset = 0;

    if (boundMesh->GetSubMeshCount() == 0)
    {
        if (boundMesh->GetIndexCount() > 0)
        {
            count = boundMesh->GetIndexCount();
        }
        else
        {
            count = boundMesh->GetVertexCount();
        }
    }
    else
    {
        const SubMesh* m = boundMesh->GetSubMesh(subLayer);
        offset = m->start;
        count = m->count;
    }

    switch (boundMesh->GetPrimitiveType())
    {
        case GeometryPrimitive::Triangles:
            mode = GL_TRIANGLES;
            break;
        case GeometryPrimitive::Points:
            mode = GL_POINTS;
            break;
        case GeometryPrimitive::Lines:
            mode = GL_LINES;
            break;
        case GeometryPrimitive::TriangleFan:
            mode = GL_TRIANGLE_FAN;
            break;
        case GeometryPrimitive::TriangleStrip:
            mode = GL_TRIANGLE_STRIP;
            break;
        case GeometryPrimitive::Patches:
            mode = GL_PATCHES;
            break;
    }

    if (boundMesh->GetIndexCount() > 0)
    {
        glDrawElements(mode, count, GL_UNSIGNED_INT, (const GLvoid*)(offset * sizeof(unsigned int)));
    }
    else
    {
        glDrawArrays(mode, 0, count);
    }
}

void OGLRenderer::BindTextureToShader(const TextureBase* t, const std::string& uniform, int texUnit) const
{
    GLint texID = 0;

    if (!boundShader)
    {
        std::cout << __FUNCTION__ << " has been called without a bound shader!" << std::endl;
        return; // Debug message time!
    }

    GLuint slot = glGetUniformLocation(boundShader->programID, uniform.c_str());

    if (slot < 0)
    {
        return;
    }

    if (const OGLTexture* oglTexture = dynamic_cast<const OGLTexture*>(t))
    {
        texID = oglTexture->GetObjectID();
    }

    glActiveTexture(GL_TEXTURE0 + texUnit);
    glBindTexture(GL_TEXTURE_2D, texID);

    glUniform1i(slot, texUnit);
}

void OGLRenderer::DrawString(const std::string& text, const Vector2& pos, const Vector4& colour, float size)
{
    DebugString s;
    s.colour = colour;
    s.pos = pos;
    s.size = size;
    s.text = text;
    debugStrings.emplace_back(s);
}

void OGLRenderer::DrawLine(const Vector3& start, const Vector3& end, const Vector4& colour)
{
    DebugLine l;
    l.start = start;
    l.end = end;
    l.colour = colour;
    debugLines.emplace_back(l);
}

Matrix4 OGLRenderer::SetupDebugLineMatrix() const
{
    return Matrix4();
}
Matrix4 OGLRenderer::SetupDebugStringMatrix() const
{
    return Matrix4();
}

void OGLRenderer::DrawDebugData()
{
    if (debugStrings.empty() && debugLines.empty())
    {
        return; // don't mess with OGL state if there's no point!
    }
    BindShader(debugShader);

    if (forceValidDebugState)
    {
        glEnable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    int matLocation = glGetUniformLocation(debugShader->GetProgramID(), "viewProjMatrix");
    Matrix4 pMat;

    BindTextureToShader(font->GetTexture(), "mainTex", 0);

    GLuint texSlot = glGetUniformLocation(boundShader->programID, "useTexture");

    if (debugLines.size() > 0)
    {
        pMat = SetupDebugLineMatrix();
        glUniformMatrix4fv(matLocation, 1, false, pMat.array);
        glUniform1i(texSlot, 0);
        DrawDebugLines();
    }

    if (debugStrings.size() > 0)
    {
        pMat = SetupDebugStringMatrix();
        glUniformMatrix4fv(matLocation, 1, false, pMat.array);
        glUniform1i(texSlot, 1);
        DrawDebugStrings();
    }

    if (forceValidDebugState)
    {
        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }
}

void OGLRenderer::DrawDebugStrings()
{
    vector<Vector3> vertPos;
    vector<Vector2> vertTex;
    vector<Vector4> vertColours;

    for (DebugString& s : debugStrings)
    {
        font->BuildVerticesForString(s.text, s.pos, s.colour, s.size, vertPos, vertTex, vertColours);
    }

    debugTextMesh->SetVertexPositions(vertPos);
    debugTextMesh->SetVertexTextureCoords(vertTex);
    debugTextMesh->SetVertexColours(vertColours);
    debugTextMesh->UpdateGPUBuffers(0, vertPos.size());

    BindMesh(debugTextMesh);
    DrawBoundMesh();

    debugStrings.clear();
}

void OGLRenderer::DrawDebugLines()
{
    vector<Vector3> vertPos;
    vector<Vector4> vertCol;

    for (DebugLine& s : debugLines)
    {
        vertPos.emplace_back(s.start);
        vertPos.emplace_back(s.end);

        vertCol.emplace_back(s.colour);
        vertCol.emplace_back(s.colour);
    }

    debugLinesMesh->SetVertexPositions(vertPos);
    debugLinesMesh->SetVertexColours(vertCol);
    debugLinesMesh->UpdateGPUBuffers(0, vertPos.size());

    BindMesh(debugLinesMesh);
    DrawBoundMesh();

    debugLines.clear();
}

void OGLRenderer::SetVerticalSync(VerticalSyncState s)
{
    switch (s)
    {
        case VerticalSyncState::VSync_OFF:
            Window::GetWindow()->GetDevice()->SetVsync(false);
            break;
        case VerticalSyncState::VSync_ON:
            Window::GetWindow()->GetDevice()->SetVsync(true);
            break;
        case VerticalSyncState::VSync_ADAPTIVE:        
            break;
    }
}

#ifdef OPENGL_DEBUGGING
static void APIENTRY DebugCallback(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar* message, const void* userParam)
{
    string sourceName;
    string typeName;
    string severityName;

    switch (source)
    {
        case GL_DEBUG_SOURCE_API_ARB:
            sourceName = "Source(OpenGL)";
            break;
        case GL_DEBUG_SOURCE_WINDOW_SYSTEM_ARB:
            sourceName = "Source(Window System)";
            break;
        case GL_DEBUG_SOURCE_SHADER_COMPILER_ARB:
            sourceName = "Source(Shader Compiler)";
            break;
        case GL_DEBUG_SOURCE_THIRD_PARTY_ARB:
            sourceName = "Source(Third Party)";
            break;
        case GL_DEBUG_SOURCE_APPLICATION_ARB:
            sourceName = "Source(Application)";
            break;
        case GL_DEBUG_SOURCE_OTHER_ARB:
            sourceName = "Source(Other)";
            break;
    }

    switch (type)
    {
        case GL_DEBUG_TYPE_ERROR_ARB:
            typeName = "Type(Error)";
            break;
        case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR_ARB:
            typeName = "Type(Deprecated Behaviour)";
            break;
        case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR_ARB:
            typeName = "Type(Undefined Behaviour)";
            break;
        case GL_DEBUG_TYPE_PORTABILITY_ARB:
            typeName = "Type(Portability)";
            break;
        case GL_DEBUG_TYPE_PERFORMANCE_ARB:
            typeName = "Type(Performance)";
            break;
        case GL_DEBUG_TYPE_OTHER_ARB:
            typeName = "Type(Other)";
            break;
    }

    switch (severity)
    {
        case GL_DEBUG_SEVERITY_HIGH_ARB:
            severityName = "Priority(High)";
            break;
        case GL_DEBUG_SEVERITY_MEDIUM_ARB:
            severityName = "Priority(Medium)";
            break;
        case GL_DEBUG_SEVERITY_LOW_ARB:
            severityName = "Priority(Low)";
            break;
    }

    std::cout << "OpenGL Debug Output: " + sourceName + ", " + typeName + ", " + severityName + ", " + string(message) << std::endl;
}
#endif