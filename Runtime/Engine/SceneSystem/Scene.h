#pragma once


#include <memory>
#include <unordered_set>
#include <Eventing/Event.h>

#include "GameObject.h"
#include "Components/CameraComponent.h"
#include "Components/LightComponent.h"
#include "Components/SkyBoxComponent.h"
#include "Components/MeshRenderer.h"
#include "Components/UICanvasComponent.h"
#include "Components/ParticleSystemComponent.h"
#include "Components/PhysicsColliderComponent.h"
#include "Components/PhysicsCollider2DComponent.h"
#include "Components/Rigidbody2DComponent.h"
#include "Components/RigidbodyComponent.h"
#include "Components/AnimatorComponent.h"
#include "Components/SkinnedMeshRenderer.h"
#include "Components/SpriteRendererComponent.h"

#include "EngineDef.h"
#include "Reflection/Macros.h"
#include "Reflection/Object.h"
#include "SceneSystem/ScenePhysicsSettings.h"
#include "SceneSystem/PhysicsPresentationTransform.h"
#include "SceneSystem/Scene.generated.h"

namespace NLS::Physics
{
	class PhysicsWorld;
    class PhysicsCommandBuffer;
}

namespace NLS::Engine::SceneSystem
{
	/**
	* The scene is a set of GameObjects
	*/
	CLASS(NLS_ENGINE_API Scene) : public NLS::Object
	{
    public:
		GENERATED_BODY()
		// Engine publishes a type-agnostic component notification. Higher-level
		// Contexts can bind optional systems such as scripting without creating a
		// reverse dependency from Engine to those systems.
		NLS::Event<Components::Component*> ComponentAddedEvent;
        enum class AddGameObjectActivation
        {
            Immediate,
            Deferred
        };

		/**
		* Contains a set of vectors of components that are sorted. It allows fast
		* manipulation of components without parsing the whole scene
		*/
		struct FastAccessComponents
		{
			std::vector<Components::MeshRenderer*>		modelRenderers;
            std::vector<Components::AnimatorComponent*> animators;
            std::vector<Components::SkinnedMeshRenderer*> skinnedMeshes;
            std::vector<Components::SpriteRendererComponent*> sprites;
			std::vector<Components::CameraComponent*>			cameras;
			std::vector<Components::LightComponent*>			lights;
			std::vector<Components::SkyBoxComponent*>			skyboxs;
			std::vector<Components::UICanvasComponent*>			uiCanvases;
                std::vector<Components::ParticleSystemComponent*> particleSystems;
			std::vector<Components::PhysicsColliderComponent*>	physicsColliders;
                std::vector<Components::RigidbodyComponent*> rigidbodies;
                std::vector<Components::Rigidbody2DComponent*> rigidbodies2D;
                std::vector<Components::PhysicsCollider2DComponent*> physicsColliders2D;
		};

		/**
		* Constructor of the scene
		*/
		Scene();

		/**
		* Handle the memory de-allocation of every GameObjects
		*/
		~Scene();

            // A diagnostic recovery can display known objects while retaining
            // the original graph. It must never become a lossy save source.
            bool IsSerializationReadOnly() const { return !m_recoveryDocument.empty(); }
            const std::string& GetSerializationRecoveryDocument() const { return m_recoveryDocument; }
            void MarkSerializationRecovery(std::string originalDocument) { m_recoveryDocument = std::move(originalDocument); }

		/**
		* Play the scene
		*/
		FUNCTION()
		void Play();

		/**
		* Stop the scene and leave it in edit mode.
		*/
		void Stop();

		/**
		* Returns true if the scene is playing
		*/
		bool IsPlaying() const;

		/**
		* Update every GameObjects
		* @param p_deltaTime
		*/
		void Update(float p_deltaTime);
            // Script hosts flush scheduled gameplay callbacks between these
            // stages. Preview hosts can continue to use Update for both.
            void UpdateGameplay(float p_deltaTime);
            void UpdateAnimations(float p_deltaTime);

		/**
		* Update every GameObjects 60 frames per seconds
		* @param p_deltaTime
		*/
		void FixedUpdate(float p_deltaTime);
        std::vector<NLS::Physics::ContactEvent> SimulatePhysics(float fixedDeltaTime);
        NLS::Event<const NLS::Physics::ContactEvent&> PhysicsContactEvent;

		/**
		* Update every GameObjects lately
		* @param p_deltaTime
		*/
		void LateUpdate(float p_deltaTime);

		/**
		* Create an GameObject with a default name and return a reference to it.
		*/
		GameObject& CreateGameObject();

		/**
		* Create an GameObject with the given name and return a reference to it.
		* @param p_name
		* @param p_tag
		*/
			GameObject& CreateGameObject(const std::string& p_name, const std::string& p_tag = "");

			GameObject& CreateEditorTransientGameObject(const std::string& p_name, const std::string& p_tag = "");

			bool AddGameObject(
                GameObject* gameObject,
                AddGameObjectActivation activation = AddGameObjectActivation::Immediate);
            bool AddGameObjects(
                const std::vector<GameObject*>& gameObjects,
                AddGameObjectActivation activation = AddGameObjectActivation::Immediate);
            size_t ActivateGameObjectForPlay(GameObject* gameObject);

		/**
		* Destroy and GameObject and return true on success
		* @param p_target (The GameObject to remove from the scene)
		*/
		bool DestroyGameObject(GameObject& p_target);

		/**
		* Collect garbages by removing Destroyed-marked GameObjects
		*/
		void CollectGarbages();

		/**
		* Return the first GameObject identified by the given name, or nullptr on fail
		* @param p_name
		*/
		GameObject* FindGameObjectByName(const std::string& p_name) const;

		/**
		* Return the first GameObject identified by the given tag, or nullptr on fail
		* @param p_tag
		*/
		GameObject* FindGameObjectByTag(const std::string& p_tag) const;

		/**
		* Return every GameObjects identified by the given name
		* @param p_name
		*/
		std::vector<std::reference_wrapper<GameObject>> FindGameObjectsByName(const std::string& p_name) const;

		/**
		* Return every GameObjects identified by the given tag
		* @param p_tag
		*/
		std::vector<std::reference_wrapper<GameObject>> FindGameObjectsByTag(const std::string& p_tag) const;

		/**
		* Parse the scene to find the main camera
		*/
		Components::CameraComponent* FindMainCamera() const;

		/**
		* Callback method called everytime a component is added on an GameObject of the scene
		* @param p_component
		*/
        void OnComponentAdded(Components::Component* p_compononent);

		/**
		* Callback method called everytime a component is removed on an GameObject of the scene
		* @param p_component
		*/
        void OnComponentRemoved(Components::Component* p_compononent);

		/**
		* Return a reference on the GameObject map
		*/
		std::vector<GameObject*>& GetGameObjects();
        FUNCTION()
		const std::vector<GameObject*>& GetGameObjects() const;

		/**
		* Return the fast access components data structure
		*/
		const FastAccessComponents& GetFastAccessComponents() const;
		uint64_t GetFastAccessComponentsRevision() const;
		uint64_t GetRenderContentRevision() const;
		void MarkRenderContentChanged();

		/**
		* Returns the PhysX query world owned by this scene, creating it on first
		* use. Colliders publish into it through the update synchronization point,
		* so a read view observes a consistent set of bodies.
		*/
		const std::shared_ptr<NLS::Physics::PhysicsWorld>& GetPhysicsWorld();
            const std::shared_ptr<NLS::Physics::PhysicsWorld2D>& GetPhysicsWorld2D();
            const std::shared_ptr<NLS::Physics::PhysicsWorld2D>& PeekPhysicsWorld2D() const { return m_physicsWorld2D; }
            void SyncTransforms() { (void)TrySyncTransforms(); }
            bool TrySyncTransforms();
            const ScenePhysicsSettings& GetPhysicsSettings() const { return m_physicsSettings; }
            bool SetPhysicsSettings(const ScenePhysicsSettings& settings, std::string& error);
            // Worker pools are fixed for each native world's lifetime.
            bool HasPendingPhysicsWorkerSettings() const;
            void SetPhysicsInterpolationAlpha(double alpha);
            void ResetPhysicsInterpolation();
            PhysicsPresentationTransform CapturePhysicsPresentationTransform(const GameObject& object) const;
            void QueuePhysicsRemoval(const std::shared_ptr<Physics::PhysicsWorld>& world, Physics::BodyHandle body);
            void QueuePhysicsRemoval(const std::shared_ptr<Physics::PhysicsWorld>& world, Physics::ShapeHandle shape);
            void QueuePhysicsRemoval(const std::shared_ptr<Physics::PhysicsWorld2D>& world, Physics::BodyHandle2D body);
            void QueuePhysicsRemoval(const std::shared_ptr<Physics::PhysicsWorld2D>& world, Physics::ShapeHandle2D shape);
            size_t GetPendingPhysicsCommandCount() const;
            bool IsPhysicsSynchronizing() const { return m_synchronizingPhysics; }
            bool IsPhysicsMutationDeferred() const { return m_physicsMutationDeferralDepth!=0; }
            bool IsPhysicsStopPending() const { return m_physicsStopPending; }
            class NLS_ENGINE_API PhysicsMutationDeferral final
            {
            public:
                explicit PhysicsMutationDeferral(Scene& scene);
                ~PhysicsMutationDeferral();
                PhysicsMutationDeferral(const PhysicsMutationDeferral&) = delete;
                PhysicsMutationDeferral& operator=(const PhysicsMutationDeferral&) = delete;
            private:
                Scene& m_scene;
            };

		/** Returns the query world without creating one. */
		const std::shared_ptr<NLS::Physics::PhysicsWorld>& PeekPhysicsWorld() const { return m_physicsWorld; }

		/** Bumped whenever collider geometry or poses are committed. */
		void NotifyPhysicsChanged();
		[[nodiscard]] uint64_t GetPhysicsRevision() const { return m_physicsRevision; }

		void RebuildRuntimeCachesAfterLoad()
		{
			if (!m_fastAccessComponentsValid)
				RebuildFastAccessComponents();
		}

	private:
		class ScopedFastAccessRebuildDeferral;
        std::string m_recoveryDocument;

        void CollectGameObjectSubtree(GameObject& p_GameObject, std::vector<GameObject*>& p_outGameObjects, std::unordered_set<GameObject*>& p_visitedGameObjects);
        void RemoveGameObjectsFromSceneList(const std::unordered_set<GameObject*>& p_GameObjects);
        void NotifyGameObjectDestroyed(GameObject& p_GameObject);
        void NotifyGameObjectDestroyed(GameObject& p_GameObject, std::unordered_set<GameObject*>& p_notifiedGameObjects);
        void DestroyGameObjectSubtree(GameObject& p_GameObject);
        void DestroyCollectedGameObjects(std::vector<GameObject*>& p_GameObjects);
		void RebuildFastAccessComponents();
		void SynchronizePhysics();
            void CommitPhysicsCommands();
		void RequestFastAccessComponentsRebuild();
		void BeginFastAccessRebuildDeferral();
		void EndFastAccessRebuildDeferral();
		bool m_isPlaying = false;
		bool m_isStopping = false;
            bool m_physicsStopPending = false;
		std::shared_ptr<NLS::Physics::PhysicsWorld> m_physicsWorld;
            std::shared_ptr<NLS::Physics::PhysicsWorld2D> m_physicsWorld2D;
            bool m_physicsWorld2DUnavailable = false;
		uint64_t m_physicsRevision = 0;
		bool m_physicsWorldUnavailable = false;
            ScenePhysicsSettings m_physicsSettings;
            uint32_t m_physicsWorkerCount3D = 0, m_physicsWorkerCount2D = 0;
            double m_physicsInterpolationAlpha = 1;
            bool m_synchronizingPhysics = false;
            uint32_t m_physicsMutationDeferralDepth = 0;
            std::vector<std::unique_ptr<Physics::PhysicsCommandBuffer>> m_physicsStructuralCommands;
		std::vector<GameObject*> m_gameobject;

		FastAccessComponents m_fastAccessComponents;
		bool m_fastAccessComponentsValid = false;
		bool m_fastAccessRebuildPending = false;
		uint32_t m_fastAccessRebuildDeferralDepth = 0u;
		uint64_t m_fastAccessComponentsRevision = 0u;
		uint64_t m_renderContentRevision = 0u;
	};
}
