#include <gtest/gtest.h>

#include "Rendering/Resources/Parsers/FbxSdkParser.h"

#ifndef NLS_HAS_AUTODESK_FBX_SDK
#define NLS_HAS_AUTODESK_FBX_SDK 0
#endif

#if NLS_HAS_AUTODESK_FBX_SDK
#include <fbxsdk.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

namespace
{
#if NLS_HAS_AUTODESK_FBX_SDK
struct FbxSdkObjectDeleter
{
    template<typename T>
    void operator()(T* object) const
    {
        if (object)
            object->Destroy();
    }
};

template<typename T>
using FbxPtr = std::unique_ptr<T, FbxSdkObjectDeleter>;
#endif

std::string ReadTextFile(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

std::string SliceFrom(const std::string& source, const std::string& marker)
{
    const auto markerPosition = source.find(marker);
    if (markerPosition == std::string::npos)
        return {};
    return source.substr(markerPosition);
}

std::string SliceBetween(
    const std::string& source,
    const std::string& beginMarker,
    const std::string& endMarker)
{
    const auto begin = source.find(beginMarker);
    if (begin == std::string::npos)
        return {};

    const auto end = source.find(endMarker, begin + beginMarker.size());
    if (end == std::string::npos)
        return source.substr(begin);
    return source.substr(begin, end - begin);
}

std::string SliceScopedBlock(const std::string& source, const std::string& beginMarker)
{
    const auto begin = source.find(beginMarker);
    if (begin == std::string::npos)
        return {};

    const auto openingBrace = source.find('{', begin + beginMarker.size());
    if (openingBrace == std::string::npos)
        return {};

    size_t depth = 0u;
    for (auto position = openingBrace; position < source.size(); ++position)
    {
        if (source[position] == '{')
        {
            ++depth;
        }
        else if (source[position] == '}')
        {
            --depth;
            if (depth == 0u)
                return source.substr(begin, position - begin + 1u);
        }
    }

    return {};
}

void ExpectNoExternalFbxSdkLookup(const std::string& text)
{
    EXPECT_EQ(text.find("FBXSDK" "_ROOT"), std::string::npos);
    EXPECT_EQ(text.find("F:/" "FBX"), std::string::npos);
    EXPECT_EQ(text.find("Program Files/" "Autodesk"), std::string::npos);
}

void ExpectNoVersionedFbxSdkDirectory(const std::string& text)
{
    EXPECT_EQ(text.find("ThirdParty/FBX/" "2020"), std::string::npos);
    EXPECT_EQ(text.find("ThirdParty\\FBX\\" "2020"), std::string::npos);
}

float MaxAbsVertexPosition(const std::vector<NLS::Render::Resources::Parsers::ParsedMeshData>& meshes)
{
    float maxPosition = 0.0f;
    for (const auto& mesh : meshes)
    {
        for (const auto& vertex : mesh.vertices)
        {
            maxPosition = std::max(maxPosition, std::fabs(vertex.position[0]));
            maxPosition = std::max(maxPosition, std::fabs(vertex.position[1]));
            maxPosition = std::max(maxPosition, std::fabs(vertex.position[2]));
        }
    }
    return maxPosition;
}

const NLS::Render::Assets::ImportedSceneNode* FindNodeByName(
    const NLS::Render::Assets::ImportedScene& scene,
    const std::string& name)
{
    const auto found = std::find_if(
        scene.nodes.begin(),
        scene.nodes.end(),
        [&name](const NLS::Render::Assets::ImportedSceneNode& node)
        {
            return node.name == name;
        });
    return found != scene.nodes.end() ? &*found : nullptr;
}

#if NLS_HAS_AUTODESK_FBX_SDK
std::filesystem::path MakeFbxSdkImportTestRoot(const char* name)
{
    const auto root = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

bool WriteQuadPolygonFbx(const std::filesystem::path& path)
{
    FbxPtr<FbxManager> manager(FbxManager::Create());
    if (!manager)
        return false;

    FbxIOSettings* ioSettings = FbxIOSettings::Create(manager.get(), IOSROOT);
    if (!ioSettings)
        return false;
    manager->SetIOSettings(ioSettings);

    FbxPtr<FbxScene> scene(FbxScene::Create(manager.get(), "QuadPolygonScene"));
    if (!scene)
        return false;

    FbxMesh* mesh = FbxMesh::Create(scene.get(), "QuadPolygonMesh");
    mesh->InitControlPoints(4);
    FbxVector4* controlPoints = mesh->GetControlPoints();
    controlPoints[0] = FbxVector4(0.0, 0.0, 0.0);
    controlPoints[1] = FbxVector4(1.0, 0.0, 0.0);
    controlPoints[2] = FbxVector4(1.0, 1.0, 0.0);
    controlPoints[3] = FbxVector4(0.0, 1.0, 0.0);

    FbxGeometryElementNormal* normals = mesh->CreateElementNormal();
    normals->SetMappingMode(FbxGeometryElement::eByPolygonVertex);
    normals->SetReferenceMode(FbxGeometryElement::eDirect);
    for (int index = 0; index < 4; ++index)
        normals->GetDirectArray().Add(FbxVector4(0.0, 0.0, 1.0));

    FbxGeometryElementUV* uvs = mesh->CreateElementUV("UVSet");
    uvs->SetMappingMode(FbxGeometryElement::eByPolygonVertex);
    uvs->SetReferenceMode(FbxGeometryElement::eIndexToDirect);
    uvs->GetDirectArray().Add(FbxVector2(0.0, 0.0));
    uvs->GetDirectArray().Add(FbxVector2(1.0, 0.0));
    uvs->GetDirectArray().Add(FbxVector2(1.0, 1.0));
    uvs->GetDirectArray().Add(FbxVector2(0.0, 1.0));
    for (int index = 0; index < 4; ++index)
        uvs->GetIndexArray().Add(index);

    FbxSurfacePhong* material = FbxSurfacePhong::Create(scene.get(), "QuadSurface");
    material->Diffuse.Set(FbxDouble3(0.4, 0.5, 0.6));

    FbxLayer* layer = mesh->GetLayer(0);
    if (!layer)
    {
        mesh->CreateLayer();
        layer = mesh->GetLayer(0);
    }

    FbxLayerElementMaterial* materialElement = FbxLayerElementMaterial::Create(mesh, "");
    materialElement->SetMappingMode(FbxLayerElement::eAllSame);
    materialElement->SetReferenceMode(FbxLayerElement::eIndexToDirect);
    materialElement->GetIndexArray().Add(0);
    layer->SetMaterials(materialElement);

    mesh->BeginPolygon(0);
    mesh->AddPolygon(0);
    mesh->AddPolygon(1);
    mesh->AddPolygon(2);
    mesh->AddPolygon(3);
    mesh->EndPolygon();

    FbxNode* node = FbxNode::Create(scene.get(), "QuadPolygonNode");
    node->SetNodeAttribute(mesh);
    node->AddMaterial(material);
    scene->GetRootNode()->AddChild(node);

    FbxPtr<FbxExporter> exporter(FbxExporter::Create(manager.get(), ""));
    if (!exporter || !exporter->Initialize(path.string().c_str(), -1, manager->GetIOSettings()))
        return false;

    return exporter->Export(scene.get());
}

bool ExportScene(FbxManager* manager, FbxScene* scene, const std::filesystem::path& path)
{
    FbxPtr<FbxExporter> exporter(FbxExporter::Create(manager, ""));
    if (!exporter || !exporter->Initialize(path.string().c_str(), -1, manager->GetIOSettings()))
        return false;

    return exporter->Export(scene);
}

bool InitializeFbxScene(
    const char* sceneName,
    FbxPtr<FbxManager>& manager,
    FbxPtr<FbxIOSettings>& ioSettings,
    FbxPtr<FbxScene>& scene)
{
    manager.reset(FbxManager::Create());
    if (!manager)
        return false;

    ioSettings.reset(FbxIOSettings::Create(manager.get(), IOSROOT));
    if (!ioSettings)
        return false;
    manager->SetIOSettings(ioSettings.get());

    scene.reset(FbxScene::Create(manager.get(), sceneName));
    return scene != nullptr;
}

void AddPolygonVertexNormals(FbxMesh* mesh, int count)
{
    FbxGeometryElementNormal* normals = mesh->CreateElementNormal();
    normals->SetMappingMode(FbxGeometryElement::eByPolygonVertex);
    normals->SetReferenceMode(FbxGeometryElement::eDirect);
    for (int index = 0; index < count; ++index)
        normals->GetDirectArray().Add(FbxVector4(0.0, 0.0, 1.0));
}

void AddPolygonVertexUvSet(FbxMesh* mesh, const char* name, const std::vector<FbxVector2>& values)
{
    FbxGeometryElementUV* uvs = mesh->CreateElementUV(name);
    uvs->SetMappingMode(FbxGeometryElement::eByPolygonVertex);
    uvs->SetReferenceMode(FbxGeometryElement::eIndexToDirect);
    for (const auto& value : values)
        uvs->GetDirectArray().Add(value);
    for (int index = 0; index < static_cast<int>(values.size()); ++index)
        uvs->GetIndexArray().Add(index);
}

bool WriteConcaveNgonFbx(const std::filesystem::path& path)
{
    FbxPtr<FbxManager> manager;
    FbxPtr<FbxIOSettings> ioSettings;
    FbxPtr<FbxScene> scene;
    if (!InitializeFbxScene("ConcaveNgonScene", manager, ioSettings, scene))
        return false;

    FbxMesh* mesh = FbxMesh::Create(scene.get(), "ConcaveNgonMesh");
    mesh->InitControlPoints(8);
    FbxVector4* controlPoints = mesh->GetControlPoints();
    controlPoints[0] = FbxVector4(0.0, 0.0, 0.0);
    controlPoints[1] = FbxVector4(3.0, 0.0, 0.0);
    controlPoints[2] = FbxVector4(3.0, 1.0, 0.0);
    controlPoints[3] = FbxVector4(1.0, 1.0, 0.0);
    controlPoints[4] = FbxVector4(1.0, 2.0, 0.0);
    controlPoints[5] = FbxVector4(3.0, 2.0, 0.0);
    controlPoints[6] = FbxVector4(3.0, 3.0, 0.0);
    controlPoints[7] = FbxVector4(0.0, 3.0, 0.0);

    AddPolygonVertexNormals(mesh, 8);

    FbxSurfacePhong* material = FbxSurfacePhong::Create(scene.get(), "ConcaveSurface");
    FbxNode* node = FbxNode::Create(scene.get(), "ConcaveNgonNode");
    node->SetNodeAttribute(mesh);
    node->AddMaterial(material);
    scene->GetRootNode()->AddChild(node);

    mesh->BeginPolygon(0);
    for (int index = 0; index < 8; ++index)
        mesh->AddPolygon(index);
    mesh->EndPolygon();

    return ExportScene(manager.get(), scene.get(), path);
}

bool WriteMultiMaterialPolygonFbx(const std::filesystem::path& path)
{
    FbxPtr<FbxManager> manager;
    FbxPtr<FbxIOSettings> ioSettings;
    FbxPtr<FbxScene> scene;
    if (!InitializeFbxScene("MultiMaterialScene", manager, ioSettings, scene))
        return false;

    FbxMesh* mesh = FbxMesh::Create(scene.get(), "MultiMaterialMesh");
    mesh->InitControlPoints(6);
    FbxVector4* controlPoints = mesh->GetControlPoints();
    controlPoints[0] = FbxVector4(0.0, 0.0, 0.0);
    controlPoints[1] = FbxVector4(1.0, 0.0, 0.0);
    controlPoints[2] = FbxVector4(0.0, 1.0, 0.0);
    controlPoints[3] = FbxVector4(2.0, 0.0, 0.0);
    controlPoints[4] = FbxVector4(3.0, 0.0, 0.0);
    controlPoints[5] = FbxVector4(2.0, 1.0, 0.0);
    AddPolygonVertexNormals(mesh, 6);

    FbxLayer* layer = mesh->GetLayer(0);
    if (!layer)
    {
        mesh->CreateLayer();
        layer = mesh->GetLayer(0);
    }

    FbxLayerElementMaterial* materialElement = FbxLayerElementMaterial::Create(mesh, "");
    materialElement->SetMappingMode(FbxLayerElement::eByPolygon);
    materialElement->SetReferenceMode(FbxLayerElement::eIndexToDirect);
    materialElement->GetIndexArray().Add(0);
    materialElement->GetIndexArray().Add(1);
    layer->SetMaterials(materialElement);

    FbxSurfacePhong* red = FbxSurfacePhong::Create(scene.get(), "RedSurface");
    red->Diffuse.Set(FbxDouble3(1.0, 0.0, 0.0));
    FbxSurfacePhong* blue = FbxSurfacePhong::Create(scene.get(), "BlueSurface");
    blue->Diffuse.Set(FbxDouble3(0.0, 0.0, 1.0));

    mesh->BeginPolygon(0);
    mesh->AddPolygon(0);
    mesh->AddPolygon(1);
    mesh->AddPolygon(2);
    mesh->EndPolygon();

    mesh->BeginPolygon(1);
    mesh->AddPolygon(3);
    mesh->AddPolygon(4);
    mesh->AddPolygon(5);
    mesh->EndPolygon();

    FbxNode* node = FbxNode::Create(scene.get(), "MultiMaterialNode");
    node->SetNodeAttribute(mesh);
    node->AddMaterial(red);
    node->AddMaterial(blue);
    scene->GetRootNode()->AddChild(node);

    return ExportScene(manager.get(), scene.get(), path);
}

bool WriteNodeLocalMaterialOrderFbx(const std::filesystem::path& path)
{
    FbxPtr<FbxManager> manager;
    FbxPtr<FbxIOSettings> ioSettings;
    FbxPtr<FbxScene> scene;
    if (!InitializeFbxScene("NodeLocalMaterialOrderScene", manager, ioSettings, scene))
        return false;

    FbxSurfacePhong* red = FbxSurfacePhong::Create(scene.get(), "SceneRed");
    red->Diffuse.Set(FbxDouble3(1.0, 0.0, 0.0));
    FbxSurfacePhong* blue = FbxSurfacePhong::Create(scene.get(), "SceneBlue");
    blue->Diffuse.Set(FbxDouble3(0.0, 0.0, 1.0));

    FbxMesh* mesh = FbxMesh::Create(scene.get(), "NodeLocalMaterialOrderMesh");
    mesh->InitControlPoints(6);
    FbxVector4* controlPoints = mesh->GetControlPoints();
    controlPoints[0] = FbxVector4(0.0, 0.0, 0.0);
    controlPoints[1] = FbxVector4(1.0, 0.0, 0.0);
    controlPoints[2] = FbxVector4(0.0, 1.0, 0.0);
    controlPoints[3] = FbxVector4(2.0, 0.0, 0.0);
    controlPoints[4] = FbxVector4(3.0, 0.0, 0.0);
    controlPoints[5] = FbxVector4(2.0, 1.0, 0.0);
    AddPolygonVertexNormals(mesh, 6);

    FbxLayer* layer = mesh->GetLayer(0);
    if (!layer)
    {
        mesh->CreateLayer();
        layer = mesh->GetLayer(0);
    }

    FbxLayerElementMaterial* materialElement = FbxLayerElementMaterial::Create(mesh, "");
    materialElement->SetMappingMode(FbxLayerElement::eByPolygon);
    materialElement->SetReferenceMode(FbxLayerElement::eIndexToDirect);
    materialElement->GetIndexArray().Add(0);
    materialElement->GetIndexArray().Add(1);
    layer->SetMaterials(materialElement);

    mesh->BeginPolygon(0);
    mesh->AddPolygon(0);
    mesh->AddPolygon(1);
    mesh->AddPolygon(2);
    mesh->EndPolygon();

    mesh->BeginPolygon(1);
    mesh->AddPolygon(3);
    mesh->AddPolygon(4);
    mesh->AddPolygon(5);
    mesh->EndPolygon();

    FbxNode* node = FbxNode::Create(scene.get(), "NodeLocalMaterialOrderNode");
    node->SetNodeAttribute(mesh);
    node->AddMaterial(blue);
    node->AddMaterial(red);
    scene->GetRootNode()->AddChild(node);

    return ExportScene(manager.get(), scene.get(), path);
}

bool WriteInvalidNodeMaterialSlotFbx(const std::filesystem::path& path)
{
    FbxPtr<FbxManager> manager;
    FbxPtr<FbxIOSettings> ioSettings;
    FbxPtr<FbxScene> scene;
    if (!InitializeFbxScene("InvalidNodeMaterialSlotScene", manager, ioSettings, scene))
        return false;

    FbxSurfacePhong* red = FbxSurfacePhong::Create(scene.get(), "SceneRed");
    red->Diffuse.Set(FbxDouble3(1.0, 0.0, 0.0));
    FbxSurfacePhong* blue = FbxSurfacePhong::Create(scene.get(), "SceneBlue");
    blue->Diffuse.Set(FbxDouble3(0.0, 0.0, 1.0));

    FbxMesh* mesh = FbxMesh::Create(scene.get(), "InvalidNodeMaterialSlotMesh");
    mesh->InitControlPoints(3);
    FbxVector4* controlPoints = mesh->GetControlPoints();
    controlPoints[0] = FbxVector4(0.0, 0.0, 0.0);
    controlPoints[1] = FbxVector4(1.0, 0.0, 0.0);
    controlPoints[2] = FbxVector4(0.0, 1.0, 0.0);
    AddPolygonVertexNormals(mesh, 3);

    FbxLayer* layer = mesh->GetLayer(0);
    if (!layer)
    {
        mesh->CreateLayer();
        layer = mesh->GetLayer(0);
    }

    FbxLayerElementMaterial* materialElement = FbxLayerElementMaterial::Create(mesh, "");
    materialElement->SetMappingMode(FbxLayerElement::eAllSame);
    materialElement->SetReferenceMode(FbxLayerElement::eIndexToDirect);
    materialElement->GetIndexArray().Add(1);
    layer->SetMaterials(materialElement);

    mesh->BeginPolygon(1);
    mesh->AddPolygon(0);
    mesh->AddPolygon(1);
    mesh->AddPolygon(2);
    mesh->EndPolygon();

    FbxNode* node = FbxNode::Create(scene.get(), "InvalidNodeMaterialSlotNode");
    node->SetNodeAttribute(mesh);
    node->AddMaterial(red);
    scene->GetRootNode()->AddChild(node);

    FbxNode* materialLibraryNode = FbxNode::Create(scene.get(), "MaterialLibraryNode");
    materialLibraryNode->AddMaterial(blue);
    scene->GetRootNode()->AddChild(materialLibraryNode);

    return ExportScene(manager.get(), scene.get(), path);
}

bool WriteTangentAndMultiUvFbx(const std::filesystem::path& path)
{
    FbxPtr<FbxManager> manager;
    FbxPtr<FbxIOSettings> ioSettings;
    FbxPtr<FbxScene> scene;
    if (!InitializeFbxScene("TangentScene", manager, ioSettings, scene))
        return false;

    FbxMesh* mesh = FbxMesh::Create(scene.get(), "TangentMesh");
    mesh->InitControlPoints(3);
    FbxVector4* controlPoints = mesh->GetControlPoints();
    controlPoints[0] = FbxVector4(0.0, 0.0, 0.0);
    controlPoints[1] = FbxVector4(1.0, 0.0, 0.0);
    controlPoints[2] = FbxVector4(0.0, 1.0, 0.0);
    AddPolygonVertexNormals(mesh, 3);
    AddPolygonVertexUvSet(mesh, "UVSet0", {FbxVector2(0.0, 0.0), FbxVector2(1.0, 0.0), FbxVector2(0.0, 1.0)});
    AddPolygonVertexUvSet(mesh, "UVSet1", {FbxVector2(0.25, 0.25), FbxVector2(0.75, 0.25), FbxVector2(0.25, 0.75)});

    FbxGeometryElementTangent* tangents = mesh->CreateElementTangent();
    tangents->SetMappingMode(FbxGeometryElement::eByPolygonVertex);
    tangents->SetReferenceMode(FbxGeometryElement::eIndexToDirect);
    FbxGeometryElementBinormal* binormals = mesh->CreateElementBinormal();
    binormals->SetMappingMode(FbxGeometryElement::eByPolygonVertex);
    binormals->SetReferenceMode(FbxGeometryElement::eIndexToDirect);
    for (int index = 0; index < 3; ++index)
    {
        tangents->GetDirectArray().Add(FbxVector4(1.0, 0.0, 0.0, 0.0));
        tangents->GetIndexArray().Add(index);
        binormals->GetDirectArray().Add(FbxVector4(0.0, 1.0, 0.0, 0.0));
        binormals->GetIndexArray().Add(index);
    }

    FbxSurfacePhong* material = FbxSurfacePhong::Create(scene.get(), "TangentSurface");
    FbxNode* node = FbxNode::Create(scene.get(), "TangentNode");
    node->SetNodeAttribute(mesh);
    node->AddMaterial(material);
    scene->GetRootNode()->AddChild(node);

    mesh->BeginPolygon(0);
    mesh->AddPolygon(0);
    mesh->AddPolygon(1);
    mesh->AddPolygon(2);
    mesh->EndPolygon();

    return ExportScene(manager.get(), scene.get(), path);
}

bool WriteSharedMeshFbx(const std::filesystem::path& path)
{
    FbxPtr<FbxManager> manager;
    FbxPtr<FbxIOSettings> ioSettings;
    FbxPtr<FbxScene> scene;
    if (!InitializeFbxScene("SharedMeshScene", manager, ioSettings, scene))
        return false;

    FbxMesh* mesh = FbxMesh::Create(scene.get(), "SharedMesh");
    mesh->InitControlPoints(3);
    FbxVector4* controlPoints = mesh->GetControlPoints();
    controlPoints[0] = FbxVector4(0.0, 0.0, 0.0);
    controlPoints[1] = FbxVector4(1.0, 0.0, 0.0);
    controlPoints[2] = FbxVector4(0.0, 1.0, 0.0);
    AddPolygonVertexNormals(mesh, 3);
    mesh->BeginPolygon(0);
    mesh->AddPolygon(0);
    mesh->AddPolygon(1);
    mesh->AddPolygon(2);
    mesh->EndPolygon();

    FbxSurfacePhong* material = FbxSurfacePhong::Create(scene.get(), "SharedSurface");

    FbxNode* left = FbxNode::Create(scene.get(), "SharedLeft");
    left->SetNodeAttribute(mesh);
    left->AddMaterial(material);
    scene->GetRootNode()->AddChild(left);

    FbxNode* right = FbxNode::Create(scene.get(), "SharedRight");
    right->SetNodeAttribute(mesh);
    right->LclTranslation.Set(FbxDouble3(2.0, 0.0, 0.0));
    right->AddMaterial(material);
    scene->GetRootNode()->AddChild(right);

    return ExportScene(manager.get(), scene.get(), path);
}

double PolygonArea(const std::vector<std::array<double, 2>>& polygon)
{
    double area = 0.0;
    for (size_t index = 0u; index < polygon.size(); ++index)
    {
        const auto& current = polygon[index];
        const auto& next = polygon[(index + 1u) % polygon.size()];
        area += current[0] * next[1] - next[0] * current[1];
    }
    return std::fabs(area) * 0.5;
}

double TriangleArea(
    const NLS::Render::Geometry::Vertex& a,
    const NLS::Render::Geometry::Vertex& b,
    const NLS::Render::Geometry::Vertex& c)
{
    return std::fabs(
        (static_cast<double>(a.position[0]) * (static_cast<double>(b.position[1]) - static_cast<double>(c.position[1]))) +
        (static_cast<double>(b.position[0]) * (static_cast<double>(c.position[1]) - static_cast<double>(a.position[1]))) +
        (static_cast<double>(c.position[0]) * (static_cast<double>(a.position[1]) - static_cast<double>(b.position[1])))) *
        0.5;
}

bool PointInPolygon(const std::vector<std::array<double, 2>>& polygon, const double x, const double y)
{
    bool inside = false;
    for (size_t index = 0u, previous = polygon.size() - 1u; index < polygon.size(); previous = index++)
    {
        const auto& a = polygon[index];
        const auto& b = polygon[previous];
        if (((a[1] > y) != (b[1] > y)) &&
            (x < (b[0] - a[0]) * (y - a[1]) / (b[1] - a[1]) + a[0]))
        {
            inside = !inside;
        }
    }
    return inside;
}

void ExpectPosition(
    const NLS::Render::Geometry::Vertex& vertex,
    const float x,
    const float y,
    const float z)
{
    EXPECT_FLOAT_EQ(vertex.position[0], x);
    EXPECT_FLOAT_EQ(vertex.position[1], y);
    EXPECT_FLOAT_EQ(vertex.position[2], z);
}
#endif
}

TEST(FbxSdkIntegrationContractTests, EditorFbxImportDefaultsToFbxSdkParserWithoutAutomaticAssimpFallback)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Assets/ExternalAssetImporter.cpp");

    ASSERT_FALSE(source.empty());
    EXPECT_NE(source.find("FbxSdkParser"), std::string::npos);
    EXPECT_NE(source.find("ResolveFbxReaderSelection"), std::string::npos);
    EXPECT_NE(source.find("FbxReaderSelection::Autodesk"), std::string::npos);
    EXPECT_NE(source.find("FbxReaderSelection::AutodeskWithAssimpFallback"), std::string::npos);

    const auto importScene = SliceFrom(source, "ImportSceneForRequest(");
    ASSERT_FALSE(importScene.empty());
    const auto fbxBranch = SliceBetween(importScene, "extension == \".fbx\"", "extension == \".obj\"");
    ASSERT_FALSE(fbxBranch.empty());
    EXPECT_NE(fbxBranch.find("LoadFbxWithAutodesk"), std::string::npos);
    EXPECT_NE(fbxBranch.find("FbxReaderSelection::AutodeskWithAssimpFallback"), std::string::npos);

    const auto autodeskReader = SliceBetween(source, "bool LoadFbxWithAutodesk(", "void AddFbxReaderFallbackWarning(");
    ASSERT_FALSE(autodeskReader.empty());
    EXPECT_NE(autodeskReader.find("FbxSdkParser"), std::string::npos);
}

TEST(FbxSdkIntegrationContractTests, ExternalModelImportExposesProfilerScopesForSlowImportStages)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Assets/ExternalAssetImporter.cpp");

