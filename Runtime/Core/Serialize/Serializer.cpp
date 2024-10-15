#include "Serialize/Serializer.h"
#include "Debug/Logger.h"
#include "Debug/Assertion.h"

namespace NLS
{

void Serializer::SerializeObject(const ObjectView& obj, json& output)
{
    const IJsonHandler* handler = GetHandler(obj.GetType(), obj.GetType().IsPointer());
    if (handler == nullptr)
    {
        return;
    }
    handler->SerializeImpl(obj, output);
}


std::string Serializer::SerializeToString(const ObjectView& obj)
{
    json output;
    SerializeObject(obj, output);
    return output.dump(4);
}


void Serializer::SerializeToFile(const ObjectView& obj, const std::string& p_path)
{
    std::ofstream o(p_path);
    json output;
    SerializeObject(obj, output);
    o << std::setw(4) << output << std::endl;
}

void Serializer::DeserializeObject(const ObjectView& obj, const json& input)
{
    const IJsonHandler* handler = GetHandler(obj.GetType(), obj.GetType().IsPointer());
    if (handler == nullptr)
    {
        return;
    }
    handler->DeserializeImpl(obj, input);
}



void Serializer::DeserializeFromString(const ObjectView& obj, const std::string& str)
{
    json input = json::parse(str);
    DeserializeObject(obj, input);
}


void Serializer::DeserializeFromFile(const ObjectView& obj, const std::string& p_path)
{
    std::ifstream file(p_path);
    json input;
    file >> input;
    DeserializeObject(obj, input);
}

const NLS::IJsonHandler* Serializer::GetHandler(const Type& type, bool isPointer)
{
    NLS_ASSERT(!mHandlers.empty(), "No serialization strategy available!");

    const IJsonHandler* ret = nullptr;
    uint32_t retLvl = IJsonHandler::NoMatch;
    for (const auto handler : mHandlers)
    {
        const uint32_t tempLvl = handler->CalcMatchLevel(type, isPointer);
        if (tempLvl == retLvl && retLvl == 0)
            NLS_LOG_ERROR("Type[%s][isPointer: %s] should not match two handler perfectly.", type.getName(), isPointer ? "true" : "false");
        if (tempLvl < retLvl)
        {
            ret = handler;
            retLvl = tempLvl;
        }
    }
    if (retLvl == IJsonHandler::NoMatch)
    {
        NLS_LOG_ERROR("Type[%s][isPointer: %s] has no legal handler.", type.getName(), isPointer ? "true" : "false");
        return ret;
    }

    return ret;
}


} // namespace NLS
