#pragma once
#include "Matrix4.h"
#include "TextureBase.h"
#include "ShaderBase.h"
#include "Vector4.h"
#include "EngineDef.h"
namespace NLS {
	using namespace NLS::Rendering;

	class MeshGeometry;
	namespace CSC8503 {
		class Transform;
		using namespace Maths;

		class NLS_ENGINE_API RenderObject
		{
		public:
			RenderObject(Transform* parentTransform, MeshGeometry* mesh, TextureBase* tex, ShaderBase* shader);
			~RenderObject();

			void SetDefaultTexture(TextureBase* t) {
				texture = t;
			}

			TextureBase* GetDefaultTexture() const {
				return texture;
			}

			MeshGeometry*	GetMesh() const {
				return mesh;
			}

			Transform*		GetTransform() const {
				return transform;
			}

			ShaderBase*		GetShader() const {
				return shader;
			}

			void SetColour(const Vector4& c) {
				colour = c;
			}

			Vector4 GetColour() const {
				return colour;
			}

		protected:
			MeshGeometry*	mesh;
			TextureBase*	texture;
			ShaderBase*		shader;
			Transform*		transform;
			Vector4			colour;
		};
	}
}