    ASSERT_FALSE(source.empty());
    EXPECT_NE(source.find("#include \"Profiling/Profiler.h\""), std::string::npos);

    const auto importFunction = SliceBetween(
        source,
        "ExternalModelImportResult ImportExternalModelAsset(",
        "    return result;\n}\n}");
    ASSERT_FALSE(importFunction.empty());
    EXPECT_NE(importFunction.find("NLS_PROFILE_NAMED_SCOPE(\"AssetImport::ExternalModel::Total\")"), std::string::npos);
    EXPECT_NE(importFunction.find("NLS_PROFILE_NAMED_SCOPE(\"AssetImport::ExternalModel::SourceParse\")"), std::string::npos);
    EXPECT_NE(importFunction.find("NLS_PROFILE_NAMED_SCOPE(\"AssetImport::ExternalModel::GenerateSubAssets\")"), std::string::npos);
    EXPECT_NE(importFunction.find("NLS_PROFILE_NAMED_SCOPE(\"AssetImport::ExternalModel::ConvertMaterials\")"), std::string::npos);
    EXPECT_NE(importFunction.find("NLS_PROFILE_NAMED_SCOPE(\"AssetImport::ExternalModel::LoadSourceMeshes\")"), std::string::npos);
    EXPECT_NE(importFunction.find("NLS_PROFILE_NAMED_SCOPE(\"AssetImport::ExternalModel::LoadTextures\")"), std::string::npos);
    EXPECT_NE(importFunction.find("NLS_PROFILE_NAMED_SCOPE(\"AssetImport::ExternalModel::SerializeSubAssets\")"), std::string::npos);
    EXPECT_NE(importFunction.find("NLS_PROFILE_NAMED_SCOPE(\"AssetImport::ExternalModel::BuildPrefab\")"), std::string::npos);
    EXPECT_NE(importFunction.find("NLS_PROFILE_NAMED_SCOPE(\"AssetImport::ExternalModel::SerializePrefab\")"), std::string::npos);
    EXPECT_NE(importFunction.find("NLS_PROFILE_NAMED_SCOPE(\"AssetImport::ExternalModel::WriteAndCommit\")"), std::string::npos);
}

