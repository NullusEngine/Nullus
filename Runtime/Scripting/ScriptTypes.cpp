#include "ScriptTypes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <type_traits>

namespace NLS::Scripting
{
namespace
{
uint64_t Fnv1a(std::string_view value)
{
    uint64_t hash = 14695981039346656037ull;
    for (const auto character : value)
    {
        hash ^= static_cast<uint8_t>(character);
        hash *= 1099511628211ull;
    }
    return hash == 0 ? 1 : hash;
}

constexpr std::array<uint32_t, 64> kSha256RoundConstants = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

constexpr uint32_t RotRight(uint32_t value, uint32_t count)
{
    return (value >> count) | (value << (32u - count));
}

std::array<uint8_t, 32> Sha256(std::string_view input)
{
    std::vector<uint8_t> message(input.begin(), input.end());
    const auto bitLength = static_cast<uint64_t>(message.size()) * 8ull;
    message.push_back(0x80u);
    while ((message.size() % 64u) != 56u)
        message.push_back(0);
    for (int shift = 56; shift >= 0; shift -= 8)
        message.push_back(static_cast<uint8_t>((bitLength >> shift) & 0xffu));

    std::array<uint32_t, 8> state = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

    for (size_t offset = 0; offset < message.size(); offset += 64)
    {
        std::array<uint32_t, 64> words{};
        for (size_t index = 0; index < 16; ++index)
        {
            const size_t base = offset + index * 4;
            words[index] = (static_cast<uint32_t>(message[base]) << 24u)
                | (static_cast<uint32_t>(message[base + 1]) << 16u)
                | (static_cast<uint32_t>(message[base + 2]) << 8u)
                | static_cast<uint32_t>(message[base + 3]);
        }
        for (size_t index = 16; index < words.size(); ++index)
        {
            const auto s0 = RotRight(words[index - 15], 7) ^ RotRight(words[index - 15], 18) ^ (words[index - 15] >> 3);
            const auto s1 = RotRight(words[index - 2], 17) ^ RotRight(words[index - 2], 19) ^ (words[index - 2] >> 10);
            words[index] = words[index - 16] + s0 + words[index - 7] + s1;
        }

        auto working = state;
        for (size_t index = 0; index < words.size(); ++index)
        {
            const auto s1 = RotRight(working[4], 6) ^ RotRight(working[4], 11) ^ RotRight(working[4], 25);
            const auto choose = (working[4] & working[5]) ^ ((~working[4]) & working[6]);
            const auto temporary1 = working[7] + s1 + choose + kSha256RoundConstants[index] + words[index];
            const auto s0 = RotRight(working[0], 2) ^ RotRight(working[0], 13) ^ RotRight(working[0], 22);
            const auto majority = (working[0] & working[1]) ^ (working[0] & working[2]) ^ (working[1] & working[2]);
            const auto temporary2 = s0 + majority;
            working[7] = working[6];
            working[6] = working[5];
            working[5] = working[4];
            working[4] = working[3] + temporary1;
            working[3] = working[2];
            working[2] = working[1];
            working[1] = working[0];
            working[0] = temporary1 + temporary2;
        }
        for (size_t index = 0; index < state.size(); ++index)
            state[index] += working[index];
    }

    std::array<uint8_t, 32> digest{};
    for (size_t index = 0; index < state.size(); ++index)
        for (size_t byte = 0; byte < 4; ++byte)
            digest[index * 4 + byte] = static_cast<uint8_t>((state[index] >> (24 - byte * 8)) & 0xffu);
    return digest;
}

void Append(std::string& output, std::string_view value)
{
    output += std::to_string(value.size());
    output += ':';
    output += value;
    output += ';';
}

void AppendType(std::string& output, const ScriptType& type)
{
    Append(output, type.name);
    Append(output, std::to_string(type.id));
    Append(output, std::to_string(static_cast<uint32_t>(type.kind)));
    Append(output, std::to_string(type.size));
    Append(output, std::to_string(type.alignment));
    Append(output, type.portable ? "1" : "0");
}

std::string CanonicalValue(const ScriptValue& value)
{
    return std::visit([](const auto& item)
    {
        using Value = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<Value, std::monostate>) return std::string("null");
        if constexpr (std::is_same_v<Value, bool>) return item ? std::string("true") : std::string("false");
        if constexpr (std::is_same_v<Value, std::string>) return item;
        if constexpr (std::is_arithmetic_v<Value>) return std::to_string(item);
        if constexpr (std::is_same_v<Value, ScriptEnumValue>) return std::to_string(item.typeId) + ":" + std::to_string(item.value);
        if constexpr (std::is_same_v<Value, NativeObjectHandle>) return std::to_string(item.value);
        if constexpr (std::is_same_v<Value, ScriptObjectReference>) return item.guid + ":" + std::to_string(item.localIdentifierInFile) + ":" + std::to_string(item.fileType) + ":" + item.filePath;
        if constexpr (std::is_same_v<Value, ScriptStructValue>)
        {
            std::string result = std::to_string(item.typeId) + ":";
            for (const auto byte : item.bytes)
                result += std::to_string(byte) + ",";
            return result;
        }
        if constexpr (std::is_same_v<Value, NLS::Maths::Vector2>) return std::to_string(item.x) + "," + std::to_string(item.y);
        if constexpr (std::is_same_v<Value, NLS::Maths::Vector3>) return std::to_string(item.x) + "," + std::to_string(item.y) + "," + std::to_string(item.z);
        if constexpr (std::is_same_v<Value, NLS::Maths::Vector4>) return std::to_string(item.x) + "," + std::to_string(item.y) + "," + std::to_string(item.z) + "," + std::to_string(item.w);
        if constexpr (std::is_same_v<Value, NLS::Maths::Quaternion>) return std::to_string(item.x) + "," + std::to_string(item.y) + "," + std::to_string(item.z) + "," + std::to_string(item.w);
        if constexpr (std::is_same_v<Value, NLS::Maths::Color>) return std::to_string(item.r) + "," + std::to_string(item.g) + "," + std::to_string(item.b) + "," + std::to_string(item.a);
        return std::string{};
    }, value);
}

bool NormalizeType(ScriptType& type)
{
    if (type.kind == ScriptValueKind::Null)
    {
        type.id = 0;
        return true;
    }
    if (type.name.empty())
        return false;
    if (!type.portable)
        return false;
    if (type.id == 0)
        type.id = MakeStableScriptId(type.name);
    return type.id != 0;
}

std::string CanonicalSchema(const std::vector<ScriptTypeDescriptor>& types)
{
    std::vector<const ScriptTypeDescriptor*> sorted;
    sorted.reserve(types.size());
    for (const auto& type : types)
        sorted.push_back(&type);
    std::sort(sorted.begin(), sorted.end(), [](auto left, auto right) { return left->name < right->name; });

    std::string canonical;
    for (const auto* type : sorted)
    {
        Append(canonical, type->name);
        Append(canonical, std::to_string(type->id));
        std::vector<const ScriptMethodDescriptor*> methods;
        methods.reserve(type->methods.size());
        for (const auto& method : type->methods)
            methods.push_back(&method);
        std::sort(methods.begin(), methods.end(), [](const auto* left, const auto* right)
        {
            if (left->signature != right->signature)
                return left->signature < right->signature;
            return left->id < right->id;
        });
        for (const auto* method : methods)
        {
            Append(canonical, "method");
            Append(canonical, method->name);
            Append(canonical, method->signature);
            Append(canonical, std::to_string(method->id));
            Append(canonical, std::to_string(method->callbacks));
            Append(canonical, method->scriptable ? "1" : "0");
            Append(canonical, method->isStatic ? "1" : "0");
            AppendType(canonical, method->returnType);
            for (const auto& parameter : method->parameters)
                AppendType(canonical, parameter.type);
        }
        std::vector<const ScriptPropertyDescriptor*> properties;
        properties.reserve(type->properties.size());
        for (const auto& property : type->properties)
            properties.push_back(&property);
        std::sort(properties.begin(), properties.end(), [](const auto* left, const auto* right)
        {
            if (left->name != right->name)
                return left->name < right->name;
            return left->id < right->id;
        });
        for (const auto* property : properties)
        {
            Append(canonical, "property");
            Append(canonical, property->name);
            Append(canonical, std::to_string(property->id));
            Append(canonical, std::to_string(property->getterId));
            Append(canonical, std::to_string(property->setterId));
            Append(canonical, property->readable ? "1" : "0");
            Append(canonical, property->writable ? "1" : "0");
            Append(canonical, property->scriptable ? "1" : "0");
            AppendType(canonical, property->type);
        }
        std::vector<const ScriptFieldDescriptor*> fields;
        fields.reserve(type->fields.size());
        for (const auto& field : type->fields)
            fields.push_back(&field);
        std::sort(fields.begin(), fields.end(), [](const auto* left, const auto* right)
        {
            if (left->name != right->name)
                return left->name < right->name;
            return left->id < right->id;
        });
        for (const auto* field : fields)
        {
            Append(canonical, "field");
            Append(canonical, field->name);
            Append(canonical, std::to_string(field->id));
            Append(canonical, field->serialized ? "1" : "0");
            AppendType(canonical, field->type);
            auto aliases = field->aliases;
            std::sort(aliases.begin(), aliases.end());
            for (const auto& alias : aliases)
                Append(canonical, alias);
            Append(canonical, field->defaultValue ? CanonicalValue(*field->defaultValue) : "<none>");
        }
    }
    return canonical;
}
}

