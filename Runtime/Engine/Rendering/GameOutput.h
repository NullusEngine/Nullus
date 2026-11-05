#pragma once
#include "EngineDef.h"
#include "Components/CameraComponent.h"
#include "SceneSystem/Scene.h"
#include <algorithm>
#include <vector>
namespace NLS::Engine::Rendering
{
struct GameOutput
{
    uint32_t display = 0, width = 0, height = 0;
    uint64_t sessionId = 0, frameId = 0;
    // Used on the game thread only. Render packages freeze camera state.
    std::vector<Components::CameraComponent*> cameras;
    static GameOutput Build(SceneSystem::Scene& scene,uint32_t display,uint32_t width,uint32_t height)
    {
        GameOutput output;output.display=display;output.width=width;output.height=height;
        for(auto* camera : scene.GetFastAccessComponents().cameras)
            if(camera&&camera->IsActiveAndEnabled()&&camera->GetTargetDisplay()==static_cast<int>(display))
            {const auto rect=camera->GetViewportRect();if(rect.z>0&&rect.w>0)output.cameras.push_back(camera);}
        std::stable_sort(output.cameras.begin(),output.cameras.end(),[](const auto* a,const auto* b){return a->GetDepth()<b->GetDepth();});
        return output;
    }
};
}