TEST(FbxSdkIntegrationContractTests, FbxSdkParserExposesProfilerScopesForSlowImportStages)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Rendering/Resources/Parsers/FbxSdkParser.cpp");

    ASSERT_FALSE(source.empty());
    EXPECT_NE(source.find("#include \"Profiling/Profiler.h\""), std::string::npos);

    const auto loadFbxScene = SliceBetween(source, "bool LoadFbxScene(", "bool LoadSceneData(");
    ASSERT_FALSE(loadFbxScene.empty());
    EXPECT_NE(loadFbxScene.find("NLS_PROFILE_NAMED_SCOPE(\"AssetImport::FbxSdk::LoadFbxScene\")"), std::string::npos);
    EXPECT_NE(loadFbxScene.find("NLS_PROFILE_NAMED_SCOPE(\"AssetImport::FbxSdk::CreateManager\")"), std::string::npos);
    EXPECT_NE(loadFbxScene.find("NLS_PROFILE_NAMED_SCOPE(\"AssetImport::FbxSdk::InitializeImporter\")"), std::string::npos);
    EXPECT_NE(loadFbxScene.find("NLS_PROFILE_NAMED_SCOPE(\"AssetImport::FbxSdk::CreateScene\")"), std::string::npos);
    EXPECT_NE(loadFbxScene.find("NLS_PROFILE_NAMED_SCOPE(\"AssetImport::FbxSdk::ImportScene\")"), std::string::npos);
    EXPECT_NE(loadFbxScene.find("\"InitializeImporter\""), std::string::npos);
    EXPECT_NE(loadFbxScene.find("\"ImportScene\""), std::string::npos);

    const auto loadSceneData = SliceBetween(source, "bool LoadSceneData(", "bool FbxSdkParser::LoadModel(");
    ASSERT_FALSE(loadSceneData.empty());
    EXPECT_NE(loadSceneData.find("NLS_PROFILE_NAMED_SCOPE(\"AssetImport::FbxSdk::LoadSceneData\")"), std::string::npos);
    EXPECT_NE(loadSceneData.find("NLS_PROFILE_NAMED_SCOPE(\"AssetImport::FbxSdk::ConvertNurbsAndPatches\")"), std::string::npos);
    EXPECT_NE(loadSceneData.find("\"LoadFbxSceneFailed\""), std::string::npos);
    EXPECT_NE(loadSceneData.find("NLS_PROFILE_NAMED_SCOPE(\"AssetImport::FbxSdk::ProcessMaterials\")"), std::string::npos);
    EXPECT_NE(loadSceneData.find("NLS_PROFILE_NAMED_SCOPE(\"AssetImport::FbxSdk::ProcessMeshes\")"), std::string::npos);

    const auto loadModelData = SliceFrom(source, "bool FbxSdkParser::LoadModelData(");
    ASSERT_FALSE(loadModelData.empty());
    EXPECT_NE(loadModelData.find("NLS_PROFILE_NAMED_SCOPE(\"AssetImport::FbxSdk::LoadModelData\")"), std::string::npos);
}

