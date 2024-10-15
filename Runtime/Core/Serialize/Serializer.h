#pragma once
#include "IJsonHandler.h"
#include "Singleton.h"
namespace NLS
{
class NLS_CORE_API Serializer : public SingletonWithInst<Serializer>
{
public:
    void SerializeObject(const ObjectView& obj, json& output);
    std::string SerializeToString(const ObjectView& obj);
    void SerializeToFile(const ObjectView& obj, const std::string& p_path);

    void DeserializeObject(const ObjectView& obj, const json& input);
    void DeserializeFromString(const ObjectView& obj, const std::string& str);
    void DeserializeFromFile(const ObjectView& obj, const std::string& p_path);

    template<class Handler>
    IJsonHandler* AddHandler();

private:
    const IJsonHandler* GetHandler(const Type& type, bool isPointer);

private:
    std::vector<IJsonHandler*> mHandlers;
};

template<class Handler>
IJsonHandler* Serializer::AddHandler()
{
    static_assert(std::is_base_of<IJsonHandler, Handler>(), "Handler should be derived from IJsonHandler.");
    mHandlers.push_back(new Handler);
    return mHandlers.back();
}
} // namespace NLS
