#include "CommonSerialize.h"
#include "Serializer.h"

namespace NLS
{
void CommonValueHandler::SerializeImpl(const meta::Variant& obj, json& j) const
{
    const auto type = obj.GetType();
    if (!type.IsValid())
        return;

    auto serialized = type.SerializeJson(obj, true);
    j = json::parse(serialized.dump());
}

void CommonValueHandler::DeserializeImpl(meta::Variant& obj, const json& input) const
{
    const auto type = obj.GetType();
    if (!type.IsValid())
        return;

    std::string err;
    NLS::Json value = NLS::Json::parse(input.dump(), err, json11::JsonParse::STANDARD);
    if (!err.empty())
        return;
    type.DeserializeJson(obj, value);
}

uint32_t CommonValueHandler::CalcMatchLevel(const meta::Type& type, bool isPointer) const
{
    (void)isPointer;
    if (!type.IsValid())
        return IJsonHandler::NoMatch;
    return DefaultMatch;
}
} // namespace NLS
