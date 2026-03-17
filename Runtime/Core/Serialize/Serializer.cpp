#include "Serialize/Serializer.h"
#include "Debug/Logger.h"
#include "Debug/Assertion.h"
#include <iomanip>
#include <fstream>

namespace NLS
{

void Serializer::SerializeObject(const meta::Variant& obj, json& output)
{
    const IJsonHandler* handler = GetHandler(obj.GetType(), obj.GetType().IsPointer());
    if (handler == nullptr)
    {
        return;
    }
    handler->SerializeImpl(obj, output);
}


std::string Serializer::SerializeToString(const meta::Variant& obj)
{
    json output;
    SerializeObject(obj, output);
    return output.dump(4);
}


void Serializer::SerializeToFile(const meta::Variant& obj, const std::string& p_path)
{
    std::ofstream o(p_path);
    json output;
    SerializeObject(obj, output);
    o << std::setw(4) << output << std::endl;
}

void Serializer::DeserializeObject(meta::Variant& obj, const json& input)
{
    const IJsonHandler* handler = GetHandler(obj.GetType(), obj.GetType().IsPointer());
    if (handler == nullptr)
    {
        return;
    }
    handler->DeserializeImpl(obj, input);
}



void Serializer::DeserializeFromString(meta::Variant& obj, const std::string& str)
{
    json input = json::parse(str);
    DeserializeObject(obj, input);
}


void Serializer::DeserializeFromFile(meta::Variant& obj, const std::string& p_path)
{
    std::ifstream file(p_path);
    json input;
    file >> input;
    DeserializeObject(obj, input);
}

const NLS::IJsonHandler* Serializer::GetHandler(const meta::Type& type, bool isPointer)
{
    NLS_ASSERT(!mHandlers.empty(), "No serialization strategy available!");

    const IJsonHandler* ret = nullptr;
    uint32_t retLvl = IJsonHandler::NoMatch;
    for (const auto handler : mHandlers)
    {
        const uint32_t tempLvl = handler->CalcMatchLevel(type, isPointer);
        if (tempLvl < retLvl)
        {
            ret = handler;
            retLvl = tempLvl;
        }
    }
    if (retLvl == IJsonHandler::NoMatch)
    {
        NLS_LOG_ERROR("No legal serializer handler found");
        return ret;
    }

    return ret;
}


} // namespace NLS
