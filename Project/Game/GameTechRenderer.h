#pragma once
#include "OGLRenderer.h"
#include "OGLShader.h"
#include "OGLTexture.h"
#include "OGLMesh.h"

#include "GameWorld.h"

namespace NLS
{
namespace Engine
{
class RenderObject;

class GameTechRenderer : public OGLRenderer
{
public:
    GameTechRenderer(GameWorld& world);
    ~GameTechRenderer();

protected:
    void RenderFrame() override;

    Matrix4 SetupDebugLineMatrix() const override;
    Matrix4 SetupDebugStringMatrix() const override;

    OGLShader* defaultShader;

    GameWorld& gameWorld;

    void BuildObjectList();
    void SortObjectList();
    void RenderShadowMap();
    void RenderCamera();
    void RenderSkybox();

    void LoadSkybox();

    vector<const RenderObject*> activeObjects;

    OGLShader* skyboxShader;
    OGLMesh* skyboxMesh;
    GLuint skyboxTex;

    // shadow mapping things
    OGLShader* shadowShader;
    GLuint shadowTex;
    GLuint shadowFBO;
    Matrix4 shadowMatrix;

    Vector4 lightColour;
    float lightRadius;
    Vector3 lightPosition;
};
} // namespace Engine
} // namespace NLS