bool ScriptApiDatabase::RegisterType(ScriptTypeDescriptor descriptor)
{
    if (descriptor.name.empty())
        return false;
    if (descriptor.id == 0)
        descriptor.id = MakeStableScriptId(descriptor.name);
    if (descriptor.id == 0 || m_typeIndex.contains(descriptor.id) || m_memberIds.contains(descriptor.id))
        return false;

    std::unordered_map<ScriptMemberId, std::string> pendingMemberIds;
    const auto normalizeMethodTypes = [&descriptor](ScriptMethodDescriptor& method)
    {
        if (!NormalizeType(method.returnType))
            return false;
        for (auto& parameter : method.parameters)
            if (!NormalizeType(parameter.type))
                return false;
        return true;
    };
    for (auto& method : descriptor.methods)
    {
        if (!normalizeMethodTypes(method))
            return false;
        if (method.signature.empty())
        {
            method.signature = method.name + "(";
            for (size_t index = 0; index < method.parameters.size(); ++index)
            {
                if (index != 0)
                    method.signature += ',';
                method.signature += method.parameters[index].type.name;
            }
            method.signature += ")";
        }
        if (method.id == 0)
            method.id = MakeStableScriptId(descriptor.name + "::" + method.signature);
        const auto canonical = descriptor.name + "::" + method.signature;
        if (method.id == 0
            || m_memberIds.contains(method.id)
            || m_typeIndex.contains(method.id)
            || method.id == descriptor.id
            || pendingMemberIds.contains(method.id))
            return false;
        pendingMemberIds.emplace(method.id, canonical);
    }
    for (auto& property : descriptor.properties)
    {
        if (property.name.empty())
            return false;
        if (!NormalizeType(property.type))
            return false;
        if (property.id == 0)
            property.id = MakeStableScriptId(descriptor.name + "::" + property.name + "::property");
        const auto canonical = descriptor.name + "::" + property.name + "::property";
        if (property.id == 0
            || m_memberIds.contains(property.id)
            || m_typeIndex.contains(property.id)
            || property.id == descriptor.id
            || pendingMemberIds.contains(property.id))
            return false;
        pendingMemberIds.emplace(property.id, canonical);

        const auto sameType = [](const ScriptType& left, const ScriptType& right)
        {
            return left.id == right.id
                && left.kind == right.kind
                && left.name == right.name
                && left.size == right.size
                && left.alignment == right.alignment
                && left.portable == right.portable;
        };
        const auto getter = property.getterId == 0
            ? static_cast<const ScriptMethodDescriptor*>(nullptr)
            : [&]() -> const ScriptMethodDescriptor*
            {
                const auto found = std::find_if(
                    descriptor.methods.begin(), descriptor.methods.end(),
                    [&property](const auto& method) { return method.id == property.getterId; });
                return found == descriptor.methods.end() ? nullptr : &*found;
            }();
        const auto setter = property.setterId == 0
            ? static_cast<const ScriptMethodDescriptor*>(nullptr)
            : [&]() -> const ScriptMethodDescriptor*
            {
                const auto found = std::find_if(
                    descriptor.methods.begin(), descriptor.methods.end(),
                    [&property](const auto& method) { return method.id == property.setterId; });
                return found == descriptor.methods.end() ? nullptr : &*found;
            }();
        if (property.readable != (getter != nullptr) || property.writable != (setter != nullptr))
            return false;
        if (property.getterId != 0
            && (!getter || !getter->parameters.empty() || !sameType(getter->returnType, property.type)))
            return false;
        if (property.setterId != 0
            && (!setter || setter->parameters.size() != 1
                || !sameType(setter->parameters.front().type, property.type)
                || setter->returnType.kind != ScriptValueKind::Null))
            return false;
    }
    for (auto& field : descriptor.fields)
    {
        if (!NormalizeType(field.type))
            return false;
        if (field.id == 0)
            field.id = MakeStableScriptId(descriptor.name + "::" + field.name + "::field");
        const auto canonical = descriptor.name + "::" + field.name + "::field";
        if (field.id == 0
            || m_memberIds.contains(field.id)
            || m_typeIndex.contains(field.id)
            || field.id == descriptor.id
            || pendingMemberIds.contains(field.id))
            return false;
        pendingMemberIds.emplace(field.id, canonical);
    }

    m_typeIndex.emplace(descriptor.id, m_types.size());
    for (const auto& method : descriptor.methods)
        m_memberIds.emplace(method.id, descriptor.name + "::" + method.signature);
    for (const auto& property : descriptor.properties)
        m_memberIds.emplace(property.id, descriptor.name + "::" + property.name + "::property");
    for (const auto& field : descriptor.fields)
        m_memberIds.emplace(field.id, descriptor.name + "::" + field.name + "::field");
    m_types.push_back(std::move(descriptor));
    return true;
}

