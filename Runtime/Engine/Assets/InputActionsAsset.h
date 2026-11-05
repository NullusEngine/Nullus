#pragma once
#include <filesystem>
#include "EngineDef.h"
#include <Input/InputUser.h>
#include <Reflection/Object.h>
#include <Reflection/Macros.h>
#include <Serialize/PPtr.h>
#include "Assets/InputActionsAsset.generated.h"

namespace NLS::Engine::Assets
{
CLASS(NLS_ENGINE_API InputActionsAsset) : public NLS::Object
{
public:
    GENERATED_BODY()
    bool Reload(const std::filesystem::path& path, std::string contentHash, std::vector<std::string>& diagnostics);
    const std::shared_ptr<const Input::CompiledInput>& GetDefinition() const { return m_definition; }
    const std::string& GetContentHash() const { return m_contentHash; }
    // Publish reloads on the host input boundary. Users keep independent state,
    // stable-ID subscriptions and valid profile overrides.
    void ApplyTo(Input::InputUser& user) const;
private:
    std::shared_ptr<const Input::CompiledInput> m_definition;
    std::string m_contentHash;
};
NLS_ENGINE_API std::shared_ptr<InputActionsAsset> LoadInputActionsAsset(const Serialize::ObjectIdentifier& identifier, std::string& error);
}
