
#pragma once
#include "RHI/TextureBase.h"
#include "glad\glad.h"
#include "OGLDef.h"
#include <string>

namespace NLS
{
namespace Rendering
{
class OGL_API OGLTexture : public TextureBase
{
public:
    // friend class OGLRenderer;
    OGLTexture();
    OGLTexture(GLuint texToOwn);
    ~OGLTexture();

    static TextureBase* RGBATextureFromData(char* data, int width, int height, int channels);

    static TextureBase* RGBATextureFromFilename(const std::string& name);

    static TextureBase* RGBACubeMapFromFilenames(const CubeMapFileNames& names);

    GLuint GetObjectID() const
    {
        return texID;
    }

protected:
    GLuint texID;
};
} // namespace Rendering
} // namespace NLS
