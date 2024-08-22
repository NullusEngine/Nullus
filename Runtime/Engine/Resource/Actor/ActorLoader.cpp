#include "GameObject.h"

#include "Components/TransformComponent.h"
#include "Components/MeshRenderer.h"
#include "Components/MaterialRenderer.h"
#include "Rendering/Geometry/Vertex.h"
#include "Resources/Material.h"

#include "ActorLoader.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include "ResourceManagement/ModelManager.h"
#include "ResourceManagement/TextureManager.h"
#include "Core/ResourceManagement/ShaderManager.h"
#include "Core/ServiceLocator.h"
#include <Utils/PathParser.h>

using namespace NLS::Engine;
using namespace NLS::Engine::Components;
using namespace NLS::Render::Resources;
using namespace NLS;

class AssimpLoader
{
public:
	AssimpLoader(const std::string& actorPath, const std::string& absPath) : mActorPath(absPath)
	{
		mActorName = actorPath.substr(actorPath.find_last_of('/') + 1);
		mDirectory = Utils::PathParser::GetContainingFolder(actorPath);
	}

	GameObject* LoadActor()
	{
		Assimp::Importer importer;
		const aiScene* scene = importer.ReadFile(mActorPath, static_cast<unsigned int>(aiProcess_Triangulate));
	
		if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode)
			return nullptr;

		ProcessMaterials(scene);
	
		GameObject* rootGameObject = CreateGameObject(mActorName, nullptr);
		ProcessNode(scene, scene->mRootNode, rootGameObject);
	
		return rootGameObject;
	}

