#include "GameTechRenderer.h"
#include "GameObject.h"
#include "Camera.h"
#include "Vector2.h"
#include "Math/Vector3.h"
#include "TextureLoader.h"
#include "MeshLoader.h"
#ifdef NLS_USE_GL
    #include "OGLShader.h"
#endif
using namespace NLS;
using namespace Rendering;
using namespace Engine;

#define SHADOWSIZE 4096

Matrix4 biasMatrix = Matrix4::Translation(Vector3(0.5, 0.5, 0.5)) * Matrix4::Scale(Vector3(0.5, 0.5, 0.5));

GameTechRenderer::GameTechRenderer(GameWorld& world)
#ifdef NLS_USE_GL
    : OGLRenderer(*Window::GetWindow()), gameWorld(world)
#else
    : VulkanRenderer(*Window::GetWindow()), gameWorld(world)
#endif
{
#ifdef NLS_USE_GL
    // Shadow
    shadowShader = CreateShader("GameTechShadowVert.glsl", "GameTechShadowFrag.glsl");

    glEnable(GL_DEPTH_TEST);

    glGenTextures(1, &shadowTex);
    glBindTexture(GL_TEXTURE_2D, shadowTex);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, SHADOWSIZE, SHADOWSIZE, 0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_R_TO_TEXTURE);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &shadowFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, shadowFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, shadowTex, 0);
    glDrawBuffer(GL_NONE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    glClearColor(1, 1, 1, 1);

#endif

    // Set up the light properties
    lightColour = Vector4(0.8f, 0.8f, 0.5f, 1.0f);
    lightRadius = 1000.0f;
    lightPosition = Vector3(-200.0f, 60.0f, -200.0f);

    // Skybox!
    skyboxShader = CreateShader("skyboxVertex.glsl", "skyboxFragment.glsl");
    skyboxMesh = MeshLoader::LoadAPIMesh(std::string());
    skyboxMesh->SetVertexPositions({Vector3(-1, 1, -1), Vector3(-1, -1, -1), Vector3(1, -1, -1), Vector3(1, 1, -1)});
    skyboxMesh->SetVertexIndices({0, 1, 2, 2, 3, 0});
    skyboxMesh->UploadToGPU(this);

    LoadSkybox();
}

GameTechRenderer::~GameTechRenderer()
{
#ifdef NLS_USE_GL
    glDeleteTextures(1, &shadowTex);
    glDeleteFramebuffers(1, &shadowFBO);
#endif
}

void GameTechRenderer::LoadSkybox()
{
    CubeMapFileNames filenames = {
        "/Cubemap/skyrender0004.png",
        "/Cubemap/skyrender0001.png",
        "/Cubemap/skyrender0003.png",
        "/Cubemap/skyrender0006.png",
        "/Cubemap/skyrender0002.png",
        "/Cubemap/skyrender0005.png"};

    skyboxTex = TextureLoader::LoadAPICubeMap(filenames);
}

void GameTechRenderer::RenderFrame()
{
#ifdef NLS_USE_GL
    glEnable(GL_CULL_FACE);
    glClearColor(1, 1, 1, 1);
#endif
    BuildObjectList();
    SortObjectList();
    RenderShadowMap();
    RenderSkybox();
    RenderCamera();
#ifdef NLS_USE_GL
    glDisable(GL_CULL_FACE); // Todo - text indices are going the wrong way...
#endif
}

void GameTechRenderer::BuildObjectList()
{
    activeObjects.clear();

    gameWorld.OperateOnContents(
        [&](GameObject* o)
        {
            if (o->IsActive())
            {
                const RenderObject* g = o->GetRenderObject();
                if (g)
                {
                    activeObjects.emplace_back(g);
                }
            }
        });
}

void GameTechRenderer::SortObjectList()
{
    // Who cares!
}

void GameTechRenderer::RenderShadowMap()
{
#ifdef NLS_USE_GL
    glBindFramebuffer(GL_FRAMEBUFFER, shadowFBO);
    glClear(GL_DEPTH_BUFFER_BIT);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glViewport(0, 0, SHADOWSIZE, SHADOWSIZE);

    glCullFace(GL_FRONT);

    BindShader(shadowShader);
    int mvpLocation = glGetUniformLocation(((OGLShader*)shadowShader)->GetProgramID(), "mvpMatrix");

    Matrix4 shadowViewMatrix = Matrix4::BuildViewMatrix(lightPosition, Vector3(0, 0, 0), Vector3(0, 1, 0));
    Matrix4 shadowProjMatrix = Matrix4::Perspective(100.0f, 500.0f, 1, 45.0f);

    Matrix4 mvMatrix = shadowProjMatrix * shadowViewMatrix;

    shadowMatrix = biasMatrix * mvMatrix; // we'll use this one later on

    for (const auto& i : activeObjects)
    {
        Matrix4 modelMatrix = (*i).GetTransform()->GetMatrix();
        Matrix4 mvpMatrix = mvMatrix * modelMatrix;
        glUniformMatrix4fv(mvpLocation, 1, false, (float*)&mvpMatrix);
        BindMesh((*i).GetMesh());
        int layerCount = (*i).GetMesh()->GetSubMeshCount();
        for (int i = 0; i < layerCount; ++i)
        {
            DrawBoundMesh(i);
        }
    }

    glViewport(0, 0, currentWidth, currentHeight);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    glCullFace(GL_BACK);
#endif
}

