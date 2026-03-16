#include "AssemblyEngine.h"
#include "Components/Component.h"
#include "Components/TransformComponent.h"
#include "Components/CameraComponent.h"
#include "Components/LightComponent.h"
#include "Components/MeshRenderer.h"
#include "Components/MaterialRenderer.h"
#include "Components/SkyBoxComponent.h"
#include "GameObject.h"
#include "Gen/MetaGenerated.h"
#include "Serialize/Serializer.h"
#include "SceneSystem/Scene.h"
#include "Serialize/GameobjectSerialize.h"

namespace
{
[[maybe_unused]] auto g_metaGeneratedEngineAnchor = &NLS_META_GENERATED_LINK_FUNCTION;
}

namespace NLS
{
namespace Engine
{
    using namespace Components;
void AssemblyEngine::Initialize()
{
    Serializer::Instance()->AddHandler<GameObjectSerializeHandler>();
}
} // namespace Engine
} // namespace NLS