TEST(FbxSdkIntegrationContractTests, FbxSdkParserConfiguresModelOnlyImportOptions)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Rendering/Resources/Parsers/FbxSdkParser.cpp");

    ASSERT_FALSE(source.empty());

    const auto configureImportSettings = SliceBetween(
        source,
        "void ConfigureModelOnlyFbxImport(",
        "bool LoadFbxScene(");
    ASSERT_FALSE(configureImportSettings.empty());

    EXPECT_NE(configureImportSettings.find("SetBoolProp(IMP_FBX_MODEL, true)"), std::string::npos);
    EXPECT_NE(configureImportSettings.find("SetBoolProp(IMP_FBX_MATERIAL, true)"), std::string::npos);
    EXPECT_NE(configureImportSettings.find("SetBoolProp(IMP_FBX_TEXTURE, true)"), std::string::npos);
    EXPECT_NE(configureImportSettings.find("SetBoolProp(IMP_FBX_GLOBAL_SETTINGS, true)"), std::string::npos);
    EXPECT_NE(configureImportSettings.find("SetBoolProp(IMP_FBX_NORMAL, true)"), std::string::npos);
    EXPECT_NE(configureImportSettings.find("SetBoolProp(IMP_FBX_TANGENT, true)"), std::string::npos);
    EXPECT_NE(configureImportSettings.find("SetBoolProp(IMP_FBX_BINORMAL, true)"), std::string::npos);
    EXPECT_NE(configureImportSettings.find("SetBoolProp(IMP_FBX_ANIMATION, false)"), std::string::npos);
    EXPECT_NE(configureImportSettings.find("SetBoolProp(IMP_FBX_CHARACTER, false)"), std::string::npos);
    EXPECT_NE(configureImportSettings.find("SetBoolProp(IMP_FBX_CONSTRAINT, false)"), std::string::npos);
    EXPECT_NE(configureImportSettings.find("SetBoolProp(IMP_FBX_AUDIO, false)"), std::string::npos);
    EXPECT_NE(configureImportSettings.find("SetBoolProp(IMP_FBX_EXTRACT_EMBEDDED_DATA, false)"), std::string::npos);
    EXPECT_NE(configureImportSettings.find("SetBoolProp(IMP_FBX_SHAPE, false)"), std::string::npos);
    EXPECT_NE(configureImportSettings.find("SetBoolProp(IMP_FBX_GOBO, false)"), std::string::npos);

    const auto loadFbxScene = SliceBetween(source, "bool LoadFbxScene(", "bool LoadSceneData(");
    ASSERT_FALSE(loadFbxScene.empty());
    EXPECT_NE(loadFbxScene.find("ConfigureModelOnlyFbxImport(*ioSettings)"), std::string::npos);
}

