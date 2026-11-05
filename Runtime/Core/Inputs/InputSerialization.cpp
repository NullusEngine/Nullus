#include "InputSerialization.h"
#include <Json/json.hpp>
#include <fstream>
#include <sstream>
#include <cmath>
#include <limits>
#include <unordered_set>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace NLS::Core::Inputs
{
using namespace NLS::Input;
using Json = nlohmann::json;
namespace
{
template<class T> bool Read(const Json& value, T& result);
template<class T> Json Write(const T& value);
template<class T> bool Field(const Json& object, const char* key, T& result)
{
    if (!object.is_object()) return false;
    auto it = object.find(key);
    return it == object.end() || Read(*it, result);
}
template<class T> bool Read(const Json& value, T& result)
{
    if constexpr (std::is_enum_v<T>)
    {
        if (!value.is_number_integer()) return false;
        const auto number = value.get<int64_t>();
        if (number < 0 || number > 255) return false;
        result = static_cast<T>(number); return true;
    }
    else if constexpr (std::is_same_v<T, bool>)
    { if (!value.is_boolean()) return false; result = value.get<bool>(); return true; }
    else if constexpr (std::is_arithmetic_v<T>)
    {
        if (!value.is_number()) return false;
        const double number = value.get<double>();
        if (!std::isfinite(number) || number < static_cast<double>((std::numeric_limits<T>::lowest)()) || number > static_cast<double>((std::numeric_limits<T>::max)())) return false;
        if constexpr (std::is_integral_v<T>) if (!value.is_number_integer()) return false;
        result = static_cast<T>(number); return true;
    }
    else if constexpr (std::is_same_v<T, std::string>)
    { if (!value.is_string()) return false; result = value.get<std::string>(); return true; }
    else if constexpr (std::is_same_v<T, Guid>)
    {
        if (!value.is_string()) return false;
        auto parsed = Guid::TryParse(value.get<std::string>());
        if (!parsed || !parsed->IsValid()) return false;
        result = *parsed; return true;
    }
    else static_assert(sizeof(T) == 0, "Input JSON type needs an explicit reader");
}
template<class T> Json Write(const T& value)
{
    if constexpr (std::is_enum_v<T>) return static_cast<uint8_t>(value);
    else if constexpr (std::is_same_v<T, Guid>) return value.ToString();
    else return Json(value);
}
template<class T> bool Read(const Json& value, std::vector<T>& result)
{
    if (!value.is_array() || value.size() > 65536) return false;
    result.clear(); result.reserve(value.size());
    for (const auto& element : value) { T entry{}; if (!Read(element, entry)) return false; result.push_back(std::move(entry)); }
    return true;
}
template<class T> Json Write(const std::vector<T>& value)
{
    Json array = Json::array();
    for (const auto& entry : value) array.push_back(Write(entry));
    return array;
}
// Declare specializations before vector readers instantiate dependent calls.
template<> bool Read(const Json&, InputValue&);
template<> bool Read(const Json&, ControlPath&);
template<> bool Read(const Json&, ControlPart&);
template<> bool Read(const Json&, Processor&);
template<> bool Read(const Json&, Interaction&);
template<> bool Read(const Json&, Chord&);
template<> bool Read(const Json&, ActionDefinition&);
template<> bool Read(const Json&, BindingDefinition&);
template<> bool Read(const Json&, ContextDefinition&);
template<> bool Read(const Json&, BindingOverride&);
template<> Json Write(const InputValue&);
template<> Json Write(const ControlPath&);
template<> Json Write(const ControlPart&);
template<> Json Write(const Processor&);
template<> Json Write(const Interaction&);
template<> Json Write(const Chord&);
template<> Json Write(const ActionDefinition&);
template<> Json Write(const BindingDefinition&);
template<> Json Write(const ContextDefinition&);
template<> Json Write(const BindingOverride&);

// Field uses the vector overload as well (declared after its forward declaration).
template<class T> bool ArrayField(const Json& object, const char* key, std::vector<T>& result)
{
    if (!object.is_object()) return false;
    const auto it = object.find(key); return it == object.end() || Read(*it, result);
}
template<> bool Read(const Json& j, InputValue& v)
{
    return j.is_array() && j.size() == 3 && Read(j[0], v.vector.x) && Read(j[1], v.vector.y) && Read(j[2], v.vector.z);
}
template<> Json Write(const InputValue& v) { return Json::array({v.vector.x, v.vector.y, v.vector.z}); }
template<> bool Read(const Json& j, ControlPath& v)
{
    return j.is_object() && Field(j, "device", v.device) && Field(j, "kind", v.kind) && Field(j, "control", v.control) && Field(j, "code", v.code) &&
        static_cast<int>(v.device) <= 2 && static_cast<int>(v.kind) <= 6 &&
        (v.kind != ControlKind::Button || v.control < 512) && (v.kind != ControlKind::Axis || v.control < 16);
}
template<> Json Write(const ControlPath& v)
{
    // Device handles are session-local. Profiles persist a device type/control,
    // then InputUser pairing selects the player's actual physical device.
    return {{"device", Write(v.device)}, {"kind", Write(v.kind)}, {"control", v.control}, {"code", v.code}};
}
template<> bool Read(const Json& j, ControlPart& v) { return j.is_object() && j.contains("path") && Read(j["path"], v.path) && Field(j, "contribution", v.contribution); }
template<> Json Write(const ControlPart& v) { return {{"path", Write(v.path)}, {"contribution", Write(v.contribution)}}; }
template<> bool Read(const Json& j, Processor& v)
{
    InputValue scale(1, 1, 1);
    if (!j.is_object() || !Field(j, "kind", v.kind) || static_cast<int>(v.kind) > 4 || !Field(j, "scale", scale) || !Field(j, "minimum", v.minimum) || !Field(j, "maximum", v.maximum)) return false;
    v.scale = scale.vector;
    return v.minimum <= v.maximum;
}
template<> Json Write(const Processor& v) { return {{"kind", Write(v.kind)}, {"scale", Write(InputValue(v.scale.x, v.scale.y, v.scale.z))}, {"minimum", v.minimum}, {"maximum", v.maximum}}; }
template<> bool Read(const Json& j, Interaction& v)
{ return j.is_object() && Field(j, "kind", v.kind) && static_cast<int>(v.kind) <= 5 && Field(j, "duration", v.duration) && Field(j, "interval", v.interval) && Field(j, "count", v.count) && v.duration > 0 && v.interval > 0 && v.count > 0; }
template<> Json Write(const Interaction& v) { return {{"kind", Write(v.kind)}, {"duration", v.duration}, {"interval", v.interval}, {"count", v.count}}; }
template<> bool Read(const Json& j, Chord& v)
{ return j.is_object() && ArrayField(j, "controls", v.controls) && Field(j, "modifiers", v.modifiers) && static_cast<int>(v.modifiers) < 32 && Field(j, "forbiddenModifiers", v.forbiddenModifiers) && static_cast<int>(v.forbiddenModifiers) < 32 && Field(j, "match", v.modifierMatch) && static_cast<int>(v.modifierMatch) <= 1 && Field(j, "matchModifiers", v.matchModifiers); }
template<> Json Write(const Chord& v) { return {{"controls", Write(v.controls)}, {"modifiers", Write(v.modifiers)}, {"forbiddenModifiers", Write(v.forbiddenModifiers)}, {"match", Write(v.modifierMatch)}, {"matchModifiers", v.matchModifiers}}; }
template<> bool Read(const Json& j, ActionDefinition& v)
{
    return j.is_object() && j.contains("id") && Read(j["id"], v.id) && Field(j, "name", v.name) && Field(j, "type", v.type) && static_cast<int>(v.type) <= 3 &&
        Field(j, "nature", v.nature) && static_cast<int>(v.nature) <= 2 && Field(j, "aggregation", v.aggregation) && static_cast<int>(v.aggregation) <= 1 &&
        Field(j, "pressThreshold", v.pressThreshold) && Field(j, "releaseThreshold", v.releaseThreshold) && ArrayField(j, "processors", v.processors) && Field(j, "chord", v.chord) && Field(j, "interaction", v.interaction);
}
template<> Json Write(const ActionDefinition& v)
{
    return {{"id", Write(v.id)}, {"name", v.name}, {"type", Write(v.type)}, {"nature", Write(v.nature)}, {"aggregation", Write(v.aggregation)},
        {"pressThreshold", v.pressThreshold}, {"releaseThreshold", v.releaseThreshold}, {"processors", Write(v.processors)}, {"chord", Write(v.chord)}, {"interaction", Write(v.interaction)}};
}
template<> bool Read(const Json& j, BindingDefinition& v)
{
    return j.is_object() && j.contains("id") && j.contains("action") && Read(j["id"], v.id) && Read(j["action"], v.action) &&
        Field(j, "composite", v.composite) && static_cast<int>(v.composite) <= 3 && Field(j, "nature", v.nature) && static_cast<int>(v.nature) <= 2 &&
        ArrayField(j, "parts", v.parts) && ArrayField(j, "processors", v.processors) && Field(j, "chord", v.chord) && Field(j, "hasInteraction", v.hasInteraction) && Field(j, "interaction", v.interaction);
}
template<> Json Write(const BindingDefinition& v)
{
    return {{"id", Write(v.id)}, {"action", Write(v.action)}, {"composite", Write(v.composite)}, {"nature", Write(v.nature)}, {"parts", Write(v.parts)},
        {"processors", Write(v.processors)}, {"chord", Write(v.chord)}, {"hasInteraction", v.hasInteraction}, {"interaction", Write(v.interaction)}};
}
template<> bool Read(const Json& j, ContextDefinition& v)
{
    return j.is_object() && j.contains("id") && Read(j["id"], v.id) && Field(j, "name", v.name) && Field(j, "priority", v.priority) &&
        Field(j, "consumption", v.consumption) && static_cast<int>(v.consumption) <= 3 && Field(j, "reacquire", v.reacquire) && static_cast<int>(v.reacquire) <= 1 && ArrayField(j, "bindings", v.bindings);
}
template<> Json Write(const ContextDefinition& v) { return {{"id", Write(v.id)}, {"name", v.name}, {"priority", v.priority}, {"consumption", Write(v.consumption)}, {"reacquire", Write(v.reacquire)}, {"bindings", Write(v.bindings)}}; }
template<> bool Read(const Json& j, BindingOverride& v)
{
    if (!j.is_object() || !j.contains("binding") || !Read(j["binding"], v.binding) || !Field(j, "state", v.state) || static_cast<int>(v.state) > 2 || !ArrayField(j, "parts", v.parts)) return false;
    if (j.contains("chord")) { Chord chord; if (!Read(j["chord"], chord)) return false; v.chord = chord; }
    return true;
}
template<> Json Write(const BindingOverride& v)
{
    Json result = {{"binding", Write(v.binding)}, {"state", Write(v.state)}, {"parts", Write(v.parts)}};
    if (v.chord) result["chord"] = Write(*v.chord);
    return result;
}
bool ReadFile(const std::filesystem::path& path, std::string& content, std::vector<std::string>& diagnostics)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) { diagnostics.push_back("Cannot open input resource: " + path.generic_string()); return false; }
    std::ostringstream buffer; buffer << stream.rdbuf(); content = buffer.str();
    return !stream.bad();
}
bool AtomicWrite(const std::filesystem::path& path, const std::string& text, std::string& error)
{
    std::error_code ec;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) { error = ec.message(); return false; }
    auto temporary = path; temporary += ".tmp-" + Guid::New().ToString();
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream.write(text.data(), static_cast<std::streamsize>(text.size())); stream.flush();
        if (!stream) { error = "Cannot write input resource"; stream.close(); std::filesystem::remove(temporary, ec); return false; }
    }
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) ec = std::error_code(GetLastError(), std::system_category());
#else
    std::filesystem::rename(temporary, path, ec);
