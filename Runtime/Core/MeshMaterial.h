#pragma once
#include <string>
#include <vector>
#include <map>
#include "CoreDef.h"
using std::string;
using std::vector;
using std::map;

namespace NLS
{

namespace Rendering
{
class TextureBase;
}
class NLS_CORE_API MeshMaterialEntry
{
    friend class MeshMaterial;

public:
    bool GetEntry(const string& name, const string** output) const
    {
        auto i = entries.find(name);
        if (i == entries.end())
        {
            return false;
        }
        *output = &i->second.first;
        return true;
    }
    Rendering::TextureBase* GetEntry(const string& name) const
    {
        auto i = entries.find(name);
        if (i == entries.end())
        {
            return nullptr;
        }
        return i->second.second;
    }
    void LoadTextures();

protected:
    std::map<string, std::pair<string, Rendering::TextureBase*>> entries;
};

class MeshMaterial
{
    MeshMaterial(const std::string& filename);
    ~MeshMaterial() {}
    const MeshMaterialEntry* GetMaterialForLayer(int i) const;

    void LoadTextures();

protected:
    std::vector<MeshMaterialEntry> materialLayers;
    std::vector<MeshMaterialEntry*> meshLayers;
};

} // namespace NLS