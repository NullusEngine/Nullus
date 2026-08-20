#pragma once

#include "ScriptTypes.h"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace NLS::Scripting
{
// The only on-disk representation shared by the Native runtime and managed
// generators.  JSON is human-reviewable; the binary form carries the same
// canonical JSON plus a version and SHA-256 digest for fast startup checks.
class NLS_SCRIPTING_API ScriptApiManifest final
{
public:
    static constexpr uint32_t BinaryVersion = 1;

    static std::string ToJson(const ScriptApiDatabase& database);
    static std::vector<uint8_t> ToBinary(const ScriptApiDatabase& database);
    static ScriptStatus FromJson(std::string_view json, ScriptApiDatabase& output);
    static ScriptStatus FromBinary(std::span<const uint8_t> binary, ScriptApiDatabase& output);

    static ScriptStatus Write(const ScriptApiDatabase& database, const std::filesystem::path& directory);
    static ScriptStatus Read(const std::filesystem::path& directory, ScriptApiDatabase& output);
};
}
