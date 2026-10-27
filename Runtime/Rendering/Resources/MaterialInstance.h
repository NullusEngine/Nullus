#pragma once
#include <map>
#include "Material.h"
#include "MaterialParameterSchema.h"

namespace NLS::Render::Resources
{
NLS_RENDER_API bool ApplyMaterialParameter(Material& material,std::string_view name,
    const MaterialParameterValue& value,std::string& error);
NLS_RENDER_API bool ApplyMaterialCubeTexture(Material& material,std::string_view name,std::string_view path,std::string& error);
// A transient override layer. The source material is copied, never mutated or
// retained as a naked pointer. Rebase is called by the publication owner.
class NLS_RENDER_API MaterialInstance
{
public:
    explicit MaterialInstance(const Material& base);
    ~MaterialInstance();
    static void NotifyBaseChanged(const Material& base);
    Material& GetMaterial() const { return *m_material; }
    bool SetParameter(std::string name, MaterialParameterValue value, std::string& error);
    bool ClearParameter(std::string_view name, std::string& error);
    bool Rebase(const Material& base, std::string& error);
    const std::map<std::string,MaterialParameterValue>& GetOverrides() const { return m_overrides; }

private:
    bool Apply(std::string_view name, const MaterialParameterValue& value, std::string& error);
    std::unique_ptr<Material> m_base;
    std::unique_ptr<Material> m_material;
    std::map<std::string,MaterialParameterValue> m_overrides;
    uint64_t m_baseId = 0;
};
}
