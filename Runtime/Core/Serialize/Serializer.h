#pragma once
#include "IJsonHandler.h"
#include "Singleton.h"
namespace NLS
{
class NLS_CORE_API Serializer : public SingletonWithInst<Serializer>
{
public:
    void SerializeObject(const meta::Variant& obj, json& output);
    std::string SerializeToString(const meta::Variant& obj);
    void SerializeToFile(const meta::Variant& obj, const std::string& p_path);

    void DeserializeObject(meta::Variant& obj, const json& input);
    void DeserializeFromString(meta::Variant& obj, const std::string& str);
    void DeserializeFromFile(meta::Variant& obj, const std::string& p_path);

    template<class Handler>
    IJsonHandler* AddHandler();

private:
    const IJsonHandler* GetHandler(const meta::Type& type, bool isPointer);

private:
    std::vector<IJsonHandler*> mHandlers;
};

template<class Handler>
IJsonHandler* Serializer::AddHandler()
{
    static_assert(std::is_base_of<IJsonHandler, Handler>(), "Handler should be derived from IJsonHandler.");
    for (auto* handler : mHandlers)
    {
        if (typeid(*handler) == typeid(Handler))
            return handler;
    }
    mHandlers.push_back(new Handler);
    return mHandlers.back();
}
} // namespace NLS