const ScriptTypeDescriptor* ScriptApiDatabase::FindType(ScriptTypeId id) const
{
    const auto found = m_typeIndex.find(id);
    return found == m_typeIndex.end() ? nullptr : &m_types[found->second];
}

const ScriptTypeDescriptor* ScriptApiDatabase::FindType(const std::string& name) const
{
    const auto found = std::find_if(m_types.begin(), m_types.end(), [&name](const auto& descriptor) { return descriptor.name == name; });
    return found == m_types.end() ? nullptr : &*found;
}

uint64_t ScriptApiDatabase::GetSchemaHash() const
{
    const auto digest = GetSchemaHashBytes();
    uint64_t result = 0;
    for (size_t index = 0; index < sizeof(result); ++index)
        result = (result << 8u) | digest[index];
    return result == 0 ? 1 : result;
}

std::array<uint8_t, 32> ScriptApiDatabase::GetSchemaHashBytes() const
{
    return Sha256(CanonicalSchema(m_types));
}

std::string ScriptApiDatabase::GetSchemaHashHex() const
{
    const auto digest = GetSchemaHashBytes();
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (const auto byte : digest)
        stream << std::setw(2) << static_cast<unsigned int>(byte);
    return stream.str();
}

void ScriptApiDatabase::Clear()
{
    m_types.clear();
    m_typeIndex.clear();
    m_memberIds.clear();
}