#endif
    if (ec) { error = ec.message(); std::filesystem::remove(temporary, ec); return false; }
    return true;
}
}
bool ParseInputDefinition(std::string_view text, InputDefinition& result, std::vector<std::string>& diagnostics)
{
    auto root = Json::parse(text.begin(), text.end(), nullptr, false);
    InputDefinition value;
    if (root.is_discarded() || !root.is_object() || !root.contains("version") || !root.contains("id") || !Read(root["version"], value.version) || value.version != 1 || !Read(root["id"], value.id) ||
        !ArrayField(root, "actions", value.actions) || !ArrayField(root, "contexts", value.contexts) || !Field(root, "defaultContext", value.defaultContext))
    { diagnostics.emplace_back("Invalid .inputactions data or unsupported version"); return false; }
    if (!CompiledInput::Compile(value, diagnostics)) return false;
    result = std::move(value); return true;
}
std::string SerializeInputDefinition(const InputDefinition& value)
{
    Json result = {{"version", value.version}, {"id", Write(value.id)}, {"actions", Write(value.actions)}, {"contexts", Write(value.contexts)}};
    if (value.defaultContext.IsValid()) result["defaultContext"] = Write(value.defaultContext);
    return result.dump(2);
}
bool ParseInputProfile(std::string_view text, InputProfile& result, std::vector<std::string>& diagnostics)
{
    auto root = Json::parse(text.begin(), text.end(), nullptr, false);
    InputProfile value;
    if (root.is_discarded() || !root.is_object() || !root.contains("version") || !Read(root["version"], value.version) || value.version != 1 ||
        !ArrayField(root, "overrides", value.overrides) || !ArrayField(root, "unresolved", value.unresolved) || !Field(root, "pointerSensitivity", value.pointerSensitivity) || !Field(root, "gamepadSensitivity", value.gamepadSensitivity) || value.pointerSensitivity <= 0 || value.gamepadSensitivity <= 0)
    { diagnostics.emplace_back("Invalid .inputprofile data or unsupported version"); return false; }
    std::unordered_set<BindingId> ids;
    for (const auto& entry : value.overrides) if (!ids.insert(entry.binding).second) { diagnostics.emplace_back("Duplicate profile BindingId"); return false; }
    result = std::move(value); return true;
}
std::string SerializeInputProfile(const InputProfile& value)
{ return Json({{"version", value.version}, {"pointerSensitivity", value.pointerSensitivity}, {"gamepadSensitivity", value.gamepadSensitivity}, {"overrides", Write(value.overrides)}, {"unresolved", Write(value.unresolved)}}).dump(2); }
bool LoadInputDefinition(const std::filesystem::path& path, InputDefinition& result, std::vector<std::string>& diagnostics)
{ std::string content; return ReadFile(path, content, diagnostics) && ParseInputDefinition(content, result, diagnostics); }
bool LoadInputProfile(const std::filesystem::path& path, InputProfile& result, std::vector<std::string>& diagnostics)
{ std::string content; return ReadFile(path, content, diagnostics) && ParseInputProfile(content, result, diagnostics); }
bool SaveInputProfile(const std::filesystem::path& path, const InputProfile& profile, std::string& error)
{ return AtomicWrite(path, SerializeInputProfile(profile), error); }
bool SaveInputDefinition(const std::filesystem::path& path, const InputDefinition& definition, std::string& error)
{ return AtomicWrite(path, SerializeInputDefinition(definition), error); }
}
