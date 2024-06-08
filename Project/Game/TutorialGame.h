#pragma once
#include "GameTechRenderer.h"
#include "Physics/PhysicsSystem.h"
#include "Physics/PositionConstraint.h"
#include "StateGameObject.h"
namespace NLS
{
namespace Engine
{
class TutorialGame
{
public:
    TutorialGame();
    ~TutorialGame();

    virtual void UpdateGame(float dt);

protected:
    void InitialiseAssets();

    void InitCamera();
    void UpdateKeys();

    void InitWorld();

    void InitGameExamples();

    void InitSphereGridWorld(int numRows, int numCols, float rowSpacing, float colSpacing, float radius);
    void InitMixedGridWorld(int numRows, int numCols, float rowSpacing, float colSpacing);
    void InitCubeGridWorld(int numRows, int numCols, float rowSpacing, float colSpacing, const Vector3& cubeDims);
    void InitDefaultFloor();
    void BridgeConstraintTest();

    bool SelectObject();
    void MoveSelectedObject();
    void DebugObjectMovement();
    void LockedObjectMovement();

    GameObject* AddFloorToWorld(const Vector3& position);
    GameObject* AddSphereToWorld(const Vector3& position, float radius, float inverseMass = 10.0f);
    GameObject* AddCubeToWorld(const Vector3& position, Vector3 dimensions, float inverseMass = 10.0f);

    GameObject* AddCapsuleToWorld(const Vector3& position, float halfHeight, float radius, float inverseMass = 10.0f);

    GameObject* AddPlayerToWorld(const Vector3& position);
    GameObject* AddEnemyToWorld(const Vector3& position);
    GameObject* AddBonusToWorld(const Vector3& position);

    StateGameObject* AddStateObjectToWorld(const Vector3& position);
    StateGameObject* testStateObject;

    GameTechRenderer* renderer;
    PhysicsSystem* physics;
    GameWorld* world;

    bool useGravity;
    bool inSelectionMode;

    float forceMagnitude;

    GameObject* selectionObject = nullptr;

    MeshGeometry* capsuleMesh = nullptr;
    MeshGeometry* cubeMesh = nullptr;
    MeshGeometry* sphereMesh = nullptr;
    TextureBase* basicTex = nullptr;
    ShaderBase* basicShader = nullptr;

    // Coursework Meshes
    MeshGeometry* charMeshA = nullptr;
    MeshGeometry* charMeshB = nullptr;
    MeshGeometry* enemyMesh = nullptr;
    MeshGeometry* bonusMesh = nullptr;

    // Coursework Additional functionality
    GameObject* lockedObject = nullptr;
    Vector3 lockedOffset = Vector3(0, 14, 20);
    void LockCameraToObject(GameObject* o)
    {
        lockedObject = o;
    }
};
} // namespace Engine
} // namespace NLS