TEST(FbxSdkIntegrationContractTests, FbxSdkParserUsesUnityStylePolygonTriangulationBoundary)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Rendering/Resources/Parsers/FbxSdkParser.cpp");

    ASSERT_FALSE(source.empty());

    const auto loadSceneData = SliceBetween(source, "bool LoadSceneData(", "bool FbxSdkParser::LoadModel(");
    ASSERT_FALSE(loadSceneData.empty());
    EXPECT_EQ(loadSceneData.find("Triangulate(scene.get(), true)"), std::string::npos);
    EXPECT_NE(loadSceneData.find("ConvertNurbsAndPatches"), std::string::npos);

    const auto processMesh = SliceBetween(source, "void ProcessMesh(", "void BuildMeshRecord(");
    ASSERT_FALSE(processMesh.empty());
    EXPECT_EQ(processMesh.find("polygonSize != 3"), std::string::npos);
    EXPECT_NE(processMesh.find("polygonSize < 3"), std::string::npos);
    EXPECT_NE(source.find("polygonSize - 2"), std::string::npos);
}

#if NLS_HAS_AUTODESK_FBX_SDK
TEST(FbxSdkIntegrationContractTests, FbxSdkParserTriangulatesQuadPolygonsWithoutFbxSceneTriangulate)
{
    const auto root = MakeFbxSdkImportTestRoot("NullusFbxSdkQuadPolygonTests");
    const auto sourcePath = root / "QuadPolygon.fbx";
    ASSERT_TRUE(WriteQuadPolygonFbx(sourcePath));

    NLS::Render::Resources::Parsers::FbxSdkParser parser;
    std::vector<NLS::Render::Resources::Parsers::ParsedMeshData> meshes;
    std::vector<std::string> materials;
    ASSERT_TRUE(parser.LoadModelData(
        sourcePath.string(),
        meshes,
        materials,
        NLS::Render::Resources::Parsers::EModelParserFlags::NONE,
        nullptr,
        true));

    ASSERT_EQ(meshes.size(), 1u);
    ASSERT_EQ(meshes[0].vertices.size(), 6u);
    ASSERT_EQ(meshes[0].indices.size(), 6u);
    for (uint32_t index = 0u; index < 6u; ++index)
        EXPECT_EQ(meshes[0].indices[index], index);

    ExpectPosition(meshes[0].vertices[0], 0.0f, 0.0f, 0.0f);
    ExpectPosition(meshes[0].vertices[1], 1.0f, 0.0f, 0.0f);
    ExpectPosition(meshes[0].vertices[2], 1.0f, 1.0f, 0.0f);
    ExpectPosition(meshes[0].vertices[3], 0.0f, 0.0f, 0.0f);
    ExpectPosition(meshes[0].vertices[4], 1.0f, 1.0f, 0.0f);
    ExpectPosition(meshes[0].vertices[5], 0.0f, 1.0f, 0.0f);

    std::filesystem::remove_all(root);
}

TEST(FbxSdkIntegrationContractTests, FbxSdkParserTriangulatesConcaveNgonsWithoutFanOverlap)
{
    const auto root = MakeFbxSdkImportTestRoot("NullusFbxSdkConcaveNgonTests");
    const auto sourcePath = root / "ConcaveNgon.fbx";
    ASSERT_TRUE(WriteConcaveNgonFbx(sourcePath));

    NLS::Render::Resources::Parsers::FbxSdkParser parser;
    std::vector<NLS::Render::Resources::Parsers::ParsedMeshData> meshes;
    std::vector<std::string> materials;
    ASSERT_TRUE(parser.LoadModelData(
        sourcePath.string(),
        meshes,
        materials,
        NLS::Render::Resources::Parsers::EModelParserFlags::NONE,
        nullptr,
        true));

    ASSERT_EQ(meshes.size(), 1u);
    ASSERT_EQ(meshes[0].vertices.size(), 18u);
    ASSERT_EQ(meshes[0].indices.size(), 18u);

    const std::vector<std::array<double, 2>> polygon = {
        std::array<double, 2>{0.0, 0.0},
        std::array<double, 2>{3.0, 0.0},
        std::array<double, 2>{3.0, 1.0},
        std::array<double, 2>{1.0, 1.0},
        std::array<double, 2>{1.0, 2.0},
        std::array<double, 2>{3.0, 2.0},
        std::array<double, 2>{3.0, 3.0},
        std::array<double, 2>{0.0, 3.0}
    };

    double triangleArea = 0.0;
    for (size_t vertexIndex = 0u; vertexIndex < meshes[0].vertices.size(); vertexIndex += 3u)
    {
        const auto& a = meshes[0].vertices[vertexIndex + 0u];
        const auto& b = meshes[0].vertices[vertexIndex + 1u];
        const auto& c = meshes[0].vertices[vertexIndex + 2u];
        triangleArea += TriangleArea(a, b, c);

        const double centroidX =
            (static_cast<double>(a.position[0]) + static_cast<double>(b.position[0]) + static_cast<double>(c.position[0])) /
            3.0;
        const double centroidY =
            (static_cast<double>(a.position[1]) + static_cast<double>(b.position[1]) + static_cast<double>(c.position[1])) /
            3.0;
        EXPECT_TRUE(PointInPolygon(polygon, centroidX, centroidY))
            << "triangle centroid outside polygon at " << centroidX << ", " << centroidY;
    }

    EXPECT_NEAR(triangleArea, PolygonArea(polygon), 1e-5);

    std::filesystem::remove_all(root);
}

