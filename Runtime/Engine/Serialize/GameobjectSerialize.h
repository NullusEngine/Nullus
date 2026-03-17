#pragma once

#include "Serialize/IJsonHandler.h"

namespace NLS
{
class GameObjectSerializeHandler final : public IJsonHandler
{
public:
    void SerializeImpl(const meta::Variant& data, json& output) const override;
    void DeserializeImpl(meta::Variant& data, const json& input) const override;
    uint32_t CalcMatchLevel(const meta::Type& type, bool isPointer) const override;
};

class SceneSerializeHandler final : public IJsonHandler
{
public:
    void SerializeImpl(const meta::Variant& data, json& output) const override;
    void DeserializeImpl(meta::Variant& data, const json& input) const override;
    uint32_t CalcMatchLevel(const meta::Type& type, bool isPointer) const override;
};
} // namespace NLS