uint64_t MakeStableScriptId(const std::string& canonicalName)
{
    return Fnv1a(canonicalName);
}

uint64_t MakeScriptContentHash(std::string_view source)
{
    const auto digest = Sha256(source);
    uint64_t result = 0;
    for (size_t index = 0; index < sizeof(result); ++index)
        result = (result << 8u) | digest[index];
    return result == 0 ? 1 : result;
}

const char* ToString(ScriptLanguage language)
{
    switch (language)
    {
    case ScriptLanguage::CSharp: return "CSharp";
    case ScriptLanguage::Lua: return "Lua";
    case ScriptLanguage::Unknown: break;
    }
    return "Unknown";
}

const char* ToString(ScriptCallback callback)
{
    switch (callback)
    {
    case ScriptCallback::Awake: return "Awake";
    case ScriptCallback::Start: return "Start";
    case ScriptCallback::OnEnable: return "OnEnable";
    case ScriptCallback::OnDisable: return "OnDisable";
    case ScriptCallback::Update: return "Update";
    case ScriptCallback::FixedUpdate: return "FixedUpdate";
    case ScriptCallback::LateUpdate: return "LateUpdate";
    case ScriptCallback::OnDestroy: return "OnDestroy";
    case ScriptCallback::Count: break;
    }
    return "Unknown";
}
}