TEST(FbxSdkIntegrationContractTests, FbxSdkParserSplitsPolygonMeshByFaceMaterialIntoScenePrimitives)
{
    const auto root = MakeFbxSdkImportTestRoot("NullusFbxSdkMultiMaterialTests");
    const auto sourcePath = root / "MultiMaterial.fbx";
    ASSERT_TRUE(WriteMultiMaterialPolygonFbx(sourcePath));

    NLS::Render::Resources::Parsers::FbxSdkParser parser;
    std::vector<NLS::Render::Resources::Parsers::ParsedMeshData> meshes;
    std::vector<std::string> materials;
    ASSERT_TRUE(parser.LoadModelData(
        sourcePath.string(),
        meshes,
        materials,
        NLS::Render::Resources::Parsers::EModelParserFlags::NONE,
        nullptr,
        false));

    ASSERT_EQ(materials.size(), 2u);
    ASSERT_EQ(meshes.size(), 2u);
    EXPECT_EQ(meshes[0].materialIndex, 0u);
    EXPECT_EQ(meshes[1].materialIndex, 1u);
    EXPECT_EQ(meshes[0].sourceKey, "parser/mesh/0/primitive/0");
    EXPECT_EQ(meshes[1].sourceKey, "parser/mesh/0/primitive/1");
    ASSERT_EQ(meshes[0].vertices.size(), 3u);
    ASSERT_EQ(meshes[1].vertices.size(), 3u);

    NLS::Render::Assets::ImportedScene scene;
    ASSERT_TRUE(parser.PopulateImportedSceneData(
        sourcePath,
        NLS::Render::Assets::SceneModelSourceFormat::Fbx,
        scene));

    ASSERT_EQ(scene.meshes.size(), 1u);
    EXPECT_EQ(scene.meshes[0].sourceKey, "parser/mesh/0");
    EXPECT_EQ(scene.meshes[0].primitiveCount, 2u);
    ASSERT_EQ(scene.meshes[0].primitives.size(), 2u);
    EXPECT_EQ(scene.meshes[0].primitives[0].materialKey, "parser/material/0");
    EXPECT_EQ(scene.meshes[0].primitives[1].materialKey, "parser/material/1");

    const auto* node = FindNodeByName(scene, "MultiMaterialNode");
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->meshKey, "parser/mesh/0");

    std::filesystem::remove_all(root);
}

TEST(FbxSdkIntegrationContractTests, FbxSdkParserMapsPolygonMaterialsThroughNodeLocalSlots)
{
    const auto root = MakeFbxSdkImportTestRoot("NullusFbxSdkNodeLocalMaterialOrderTests");
    const auto sourcePath = root / "NodeLocalMaterialOrder.fbx";
    ASSERT_TRUE(WriteNodeLocalMaterialOrderFbx(sourcePath));

    NLS::Render::Resources::Parsers::FbxSdkParser parser;
    std::vector<NLS::Render::Resources::Parsers::ParsedMeshData> meshes;
    std::vector<std::string> materials;
    ASSERT_TRUE(parser.LoadModelData(
        sourcePath.string(),
        meshes,
        materials,
        NLS::Render::Resources::Parsers::EModelParserFlags::NONE,
        nullptr,
        false));

    ASSERT_EQ(materials.size(), 2u);
    EXPECT_EQ(materials[0], "SceneRed");
    EXPECT_EQ(materials[1], "SceneBlue");
    ASSERT_EQ(meshes.size(), 2u);
    EXPECT_EQ(meshes[0].materialIndex, 1u);
    EXPECT_EQ(meshes[1].materialIndex, 0u);

    NLS::Render::Assets::ImportedScene scene;
    ASSERT_TRUE(parser.PopulateImportedSceneData(
        sourcePath,
        NLS::Render::Assets::SceneModelSourceFormat::Fbx,
        scene));

    ASSERT_EQ(scene.meshes.size(), 1u);
    ASSERT_EQ(scene.meshes[0].primitives.size(), 2u);
    EXPECT_EQ(scene.meshes[0].primitives[0].materialKey, "parser/material/1");
    EXPECT_EQ(scene.meshes[0].primitives[1].materialKey, "parser/material/0");

    std::filesystem::remove_all(root);
}

TEST(FbxSdkIntegrationContractTests, FbxSdkParserLeavesInvalidNodeLocalMaterialSlotsUnbound)
{
    const auto root = MakeFbxSdkImportTestRoot("NullusFbxSdkInvalidNodeMaterialSlotTests");
    const auto sourcePath = root / "InvalidNodeMaterialSlot.fbx";
    ASSERT_TRUE(WriteInvalidNodeMaterialSlotFbx(sourcePath));

    NLS::Render::Resources::Parsers::FbxSdkParser parser;
    std::vector<NLS::Render::Resources::Parsers::ParsedMeshData> meshes;
    std::vector<std::string> materials;
    ASSERT_TRUE(parser.LoadModelData(
        sourcePath.string(),
        meshes,
        materials,
        NLS::Render::Resources::Parsers::EModelParserFlags::NONE,
        nullptr,
        false));

    ASSERT_GE(materials.size(), 2u);
    ASSERT_EQ(meshes.size(), 1u);
    EXPECT_EQ(meshes[0].materialIndex, std::numeric_limits<uint32_t>::max());

    NLS::Render::Assets::ImportedScene scene;
    ASSERT_TRUE(parser.PopulateImportedSceneData(
        sourcePath,
        NLS::Render::Assets::SceneModelSourceFormat::Fbx,
        scene));

    ASSERT_EQ(scene.meshes.size(), 1u);
    ASSERT_EQ(scene.meshes[0].primitives.size(), 1u);
    EXPECT_TRUE(scene.meshes[0].primitives[0].materialKey.empty());

    std::filesystem::remove_all(root);
}

TEST(FbxSdkIntegrationContractTests, FbxSdkParserImportsTangentsBinormalsAndUvLayerAttributes)
{
    const auto root = MakeFbxSdkImportTestRoot("NullusFbxSdkTangentTests");
    const auto sourcePath = root / "Tangent.fbx";
    ASSERT_TRUE(WriteTangentAndMultiUvFbx(sourcePath));

    NLS::Render::Resources::Parsers::FbxSdkParser parser;
    std::vector<NLS::Render::Resources::Parsers::ParsedMeshData> meshes;
    std::vector<std::string> materials;
    ASSERT_TRUE(parser.LoadModelData(
        sourcePath.string(),
        meshes,
        materials,
        NLS::Render::Resources::Parsers::EModelParserFlags::NONE,
        nullptr,
        true));

    ASSERT_EQ(meshes.size(), 1u);
    ASSERT_EQ(meshes[0].vertices.size(), 3u);
    for (const auto& vertex : meshes[0].vertices)
    {
        EXPECT_FLOAT_EQ(vertex.tangent[0], 1.0f);
        EXPECT_FLOAT_EQ(vertex.tangent[1], 0.0f);
        EXPECT_FLOAT_EQ(vertex.tangent[2], 0.0f);
        EXPECT_FLOAT_EQ(vertex.bitangent[0], 0.0f);
        EXPECT_FLOAT_EQ(vertex.bitangent[1], 1.0f);
        EXPECT_FLOAT_EQ(vertex.bitangent[2], 0.0f);
    }

    NLS::Render::Assets::ImportedScene scene;
    ASSERT_TRUE(parser.PopulateImportedSceneData(
        sourcePath,
        NLS::Render::Assets::SceneModelSourceFormat::Fbx,
        scene));

    ASSERT_EQ(scene.meshes.size(), 1u);
    const auto& attributes = scene.meshes[0].attributes;
    EXPECT_NE(std::find(attributes.begin(), attributes.end(), "POSITION"), attributes.end());
    EXPECT_NE(std::find(attributes.begin(), attributes.end(), "NORMAL"), attributes.end());
    EXPECT_NE(std::find(attributes.begin(), attributes.end(), "TANGENT"), attributes.end());
    EXPECT_NE(std::find(attributes.begin(), attributes.end(), "BINORMAL"), attributes.end());
    EXPECT_NE(std::find(attributes.begin(), attributes.end(), "TEXCOORD_0"), attributes.end());
    EXPECT_NE(std::find(attributes.begin(), attributes.end(), "TEXCOORD_1"), attributes.end());

    std::filesystem::remove_all(root);
}

