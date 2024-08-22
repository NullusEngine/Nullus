#pragma once

#include "EngineDef.h"
#include "GameObject.h"

namespace NLS
{
namespace Engine
{
class NLS_ENGINE_API Actor
{
public:
    GameObject* GetGameObject() const { return mGameObject; }
    void SetGameObject(GameObject* pGameObject) { mGameObject = pGameObject; }

private:
    GameObject* mGameObject = nullptr;
};
} // namespace Engine
} // namespace NLS
