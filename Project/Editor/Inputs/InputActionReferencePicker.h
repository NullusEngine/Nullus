#pragma once
#include <Input/InputAction.h>
#include <filesystem>

namespace NLS::Editor::Inputs
{
struct InputActionChoice
{
    std::filesystem::path assetPath;
    std::string displayName;
    std::string reference;
};
// Read-only enumeration. The UI caches this result until the picker opens again.
std::vector<InputActionChoice> BuildInputActionChoices(const std::filesystem::path& assetsRoot,
    std::vector<std::string>& diagnostics);
}
namespace NLS::Editor::Properties { void RegisterInputActionReferenceDrawer(); }
