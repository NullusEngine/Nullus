#pragma once
#include <Core/CoreDef.h>
#include <Input/InputAction.h>
#include <filesystem>

namespace NLS::Core::Inputs
{
// JSON and filesystem work stays above NLS_Input. Parsing is transactional:
// malformed data never partially replaces a live definition or profile.
NLS_CORE_API bool ParseInputDefinition(std::string_view text, Input::InputDefinition& result, std::vector<std::string>& diagnostics);
NLS_CORE_API std::string SerializeInputDefinition(const Input::InputDefinition& definition);
NLS_CORE_API bool ParseInputProfile(std::string_view text, Input::InputProfile& result, std::vector<std::string>& diagnostics);
NLS_CORE_API std::string SerializeInputProfile(const Input::InputProfile& profile);
NLS_CORE_API bool LoadInputDefinition(const std::filesystem::path& path, Input::InputDefinition& result, std::vector<std::string>& diagnostics);
NLS_CORE_API bool LoadInputProfile(const std::filesystem::path& path, Input::InputProfile& result, std::vector<std::string>& diagnostics);
NLS_CORE_API bool SaveInputProfile(const std::filesystem::path& path, const Input::InputProfile& profile, std::string& error);
NLS_CORE_API bool SaveInputDefinition(const std::filesystem::path& path, const Input::InputDefinition& definition, std::string& error);
}