TEST(FbxSdkIntegrationContractTests, FbxSdkParserReusesSharedMeshDataForMultipleNodes)
{
    const auto root = MakeFbxSdkImportTestRoot("NullusFbxSdkSharedMeshTests");
    const auto sourcePath = root / "SharedMesh.fbx";
    ASSERT_TRUE(WriteSharedMeshFbx(sourcePath));

    NLS::Render::Resources::Parsers::FbxSdkParser parser;
    std::vector<NLS::Render::Resources::Parsers::ParsedMeshData> meshes;
    std::vector<std::string> materials;
    ASSERT_TRUE(parser.LoadModelData(
        sourcePath.string(),
        meshes,
        materials,
        NLS::Render::Resources::Parsers::EModelParserFlags::NONE,
        nullptr,
        false));

    ASSERT_EQ(meshes.size(), 1u);
    EXPECT_EQ(meshes[0].sourceKey, "parser/mesh/0");

    NLS::Render::Assets::ImportedScene scene;
    ASSERT_TRUE(parser.PopulateImportedSceneData(
        sourcePath,
        NLS::Render::Assets::SceneModelSourceFormat::Fbx,
        scene));

    ASSERT_EQ(scene.meshes.size(), 1u);
    const auto* left = FindNodeByName(scene, "SharedLeft");
    const auto* right = FindNodeByName(scene, "SharedRight");
    ASSERT_NE(left, nullptr);
    ASSERT_NE(right, nullptr);
    EXPECT_EQ(left->meshKey, "parser/mesh/0");
    EXPECT_EQ(right->meshKey, "parser/mesh/0");

    std::filesystem::remove_all(root);
}
#endif

TEST(FbxSdkIntegrationContractTests, RuntimeFbxSourceLoadUsesAssimpFbxWithAutodeskFallback)
{
    const auto meshManagerSource = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Core/ResourceManagement/MeshManager.cpp");

    ASSERT_FALSE(meshManagerSource.empty());

    EXPECT_NE(meshManagerSource.find("AssimpParser"), std::string::npos);
    EXPECT_NE(meshManagerSource.find("FbxSdkParser"), std::string::npos);

    const auto managerFbxBranch = SliceScopedBlock(meshManagerSource, "if (extension == \".fbx\")");
    ASSERT_FALSE(managerFbxBranch.empty());

    const auto assimpGuard = managerFbxBranch.find("#if NLS_HAS_ASSIMP_FBX_IMPORTER");
    const auto assimpParser = managerFbxBranch.find("AssimpParser parser");
    const auto autodeskGuard = managerFbxBranch.find("#if NLS_HAS_AUTODESK_FBX_SDK");
    const auto autodeskFallback = managerFbxBranch.find("if (!loaded)", autodeskGuard);
    const auto autodeskParser = managerFbxBranch.find("FbxSdkParser parser");

    ASSERT_NE(assimpGuard, std::string::npos);
    ASSERT_NE(assimpParser, std::string::npos);
    ASSERT_NE(autodeskGuard, std::string::npos);
    ASSERT_NE(autodeskFallback, std::string::npos);
    ASSERT_NE(autodeskParser, std::string::npos);

    EXPECT_LT(assimpGuard, assimpParser);
    EXPECT_LT(assimpParser, autodeskGuard);
    EXPECT_LT(autodeskGuard, autodeskFallback);
    EXPECT_LT(autodeskFallback, autodeskParser);
}

TEST(FbxSdkIntegrationContractTests, CMakeUsesBundledSdkPathAndDefaultsToAssimpFbx)
{
    const auto cmake = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "ThirdParty/CMakeLists.txt");

    ASSERT_FALSE(cmake.empty());
    EXPECT_NE(cmake.find("FBX/sdk/windows"), std::string::npos);
    EXPECT_NE(cmake.find("FBX/sdk/linux"), std::string::npos);
    EXPECT_NE(cmake.find("FBX/sdk/macos"), std::string::npos);
    EXPECT_NE(cmake.find("NLS_ENABLE_ASSIMP_FBX_IMPORTER"), std::string::npos);
    EXPECT_NE(cmake.find("option(NLS_ENABLE_ASSIMP_FBX_IMPORTER \"Enable Assimp FBX importer without enabling every Assimp format\" ON)"), std::string::npos);
    EXPECT_NE(cmake.find("option(NLS_ENABLE_AUTODESK_FBX_SDK \"Enable Autodesk FBX SDK integration when the bundled SDK is available\" OFF)"), std::string::npos);
    EXPECT_NE(cmake.find("ASSIMP_BUILD_FBX_IMPORTER ${NLS_ENABLE_ASSIMP_FBX_IMPORTER}"), std::string::npos);
    ExpectNoExternalFbxSdkLookup(cmake);
    ExpectNoVersionedFbxSdkDirectory(cmake);
}

TEST(FbxSdkIntegrationContractTests, CMakeExposesNarrowAssimpFbxImporterOptionWithoutExporters)
{
    const auto cmake = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "ThirdParty/CMakeLists.txt");

    ASSERT_FALSE(cmake.empty());
    EXPECT_NE(cmake.find("option(NLS_ENABLE_ASSIMP_FBX_IMPORTER"), std::string::npos);
    EXPECT_NE(cmake.find("set(ASSIMP_BUILD_FBX_IMPORTER ${NLS_ENABLE_ASSIMP_FBX_IMPORTER}"), std::string::npos);
    EXPECT_NE(cmake.find("set(ASSIMP_BUILD_GLTF_IMPORTER ON"), std::string::npos);
    EXPECT_NE(cmake.find("set(ASSIMP_BUILD_OBJ_IMPORTER ON"), std::string::npos);
    EXPECT_NE(cmake.find("set(ASSIMP_NO_EXPORT ON"), std::string::npos);
}

TEST(FbxSdkIntegrationContractTests, CMakeForcesAssimpFbxImporterOnForAllFormatsMode)
{
    const auto cmake = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "ThirdParty/CMakeLists.txt");

    ASSERT_FALSE(cmake.empty());
    const auto allFormatsBranch = SliceBetween(
        cmake,
        "if(NLS_ASSIMP_BUILD_ALL_FORMATS)",
        "else()");
    ASSERT_FALSE(allFormatsBranch.empty());
    EXPECT_NE(allFormatsBranch.find("set(ASSIMP_BUILD_FBX_IMPORTER ON"), std::string::npos);
}

