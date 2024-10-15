
#include "GameObjectSerialize.h"
#include "Serialize/Serializer.h"
#include "Debug/Assertion.h"
#include "GameObject.h"
namespace NLS
{
void GameObjectSerializeHandler::SerializeImpl(const ObjectView& obj, json& j) const
{
    
}

void GameObjectSerializeHandler::DeserializeImpl(const ObjectView& obj, const json& input) const
{
    
}


uint32_t GameObjectSerializeHandler::CalcMatchLevel(const Type& type, bool isPointer) const
{
    if (type == Type_of<Engine::GameObject>)
        return 0;
    return NoMatch;
}
} // namespace NLS
