#pragma once
#include <string>
#include "RenderDef.h"

using std::string;
namespace NLS
{
namespace Rendering
{
enum class NLS_RENDER_API ShaderStages
{
    SHADER_VERTEX,
    SHADER_FRAGMENT,
    SHADER_GEOMETRY,
    SHADER_DOMAIN,
    SHADER_HULL,
    SHADER_MAX
};

class NLS_RENDER_API ShaderBase
{
public:
    ShaderBase()
    {
    }
    ShaderBase(const string& vertex, const string& fragment, const string& geometry = "", const string& domain = "", const string& hull = "");
    virtual ~ShaderBase();

    virtual void ReloadShader() = 0;

protected:
    string shaderFiles[(int)ShaderStages::SHADER_MAX];
};
} // namespace Rendering
} // namespace NLS