void GameTechRenderer::RenderSkybox()
{
    float screenAspect = (float)currentWidth / (float)currentHeight;
    Matrix4 viewMatrix = gameWorld.GetMainCamera()->BuildViewMatrix();
    Matrix4 projMatrix = gameWorld.GetMainCamera()->BuildProjectionMatrix(screenAspect);

#ifdef NLS_USE_GL
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);

    BindShader(skyboxShader);

    auto glSkyboxShader = (OGLShader*)skyboxShader;
    int projLocation = glGetUniformLocation(glSkyboxShader->GetProgramID(), "projMatrix");
    int viewLocation = glGetUniformLocation(glSkyboxShader->GetProgramID(), "viewMatrix");
    int texLocation = glGetUniformLocation(glSkyboxShader->GetProgramID(), "cubeTex");

    glUniformMatrix4fv(projLocation, 1, false, (float*)&projMatrix);
    glUniformMatrix4fv(viewLocation, 1, false, (float*)&viewMatrix);

    glUniform1i(texLocation, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, ((OGLTexture*)skyboxTex)->GetObjectID());

    BindMesh(skyboxMesh);
    DrawBoundMesh();

    glEnable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
#else
    auto vkSkyboxShader = (VulkanShader*)skyboxShader;
#endif
}

void GameTechRenderer::RenderCamera()
{
#ifdef NLS_USE_GL
    float screenAspect = (float)currentWidth / (float)currentHeight;
    Matrix4 viewMatrix = gameWorld.GetMainCamera()->BuildViewMatrix();
    Matrix4 projMatrix = gameWorld.GetMainCamera()->BuildProjectionMatrix(screenAspect);

    ShaderBase* activeShader = nullptr;
    int projLocation = 0;
    int viewLocation = 0;
    int modelLocation = 0;
    int colourLocation = 0;
    int hasVColLocation = 0;
    int hasTexLocation = 0;
    int shadowLocation = 0;

    int lightPosLocation = 0;
    int lightColourLocation = 0;
    int lightRadiusLocation = 0;

    int cameraLocation = 0;

    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, shadowTex);

    for (const auto& i : activeObjects)
    {
        OGLShader* shader = (OGLShader*)(*i).GetShader();
        BindShader(shader);

        BindTextureToShader((OGLTexture*)(*i).GetDefaultTexture(), "mainTex", 0);

        if (activeShader != shader)
        {
            projLocation = glGetUniformLocation(shader->GetProgramID(), "projMatrix");
            viewLocation = glGetUniformLocation(shader->GetProgramID(), "viewMatrix");
            modelLocation = glGetUniformLocation(shader->GetProgramID(), "modelMatrix");
            shadowLocation = glGetUniformLocation(shader->GetProgramID(), "shadowMatrix");
            colourLocation = glGetUniformLocation(shader->GetProgramID(), "objectColour");
            hasVColLocation = glGetUniformLocation(shader->GetProgramID(), "hasVertexColours");
            hasTexLocation = glGetUniformLocation(shader->GetProgramID(), "hasTexture");

            lightPosLocation = glGetUniformLocation(shader->GetProgramID(), "lightPos");
            lightColourLocation = glGetUniformLocation(shader->GetProgramID(), "lightColour");
            lightRadiusLocation = glGetUniformLocation(shader->GetProgramID(), "lightRadius");

            cameraLocation = glGetUniformLocation(shader->GetProgramID(), "cameraPos");
            auto&& mainCameraPosition = gameWorld.GetMainCamera()->GetPosition();
            glUniform3fv(cameraLocation, 1, (float*)&mainCameraPosition);

            glUniformMatrix4fv(projLocation, 1, false, (float*)&projMatrix);
            glUniformMatrix4fv(viewLocation, 1, false, (float*)&viewMatrix);

            glUniform3fv(lightPosLocation, 1, (float*)&lightPosition);
            glUniform4fv(lightColourLocation, 1, (float*)&lightColour);
            glUniform1f(lightRadiusLocation, lightRadius);

            int shadowTexLocation = glGetUniformLocation(shader->GetProgramID(), "shadowTex");
            glUniform1i(shadowTexLocation, 1);

            activeShader = shader;
        }

        Matrix4 modelMatrix = (*i).GetTransform()->GetMatrix();
        glUniformMatrix4fv(modelLocation, 1, false, (float*)&modelMatrix);

        Matrix4 fullShadowMat = shadowMatrix * modelMatrix;
        glUniformMatrix4fv(shadowLocation, 1, false, (float*)&fullShadowMat);

        auto&& Colour = i->GetColour();
        glUniform4fv(colourLocation, 1, (float*)&Colour);

        glUniform1i(hasVColLocation, !(*i).GetMesh()->GetColourData().empty());

        glUniform1i(hasTexLocation, (OGLTexture*)(*i).GetDefaultTexture() ? 1 : 0);

        BindMesh((*i).GetMesh());
        int layerCount = (*i).GetMesh()->GetSubMeshCount();
        for (int i = 0; i < layerCount; ++i)
        {
            DrawBoundMesh(i);
        }
    }
#endif
}

#ifdef NLS_USE_GL
Matrix4 GameTechRenderer::SetupDebugLineMatrix() const
{
    float screenAspect = (float)currentWidth / (float)currentHeight;
    Matrix4 viewMatrix = gameWorld.GetMainCamera()->BuildViewMatrix();
    Matrix4 projMatrix = gameWorld.GetMainCamera()->BuildProjectionMatrix(screenAspect);

    return projMatrix * viewMatrix;
}

Matrix4 GameTechRenderer::SetupDebugStringMatrix() const
{
    return Matrix4::Orthographic(-1, 1.0f, 100, 0, 0, 100);
}
#endif
