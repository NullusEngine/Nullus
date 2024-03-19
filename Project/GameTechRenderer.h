#pragma once
#ifdef NLS_USE_GL
#define USE_GL 1
#else
#define USE_GL 0
#endif

#include "RHI/ShaderBase.h"

#if USE_GL
#include "OGLRenderer.h"
#include "OGLTexture.h"
#else
#include "VulkanRenderer.h"
#endif

#include "GameWorld.h"

namespace NLS
{
class Maths::Vector3;
class Maths::Vector4;
namespace Engine
{
class RenderObject;

#if USE_GL
class GameTechRenderer : public OGLRenderer
#else
class GameTechRenderer : public VulkanRenderer
#endif
{
public:
    GameTechRenderer(GameWorld& world);
    ~GameTechRenderer();

protected:
    void RenderFrame() override;

#if USE_GL
    Matrix4 SetupDebugLineMatrix() const override;
    Matrix4 SetupDebugStringMatrix() const override;
#endif

    GameWorld& gameWorld;

    void BuildObjectList();
    void SortObjectList();
    void RenderShadowMap();
    void RenderCamera();
    void RenderSkybox();

    void LoadSkybox();

    vector<const RenderObject*> activeObjects;

    ShaderBase* skyboxShader;
    MeshGeometry* skyboxMesh;
    TextureBase* skyboxTex;

#if USE_GL
    // shadow mapping things
    ShaderBase* shadowShader;
    GLuint shadowTex;
    GLuint shadowFBO;
    Matrix4 shadowMatrix;
#endif

    Vector4 lightColour;
    float lightRadius;
    Vector3 lightPosition;
};
} // namespace Engine
} // namespace NLS
