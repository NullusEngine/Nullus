/*
Part of Newcastle University's Game Engineering source code.

Use as you see fit!

Comments and queries to: richard-gordon.davison AT ncl.ac.uk
https://research.ncl.ac.uk/game/
*/
#include "OGLMesh.h"
#include "Vector2.h"
#include "Vector3.h"
#include "Vector4.h"

using namespace NLS;
using namespace NLS::Rendering;
using namespace NLS::Maths;

OGLMesh::OGLMesh()
{
    vao = 0;
    subCount = 1;

    for (int i = 0; i < ToUType(VertexAttribute::MAX_ATTRIBUTES); ++i)
    {
        attributeBuffers[i] = 0;
    }
    indexBuffer = 0;
}

OGLMesh::OGLMesh(const std::string& filename)
    : MeshGeometry(filename)
{
    vao = 0;
    subCount = 1;

    for (int i = 0; i < ToUType(VertexAttribute::MAX_ATTRIBUTES); ++i)
    {
        attributeBuffers[i] = 0;
    }
    indexBuffer = 0;
}

OGLMesh::~OGLMesh()
{
    glDeleteVertexArrays(1, &vao);                                      // Delete our VAO
    glDeleteBuffers(ToUType(VertexAttribute::MAX_ATTRIBUTES), attributeBuffers); // Delete our VBOs
    glDeleteBuffers(1, &indexBuffer);                                   // Delete our indices
}

void CreateVertexBuffer(GLuint& buffer, int byteCount, char* data)
{
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, byteCount, data, GL_STATIC_DRAW);
}

void OGLMesh::BindVertexAttribute(VertexAttribute attribSlot, int buffer, int bindingID, int elementCount, int elementSize, int elementOffset)
{
    auto uintAttribSlot = ToUType(attribSlot);
    glEnableVertexAttribArray(uintAttribSlot);
    glVertexAttribFormat(uintAttribSlot, elementCount, GL_FLOAT, false, 0);
    glVertexAttribBinding(uintAttribSlot, bindingID);

    glBindVertexBuffer(bindingID, buffer, elementOffset, elementSize);
}

template<class T>
int GetElementCount();

template<>
int GetElementCount<Vector2>()
{
    return 2;
}
template<>
int GetElementCount<Vector3>()
{
    return 3;
}
template<>
int GetElementCount<Vector4>()
{
    return 4;
}

void OGLMesh::UploadToGPU(Rendering::RendererBase* renderer)
{
    if (!ValidateMeshData())
    {
        return;
    }
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);

    int numVertices = GetVertexCount();
    int numIndices = GetIndexCount();

    auto attributeFunc = [&]<class T>(VertexAttribute attrib, const vector<T>& data)
    {
        if (!data.empty())
        {
            CreateVertexBuffer(attributeBuffers[ToUType(attrib)], numVertices * sizeof(T), (char*)data.data());
            BindVertexAttribute(attrib, attributeBuffers[ToUType(attrib)], ToUType(attrib), GetElementCount<T>(), sizeof(T), 0);
        }
    };

    attributeFunc(VertexAttribute::Positions, GetPositionData());
    attributeFunc(VertexAttribute::Colours, GetColourData());
    attributeFunc(VertexAttribute::TextureCoords, GetTextureCoordData());
    attributeFunc(VertexAttribute::Normals, GetNormalData());
    attributeFunc(VertexAttribute::Tangents, GetTangentData());
    attributeFunc(VertexAttribute::JointWeights, GetSkinWeightData());
    attributeFunc(VertexAttribute::JointIndices, GetSkinIndexData());

    if (!GetIndexData().empty())
    { // buffer index data
        glGenBuffers(1, &attributeBuffers[ToUType(VertexAttribute::MAX_ATTRIBUTES)]);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, attributeBuffers[ToUType(VertexAttribute::MAX_ATTRIBUTES)]);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, numIndices * sizeof(GLuint), (int*)GetIndexData().data(), GL_STATIC_DRAW);
    }

    glBindVertexArray(0);
}

void OGLMesh::UpdateGPUBuffers(unsigned int startVertex, unsigned int vertexCount)
{
    auto attributeFunc = [&]<class T>(VertexAttribute attrib, const vector<T>& data)
    {
        if (!data.empty())
        {
            glBindBuffer(GL_ARRAY_BUFFER, attributeBuffers[ToUType(attrib)]);
            glBufferSubData(GL_ARRAY_BUFFER, startVertex * sizeof(T), vertexCount * sizeof(T), (char*)&data[startVertex]);
        }
    };

    attributeFunc(VertexAttribute::Positions, GetPositionData());
    attributeFunc(VertexAttribute::Colours, GetColourData());
    attributeFunc(VertexAttribute::TextureCoords, GetTextureCoordData());
    attributeFunc(VertexAttribute::Normals, GetNormalData());
    attributeFunc(VertexAttribute::Tangents, GetTangentData());
    // attributeFunc(VertexAttribute::JointWeights, GetSkinWeightData());
    // attributeFunc(VertexAttribute::JointIndices, GetSkinIndexData());

    glBindBuffer(GL_ARRAY_BUFFER, 0);
}


void OGLMesh::RecalculateNormals()
{
    normals.clear();

    if (indices.size() > 0)
    {
        for (size_t i = 0; i < positions.size(); i++)
        {
            normals.emplace_back(Vector3());
        }

        for (size_t i = 0; i < indices.size(); i += 3)
        {
            Vector3& a = positions[indices[i + 0]];
            Vector3& b = positions[indices[i + 1]];
            Vector3& c = positions[indices[i + 2]];

            Vector3 normal = Vector3::Cross(b - a, c - a);
            normal.Normalise();

            normals[indices[i + 0]] += normal;
            normals[indices[i + 1]] += normal;
            normals[indices[i + 2]] += normal;
        }
        for (size_t i = 0; i < normals.size(); ++i)
        {
            normals[i].Normalise();
        }
    }
    else
    {
    }
}