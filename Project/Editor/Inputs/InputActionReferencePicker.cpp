#include "InputActionReferencePicker.h"
#include <Core/Inputs/InputSerialization.h>
#include <algorithm>

namespace NLS::Editor::Inputs
{
std::vector<InputActionChoice> BuildInputActionChoices(const std::filesystem::path& root, std::vector<std::string>& diagnostics)
{
    std::vector<InputActionChoice> choices;
    if (root.empty()) return choices;
    std::error_code ec;
    auto iterator = std::filesystem::recursive_directory_iterator(root, std::filesystem::directory_options::skip_permission_denied, ec);
    for (const auto end = std::filesystem::recursive_directory_iterator(); !ec && iterator != end; iterator.increment(ec))
    {
        if (!iterator->is_regular_file(ec) || iterator->path().extension() != ".inputactions") continue;
        Input::InputDefinition definition;
        if (!NLS::Core::Inputs::LoadInputDefinition(iterator->path(), definition, diagnostics)) continue;
        if (!Input::CompiledInput::Compile(definition, diagnostics)) continue;
        const auto asset = iterator->path().lexically_relative(root).generic_string();
        for (const auto& action : definition.actions)
            choices.push_back({iterator->path(), asset + " / " + action.name, definition.id.ToString() + "/" + action.id.ToString()});
    }
    if (ec) diagnostics.push_back("Input Action picker: " + ec.message());
    std::sort(choices.begin(), choices.end(), [](const auto& a, const auto& b) { return a.displayName < b.displayName; });
    return choices;
}
}
