

#include "Components/SkyBoxComponent.h"

#include "Core/ServiceLocator.h"
#include "Core/ResourceManagement/TextureManager.h"
#include "Core/ResourceManagement/MeshManager.h"
#include "Core/ResourceManagement/ShaderManager.h"
#include "Components/MeshRenderer.h"
#include "GameObject.h"
#include <Debug/Logger.h>
#include <fstream>

namespace
{
    void AppendSkyboxTrace(const char* message)
    {
        (void)message;
    }
}

namespace NLS::Engine::Components
{
SkyBoxComponent::SkyBoxComponent()
{
    AppendSkyboxTrace("loading sky cube mesh");
    mMesh = NLS_SERVICE(NLS::Core::ResourceManagement::MeshManager)[":Models/SkyCube.obj"];

    AppendSkyboxTrace("loading skybox shader");
    auto shader = NLS_SERVICE(NLS::Core::ResourceManagement::ShaderManager)[":Shaders/Skybox.hlsl"];
    AppendSkyboxTrace("constructing material");
    mMaterial = new Render::Resources::Material(shader);
    mMaterial->SetDepthWriting(false);
    mMaterial->SetBackfaceCulling(false);
    mMaterial->SetFrontfaceCulling(false);
    AppendSkyboxTrace("applying default material parameters");
    mMaterial->Set("u_UseProceduralSky", true);
    mMaterial->Set("u_SkyTint", Maths::Vector3(0.50f, 0.62f, 0.82f));
    mMaterial->Set("u_GroundColor", Maths::Vector3(0.46f, 0.44f, 0.42f));
    mMaterial->Set("u_SunDirection", Maths::Vector3(-0.35f, 0.78f, -0.18f));
    mMaterial->Set("u_Exposure", 1.02f);
    mMaterial->Set("u_AtmosphereThickness", 0.85f);
    mMaterial->Set("u_SunSize", 0.0f);
    mMaterial->Set("u_SunSizeConvergence", 1.0f);
    AppendSkyboxTrace("initialization complete");
}

void SkyBoxComponent::SetCubeMap(Render::Resources::TextureCube* cubmap)
{
    mCubeMap = cubmap;
    if (cubmap != nullptr)
        mSkyMode = SkyMode::CubeMap;
    ApplySkyMode();
}

void SkyBoxComponent::SetSkyMode(SkyMode mode)
{
    mSkyMode = mode;
    ApplySkyMode();
}

void SkyBoxComponent::ApplySkyMode()
{
    if (mMaterial == nullptr)
        return;

    mMaterial->Set("u_UseProceduralSky", mSkyMode == SkyMode::Procedural);
    mMaterial->Set("cubeTex", mSkyMode == SkyMode::CubeMap ? mCubeMap : nullptr);
}
}