private:

	void ProcessMaterials(const aiScene* scene)
	{
		if (!scene)
			return;

		mMaterials.reserve(scene->mNumMaterials);
		for (uint32_t i = 0; i < scene->mNumMaterials; ++i)
		{
			aiMaterial* material = scene->mMaterials[i];
			mMaterials.push_back(ProcessMaterial(material));
		}
	}

	Material* ProcessMaterial(const aiMaterial* material)
	{
		if (!material)
			return nullptr;

		auto shader = NLS_SERVICE(NLS::Core::ResourceManagement::ShaderManager).CreateResource(":Shaders\\Standard.glsl");
		auto nlsMaterial = new NLS::Render::Resources::Material(shader);

		aiString name;
		aiGetMaterialString(material, AI_MATKEY_NAME, &name);

		aiString diffuseMapPath;
		if (material->GetTexture(aiTextureType_DIFFUSE, 0, &diffuseMapPath) == AI_SUCCESS)
		{
			NLS::Render::Resources::Texture2D* texture = NLS_SERVICE(NLS::Core::ResourceManagement::TextureManager).CreateResource(mDirectory + "/" + std::string(diffuseMapPath.C_Str()));

			nlsMaterial->Set<Render::Resources::Texture2D*>("u_DiffuseMap", texture);
		}

		aiString specularMapPath;
		if (material->GetTexture(aiTextureType_SPECULAR, 0, &specularMapPath) == AI_SUCCESS)
		{
			NLS::Render::Resources::Texture2D* texture = NLS_SERVICE(NLS::Core::ResourceManagement::TextureManager).CreateResource(mDirectory + "/" + std::string(specularMapPath.C_Str()));
		
			nlsMaterial->Set<Render::Resources::Texture2D*>("u_SpecularMap", texture);
		}

		aiString normalMapPath;
		if (material->GetTexture(aiTextureType_NORMALS, 0, &normalMapPath) == AI_SUCCESS)
		{
			NLS::Render::Resources::Texture2D* texture = NLS_SERVICE(NLS::Core::ResourceManagement::TextureManager).CreateResource(mDirectory + "/" + std::string(normalMapPath.C_Str()));
				
			nlsMaterial->Set<Render::Resources::Texture2D*>("u_NormalMap", texture);
		}

		aiString heightMapPath;
		if (material->GetTexture(aiTextureType_HEIGHT, 0, &heightMapPath) == AI_SUCCESS)
		{
			NLS::Render::Resources::Texture2D* texture = NLS_SERVICE(NLS::Core::ResourceManagement::TextureManager).CreateResource(mDirectory + "/" + std::string(heightMapPath.C_Str()));
						
			nlsMaterial->Set<Render::Resources::Texture2D*>("u_HeightMap", texture);
		}

		return nlsMaterial;
	}

	Mesh* ProcessMesh(const aiMesh* mesh, int matIndex)
	{
		// vertices
		std::vector<NLS::Render::Geometry::Vertex> vertices;
		for (uint32_t i = 0; i < mesh->mNumVertices; ++i)
		{
			aiVector3D position = mesh->mVertices[i];
			aiVector3D normal = (mesh->mNormals ? mesh->mNormals[i] : aiVector3D(0.0f, 0.0f, 0.0f));
			aiVector3D texCoords = mesh->mTextureCoords[0] ? mesh->mTextureCoords[0][i] : aiVector3D(0.0f, 0.0f, 0.0f);
			aiVector3D tangent = mesh->mTangents ? mesh->mTangents[i] : aiVector3D(0.0f, 0.0f, 0.0f);
			aiVector3D bitangent = mesh->mBitangents ? mesh->mBitangents[i] : aiVector3D(0.0f, 0.0f, 0.0f);

			vertices.push_back
			(
				{
					position.x,
					position.y,
					position.z,
					texCoords.x,
					texCoords.y,
					normal.x,
					normal.y,
					normal.z,
					tangent.x,
					tangent.y,
					tangent.z,
					bitangent.x,
					bitangent.y,
					bitangent.z
				}
			);
		}

		// indices
		std::vector<uint32_t> indices;
		for (uint32_t faceID = 0; faceID < mesh->mNumFaces; ++faceID)
		{
			auto& face = mesh->mFaces[faceID];

			//NLS_ASSERT(face.mNumIndices == 3);

			for (size_t indexID = 0; indexID < face.mNumIndices; ++indexID)
			{
				indices.push_back(face.mIndices[indexID]);
			}
		}

		return new Mesh(vertices, indices, matIndex);
	}

	void ProcessNodeMesh(const aiScene* scene, const aiNode* node, GameObject* gameObject)
	{
		// transform

		if (node->mNumMeshes == 0)
			return;

		MaterialRenderer* materialRenderer = gameObject->AddComponent<MaterialRenderer>();

		std::vector<Mesh*> subMeshes;
		for (uint32_t i = 0; i < node->mNumMeshes; ++i)
		{
			aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
			materialRenderer->SetMaterialAtIndex(i, *mMaterials[mesh->mMaterialIndex]);
			subMeshes.push_back(ProcessMesh(mesh, i));
		}

		Model* model = NLS_SERVICE(NLS::Core::ResourceManagement::ModelManager).CreateResource(node->mName.C_Str(), subMeshes);
		if (model)
		{
			MeshRenderer* meshRender = gameObject->AddComponent<MeshRenderer>();
			meshRender->SetModel(model);
		}

		return;
	}

	GameObject* CreateGameObject(const std::string& name, GameObject* parent)
	{
		bool playing = false;
		GameObject* gameObject = new GameObject(rand(), name, "", playing);
		if (parent != nullptr)
			gameObject->SetParent(*parent);
		return gameObject;
	}

	void ProcessNode(const aiScene* scene, const aiNode* node, GameObject* gameObject)
	{
		// Process node data
		ProcessNodeMesh(scene, node, gameObject);

		// Process children
		for (uint32_t i = 0; i < node->mNumChildren; ++i)
		{
			GameObject* child = CreateGameObject(node->mChildren[i]->mName.C_Str(), gameObject);
			ProcessNode(scene, node->mChildren[i], child);
		}
	}

private:
	std::string mActorPath;

	std::string mActorName;
	std::string mDirectory;

	std::vector<Material*> mMaterials;
};

Actor* NLS::Engine::ActorLoader::LoadActor(const std::string& path, const std::string& absPath)
{
	AssimpLoader loader(path, absPath);
	auto* go = loader.LoadActor();
	auto actor = new Actor();
	actor->SetGameObject(go);
	return actor;
}
