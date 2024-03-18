
#pragma once
#include "RHI/ShaderBase.h"
#include "glad\glad.h"
#include "OGLDef.h"
namespace NLS
{
namespace Rendering
{
class OGL_API OGLShader : public ShaderBase
{
public:
    friend class OGLRenderer;
    OGLShader(const string& vertex, const string& fragment, const string& geometry = "", const string& domain = "", const string& hull = "");
    ~OGLShader();

    void ReloadShader() override;

    bool LoadSuccess() const
    {
        return programValid == GL_TRUE;
    }

    int GetProgramID() const
    {
        return programID;
    }

    static void PrintCompileLog(GLuint object);
    static void PrintLinkLog(GLuint program);

protected:
    void DeleteIDs();

    GLuint programID;
    GLuint shaderIDs[(int)ShaderStages::SHADER_MAX];
    int shaderValid[(int)ShaderStages::SHADER_MAX];
    int programValid;
};
} // namespace Rendering
} // namespace NLS