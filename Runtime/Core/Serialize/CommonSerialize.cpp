#include "CommonSerialize.h"
#include "Serializer.h"

namespace NLS
{
void CommonValueHandler::SerializeImpl(const ObjectView& obj, json& j) const
{
    const auto type = obj.GetType();
    if (!type.IsValid())
        return;

    auto serialized = type.SerializeJson(obj.Variant(), true);
    j = json::parse(serialized.dump());
}

void CommonValueHandler::DeserializeImpl(const ObjectView& obj, const json& input) const
{
    const auto type = obj.GetType();
    if (!type.IsValid())
        return;

    std::string err;
    NLS::Json value = NLS::Json::parse(input.dump(), err, json11::JsonParse::STANDARD);
    if (!err.empty())
        return;
    type.DeserializeJson(const_cast<NLS::meta::Variant&>(obj.Variant()), value);
}

uint32_t CommonValueHandler::CalcMatchLevel(const Type& type, bool isPointer) const
{
    (void)isPointer;
    if (!type.IsValid())
        return IJsonHandler::NoMatch;
    return DefaultMatch;
}
} // namespace NLS
