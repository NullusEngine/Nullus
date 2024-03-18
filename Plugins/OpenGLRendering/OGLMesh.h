
#pragma once
#include "RHI/MeshGeometry.h"
#include "glad\glad.h"
#include "OGLDef.h"
#include <string>

namespace NLS
{
namespace Rendering
{
class OGL_API OGLMesh : public MeshGeometry
{
public:
    friend class OGLRenderer;
    OGLMesh();
    OGLMesh(const std::string& filename);
    ~OGLMesh();

    void RecalculateNormals();

    void UploadToGPU(Rendering::RendererBase* renderer = nullptr) override;
    void UpdateGPUBuffers(unsigned int startVertex, unsigned int vertexCount);

protected:
    GLuint GetVAO() const { return vao; }
    void BindVertexAttribute(int attribSlot, int bufferID, int bindingID, int elementCount, int elementSize, int elementOffset);

    int subCount;

    GLuint vao;
    GLuint oglType;
    GLuint attributeBuffers[VertexAttribute::MAX_ATTRIBUTES];
    GLuint indexBuffer;
};
} // namespace Rendering
} // namespace NLS