TEST(FbxSdkIntegrationContractTests, RenderTargetPublishesAssimpFbxAvailabilityDefine)
{
    const auto cmake = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Rendering/CMakeLists.txt");

    ASSERT_FALSE(cmake.empty());
    EXPECT_NE(cmake.find("NLS_HAS_ASSIMP_FBX_IMPORTER"), std::string::npos);
    EXPECT_NE(cmake.find("NLS_ENABLE_ASSIMP_FBX_IMPORTER"), std::string::npos);
    EXPECT_NE(cmake.find("NLS_ASSIMP_BUILD_ALL_FORMATS"), std::string::npos);
}

TEST(FbxSdkIntegrationContractTests, CMakeAllowsFreshCloneWithoutFbxSdkUnlessExplicitlyRequired)
{
    const auto cmake = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "ThirdParty/CMakeLists.txt");

    ASSERT_FALSE(cmake.empty());
    EXPECT_NE(cmake.find("NLS_ENABLE_AUTODESK_FBX_SDK"), std::string::npos);
    EXPECT_NE(cmake.find("NLS_REQUIRE_BUNDLED_FBX_SDK"), std::string::npos);
    EXPECT_NE(cmake.find("NLS_HAS_AUTODESK_FBX_SDK=0"), std::string::npos);
    EXPECT_NE(cmake.find("nls_create_fbx_sdk_unavailable_target"), std::string::npos);
    EXPECT_NE(cmake.find("message(WARNING"), std::string::npos);
}

TEST(FbxSdkIntegrationContractTests, FbxSdkParserCompilesWithSdkUnavailableFallback)
{
    const auto parser = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Rendering/Resources/Parsers/FbxSdkParser.cpp");

    ASSERT_FALSE(parser.empty());
    EXPECT_NE(parser.find("#if NLS_HAS_AUTODESK_FBX_SDK"), std::string::npos);
    EXPECT_NE(parser.find("#else"), std::string::npos);
    EXPECT_NE(parser.find("Autodesk FBX SDK is unavailable in this build."), std::string::npos);
    EXPECT_NE(parser.find("return false;"), std::string::npos);
}

#if NLS_HAS_AUTODESK_FBX_SDK
TEST(FbxSdkIntegrationContractTests, GlobalScaleKeepsEditorHelperFbxModelsAtIconScale)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const std::vector<std::filesystem::path> helperModels = {
        root / "App/Assets/Editor/Models/Camera.fbx",
        root / "App/Assets/Editor/Models/Vertical_Plane.fbx"
    };
    const auto flags =
        NLS::Render::Resources::Parsers::EModelParserFlags::TRIANGULATE |
        NLS::Render::Resources::Parsers::EModelParserFlags::GLOBAL_SCALE;
    const auto flagsWithoutGlobalScale =
        NLS::Render::Resources::Parsers::EModelParserFlags::TRIANGULATE;

    for (const auto& helperModel : helperModels)
    {
        NLS::Render::Resources::Parsers::FbxSdkParser parser;
        std::vector<NLS::Render::Resources::Parsers::ParsedMeshData> meshes;
        std::vector<std::string> materials;

        ASSERT_TRUE(parser.LoadModelData(
            helperModel.string(),
            meshes,
            materials,
            flags,
            nullptr,
            true)) << helperModel.string();
        ASSERT_FALSE(meshes.empty()) << helperModel.string();

        const float scaledMaxPosition = MaxAbsVertexPosition(meshes);
        EXPECT_GT(scaledMaxPosition, 0.25f) << helperModel.string();
        EXPECT_LT(scaledMaxPosition, 2.0f) << helperModel.string();

        NLS::Render::Resources::Parsers::FbxSdkParser unscaledParser;
        std::vector<NLS::Render::Resources::Parsers::ParsedMeshData> unscaledMeshes;
        std::vector<std::string> unscaledMaterials;
        ASSERT_TRUE(unscaledParser.LoadModelData(
            helperModel.string(),
            unscaledMeshes,
            unscaledMaterials,
            flagsWithoutGlobalScale,
            nullptr,
            true)) << helperModel.string();
        ASSERT_FALSE(unscaledMeshes.empty()) << helperModel.string();

        EXPECT_GT(MaxAbsVertexPosition(unscaledMeshes), scaledMaxPosition * 50.0f)
            << helperModel.string();
    }
}

TEST(FbxSdkIntegrationContractTests, GlobalScalePreservesUnbakedNodeScaleWhileScalingPositions)
{
    const auto helperModel = std::filesystem::path(NLS_ROOT_DIR) / "App/Assets/Editor/Models/Arrow_Picking.fbx";
    const auto flags =
        NLS::Render::Resources::Parsers::EModelParserFlags::TRIANGULATE |
        NLS::Render::Resources::Parsers::EModelParserFlags::GLOBAL_SCALE;
    const auto flagsWithoutGlobalScale =
        NLS::Render::Resources::Parsers::EModelParserFlags::TRIANGULATE;

    NLS::Render::Resources::Parsers::FbxSdkParser parser;
    std::vector<NLS::Render::Resources::Parsers::ParsedMeshData> meshes;
    std::vector<std::string> materials;

    ASSERT_TRUE(parser.LoadModelData(
        helperModel.string(),
        meshes,
        materials,
        flags,
        nullptr,
        false));
    ASSERT_FALSE(meshes.empty());

    NLS::Render::Assets::ImportedScene scene;
    ASSERT_TRUE(parser.PopulateImportedSceneData(
        helperModel,
        NLS::Render::Assets::SceneModelSourceFormat::Fbx,
        scene));

    const auto* cylinderNode = FindNodeByName(scene, "Cylinder");
    ASSERT_NE(cylinderNode, nullptr);
    ASSERT_EQ(cylinderNode->translation.size(), 3u);
    ASSERT_EQ(cylinderNode->scale.size(), 3u);

    EXPECT_LT(MaxAbsVertexPosition(meshes), 2.0f);
    EXPECT_NEAR(cylinderNode->translation[0], 0.0, 1e-5);
    EXPECT_NEAR(cylinderNode->translation[1], 0.0, 1e-5);
    EXPECT_NEAR(cylinderNode->translation[2], 0.65, 1e-5);
    EXPECT_NEAR(cylinderNode->scale[0], 100.0, 1e-5);
    EXPECT_NEAR(cylinderNode->scale[1], 100.0, 1e-5);
    EXPECT_NEAR(cylinderNode->scale[2], 100.0, 1e-5);

    NLS::Render::Resources::Parsers::FbxSdkParser unscaledParser;
    std::vector<NLS::Render::Resources::Parsers::ParsedMeshData> unscaledMeshes;
    std::vector<std::string> unscaledMaterials;

    ASSERT_TRUE(unscaledParser.LoadModelData(
        helperModel.string(),
        unscaledMeshes,
        unscaledMaterials,
        flagsWithoutGlobalScale,
        nullptr,
        false));
    ASSERT_FALSE(unscaledMeshes.empty());

    NLS::Render::Assets::ImportedScene unscaledScene;
    ASSERT_TRUE(unscaledParser.PopulateImportedSceneData(
        helperModel,
        NLS::Render::Assets::SceneModelSourceFormat::Fbx,
        unscaledScene));

    const auto* unscaledCylinderNode = FindNodeByName(unscaledScene, "Cylinder");
    ASSERT_NE(unscaledCylinderNode, nullptr);
    ASSERT_EQ(unscaledCylinderNode->translation.size(), 3u);
    ASSERT_EQ(unscaledCylinderNode->scale.size(), 3u);

    EXPECT_GT(MaxAbsVertexPosition(unscaledMeshes), MaxAbsVertexPosition(meshes) * 50.0f);
    EXPECT_NEAR(unscaledCylinderNode->translation[2], 65.0, 1e-5);
    EXPECT_NEAR(unscaledCylinderNode->scale[0], 100.0, 1e-5);
    EXPECT_NEAR(unscaledCylinderNode->scale[1], 100.0, 1e-5);
    EXPECT_NEAR(unscaledCylinderNode->scale[2], 100.0, 1e-5);
}
#endif
