#include "TutorialGame.h"
#include "GameWorld.h"
#include "TextureLoader.h"
#include "MeshLoader.h"
#include "Window.h"
#include "Assets.h"

using namespace NLS;
using namespace Engine;

TutorialGame::TutorialGame()
{
    world = new GameWorld();
    renderer = new GameTechRenderer(*world);
    physics = new PhysicsSystem(*world);

    forceMagnitude = 10.0f;
    useGravity = false;
    inSelectionMode = false;

#ifdef NLS_USE_GL
    Debug::SetRenderer(renderer);
#endif

    InitialiseAssets();
}

/*

Each of the little demo scenarios used in the game uses the same 2 meshes,
and the same texture and shader. There's no need to ever load in anything else
for this module, even in the coursework, but you can add it if you like!

*/
void TutorialGame::InitialiseAssets()
{
    auto loadFunc = [this](const string& name, MeshGeometry** into)
    {
        *into = MeshLoader::LoadAPIMesh(Assets::MESHDIR + name);
        (*into)->SetPrimitiveType(GeometryPrimitive::Triangles);
        (*into)->UploadToGPU(renderer);
    };

    loadFunc("cube.msh", &cubeMesh);
    loadFunc("sphere.msh", &sphereMesh);
    loadFunc("Male1.msh", &charMeshA);
    loadFunc("courier.msh", &charMeshB);
    loadFunc("security.msh", &enemyMesh);
    loadFunc("coin.msh", &bonusMesh);
    loadFunc("capsule.msh", &capsuleMesh);

    basicTex = TextureLoader::LoadAPITexture("checkerboard.png");
    basicShader = renderer->CreateShader("GameTechVert.glsl", "GameTechFrag.glsl");

    InitCamera();
    InitWorld();
}

TutorialGame::~TutorialGame()
{
    delete cubeMesh;
    delete sphereMesh;
    delete charMeshA;
    delete charMeshB;
    delete enemyMesh;
    delete bonusMesh;

    delete basicTex;
    delete basicShader;

    delete physics;
    delete renderer;
    delete world;
}

void TutorialGame::UpdateGame(float dt)
{
    if (!inSelectionMode)
    {
        world->GetMainCamera()->UpdateCamera(dt);
    }

    UpdateKeys();

#ifdef NLS_USE_GL
    if (useGravity)
    {
        Debug::Print("(G)ravity on", Vector2(5, 95));
    }
    else
    {
        Debug::Print("(G)ravity off", Vector2(5, 95));
    }
#endif

    SelectObject();
    MoveSelectedObject();
    physics->Update(dt);

    if (lockedObject != nullptr)
    {
        Vector3 objPos = lockedObject->GetTransform().GetPosition();
        Vector3 camPos = objPos + lockedOffset;

        Matrix4 temp = Matrix4::BuildViewMatrix(camPos, objPos, Vector3(0, 1, 0));

        Matrix4 modelMat = temp.Inverse();

        Quaternion q(modelMat);
        Vector3 angles = q.ToEuler(); // nearly there now!

        world->GetMainCamera()->SetPosition(camPos);
        world->GetMainCamera()->SetPitch(angles.x);
        world->GetMainCamera()->SetYaw(angles.y);

        // Debug::DrawAxisLines(lockedObject->GetTransform().GetMatrix(), 2.0f);
    }

    world->UpdateWorld(dt);
    renderer->Update(dt);

#ifdef NLS_USE_GL
    Debug::FlushRenderables(dt);
#endif

    renderer->Render();

    if (testStateObject)
    {
        testStateObject->Update(dt);
    }
}

void TutorialGame::UpdateKeys()
{
    if (Window::GetKeyboard()->KeyPressed(KeyboardKeys::F1))
    {
        InitWorld(); // We can reset the simulation at any time with F1
        selectionObject = nullptr;
        lockedObject = nullptr;
    }

    if (Window::GetKeyboard()->KeyPressed(KeyboardKeys::F2))
    {
        InitCamera(); // F2 will reset the camera to a specific default place
    }

    if (Window::GetKeyboard()->KeyPressed(KeyboardKeys::G))
    {
        useGravity = !useGravity; // Toggle gravity!
        physics->UseGravity(useGravity);
    }
    // Running certain physics updates in a consistent order might cause some
    // bias in the calculations - the same objects might keep 'winning' the constraint
    // allowing the other one to stretch too much etc. Shuffling the order so that it
    // is random every frame can help reduce such bias.
    if (Window::GetKeyboard()->KeyPressed(KeyboardKeys::F9))
    {
        world->ShuffleConstraints(true);
    }
    if (Window::GetKeyboard()->KeyPressed(KeyboardKeys::F10))
    {
        world->ShuffleConstraints(false);
    }

    if (Window::GetKeyboard()->KeyPressed(KeyboardKeys::F7))
    {
        world->ShuffleObjects(true);
    }
    if (Window::GetKeyboard()->KeyPressed(KeyboardKeys::F8))
    {
        world->ShuffleObjects(false);
    }

    if (lockedObject)
    {
        LockedObjectMovement();
    }
    else
    {
        DebugObjectMovement();
    }
}

void TutorialGame::LockedObjectMovement()
{
    Matrix4 view = world->GetMainCamera()->BuildViewMatrix();
    Matrix4 camWorld = view.Inverse();

    Vector3 rightAxis = Vector3(camWorld.GetColumn(0)); // view is inverse of model!

    // forward is more tricky -  camera forward is 'into' the screen...
    // so we can take a guess, and use the cross of straight up, and
    // the right axis, to hopefully get a vector that's good enough!

    Vector3 fwdAxis = Vector3::Cross(Vector3(0, 1, 0), rightAxis);
    fwdAxis.y = 0.0f;
    fwdAxis.Normalise();

    Vector3 charForward = lockedObject->GetTransform().GetOrientation() * Vector3(0, 0, 1);
    Vector3 charForward2 = lockedObject->GetTransform().GetOrientation() * Vector3(0, 0, 1);

    float force = 100.0f;

    if (Window::GetKeyboard()->KeyDown(KeyboardKeys::LEFT))
    {
        lockedObject->GetComponent<PhysicsObject>()->AddForce(-rightAxis * force);
    }

    if (Window::GetKeyboard()->KeyDown(KeyboardKeys::RIGHT))
    {
        Vector3 worldPos = selectionObject->GetTransform().GetPosition();
        lockedObject->GetComponent<PhysicsObject>()->AddForce(rightAxis * force);
    }

    if (Window::GetKeyboard()->KeyDown(KeyboardKeys::UP))
    {
        lockedObject->GetComponent<PhysicsObject>()->AddForce(fwdAxis * force);
    }

    if (Window::GetKeyboard()->KeyDown(KeyboardKeys::DOWN))
    {
        lockedObject->GetComponent<PhysicsObject>()->AddForce(-fwdAxis * force);
    }

    if (Window::GetKeyboard()->KeyDown(KeyboardKeys::NEXT))
    {
        lockedObject->GetComponent<PhysicsObject>()->AddForce(Vector3(0, -10, 0));
    }
}

void TutorialGame::DebugObjectMovement()
{
    // If we've selected an object, we can manipulate it with some key presses
    if (inSelectionMode && selectionObject)
    {
        // Twist the selected object!
        if (Window::GetKeyboard()->KeyDown(KeyboardKeys::LEFT))
        {
            selectionObject->GetComponent<PhysicsObject>()->AddTorque(Vector3(-10, 0, 0));
        }

        if (Window::GetKeyboard()->KeyDown(KeyboardKeys::RIGHT))
        {
            selectionObject->GetComponent<PhysicsObject>()->AddTorque(Vector3(10, 0, 0));
        }

        if (Window::GetKeyboard()->KeyDown(KeyboardKeys::NUM7))
        {
            selectionObject->GetComponent<PhysicsObject>()->AddTorque(Vector3(0, 10, 0));
        }

        if (Window::GetKeyboard()->KeyDown(KeyboardKeys::NUM8))
        {
            selectionObject->GetComponent<PhysicsObject>()->AddTorque(Vector3(0, -10, 0));
        }

        if (Window::GetKeyboard()->KeyDown(KeyboardKeys::RIGHT))
        {
            selectionObject->GetComponent<PhysicsObject>()->AddTorque(Vector3(10, 0, 0));
        }

        if (Window::GetKeyboard()->KeyDown(KeyboardKeys::UP))
        {
            selectionObject->GetComponent<PhysicsObject>()->AddForce(Vector3(0, 0, -10));
        }

        if (Window::GetKeyboard()->KeyDown(KeyboardKeys::DOWN))
        {
            selectionObject->GetComponent<PhysicsObject>()->AddForce(Vector3(0, 0, 10));
        }

        if (Window::GetKeyboard()->KeyDown(KeyboardKeys::NUM5))
        {
            selectionObject->GetComponent<PhysicsObject>()->AddForce(Vector3(0, -10, 0));
        }
    }
}

void TutorialGame::InitCamera()
{
    world->GetMainCamera()->SetNearPlane(0.1f);
    world->GetMainCamera()->SetFarPlane(500.0f);
    world->GetMainCamera()->SetPitch(-15.0f);
    world->GetMainCamera()->SetYaw(315.0f);
    world->GetMainCamera()->SetPosition(Vector3(-60, 40, 60));
    lockedObject = nullptr;
}

void TutorialGame::InitWorld()
{
    world->ClearAndErase();
    physics->Clear();

    InitMixedGridWorld(5, 5, 3.5f, 3.5f);
    InitGameExamples();
    InitDefaultFloor();
    BridgeConstraintTest();
    testStateObject = AddStateObjectToWorld(Vector3(0, 10, 0));
}

void TutorialGame::BridgeConstraintTest()
{
    Vector3 cubeSize = Vector3(8, 8, 8);

    float invCubeMass = 5; // how heavy the middle pieces are
    int numLinks = 10;
    float maxDistance = 30;  // constraint distance
    float cubeDistance = 20; // distance between links

    Vector3 startPos = Vector3(0, 100, 0);

    GameObject* start = AddCubeToWorld(startPos + Vector3(0, 0, 0), cubeSize, 0);
    GameObject* end = AddCubeToWorld(startPos + Vector3((numLinks + 2) * cubeDistance, 0, 0), cubeSize, 0);
    GameObject* previous = start;

    for (int i = 0; i < numLinks; ++i)
    {
        GameObject* block = AddCubeToWorld(startPos + Vector3((i + 1) * cubeDistance, 0, 0), cubeSize, invCubeMass);
        PositionConstraint* constraint = new PositionConstraint(previous, block, maxDistance);
        world->AddConstraint(constraint);
        previous = block;
    }
    PositionConstraint* constraint = new PositionConstraint(previous, end, maxDistance);
    world->AddConstraint(constraint);
}

/*

A single function to add a large immoveable cube to the bottom of our world

*/
GameObject* TutorialGame::AddFloorToWorld(const Vector3& position)
{
    GameObject* floor = new GameObject("floor");

    Vector3 floorSize = Vector3(100, 2, 100);
    AABBVolume* volume = floor->AddComponent<AABBVolume>([&](Component* component) {dynamic_cast<AABBVolume*>(component)->SetHalfDimensions(floorSize); });
    floor->GetTransform()
        .SetScale(floorSize * 2)
        .SetPosition(position);

    floor->SetRenderObject(new RenderObject(&floor->GetTransform(), cubeMesh, basicTex, basicShader));
    PhysicsObject* floorPhy = floor->AddComponent<PhysicsObject>();

    floorPhy->SetInverseMass(0);
    floorPhy->InitCubeInertia();

    world->AddGameObject(floor);

    return floor;
}

/*

Builds a game object that uses a sphere mesh for its graphics, and a bounding sphere for its
rigid body representation. This and the cube function will let you build a lot of 'simple'
physics worlds. You'll probably need another function for the creation of OBB cubes too.

*/
GameObject* TutorialGame::AddSphereToWorld(const Vector3& position, float radius, float inverseMass)
{
    GameObject* sphere = new GameObject("sphere");

    Vector3 sphereSize = Vector3(radius, radius, radius);
    SphereVolume* volume = sphere->AddComponent<SphereVolume>([&](Component* component) {dynamic_cast<SphereVolume*>(component)->SetRadius(radius); });// new SphereVolume(radius);

    sphere->GetTransform()
        .SetScale(sphereSize)
        .SetPosition(position);

    sphere->SetRenderObject(new RenderObject(&sphere->GetTransform(), sphereMesh, basicTex, basicShader));
    PhysicsObject* phyObj = sphere->AddComponent<PhysicsObject>();

    phyObj->SetInverseMass(inverseMass);
    phyObj->InitSphereInertia();

    world->AddGameObject(sphere);

    return sphere;
}

GameObject* TutorialGame::AddCapsuleToWorld(const Vector3& position, float halfHeight, float radius, float inverseMass)
{
    GameObject* capsule = new GameObject("capsule");

    capsule->AddComponent<CapsuleVolume>([&](Component* component) 
        {
            dynamic_cast<CapsuleVolume*>(component)->SetRadius(radius); 
            dynamic_cast<CapsuleVolume*>(component)->SetHalfHeight(halfHeight);
        });

    capsule->GetTransform()
        .SetScale(Vector3(radius * 2, halfHeight, radius * 2))
        .SetPosition(position);

    capsule->SetRenderObject(new RenderObject(&capsule->GetTransform(), capsuleMesh, basicTex, basicShader));
    PhysicsObject* phyObj = capsule->AddComponent<PhysicsObject>();

    phyObj->SetInverseMass(inverseMass);
    phyObj->InitCubeInertia();

    world->AddGameObject(capsule);

    return capsule;
}

GameObject* TutorialGame::AddCubeToWorld(const Vector3& position, Vector3 dimensions, float inverseMass)
{
    GameObject* cube = new GameObject("cube");

    AABBVolume* volume = cube->AddComponent<AABBVolume>([&](Component* component) {dynamic_cast<AABBVolume*>(component)->SetHalfDimensions(dimensions); });
    cube->GetTransform()
        .SetPosition(position)
        .SetScale(dimensions * 2);

    cube->SetRenderObject(new RenderObject(&cube->GetTransform(), cubeMesh, basicTex, basicShader));
    PhysicsObject* phyObj = cube->AddComponent<PhysicsObject>();

    phyObj->SetInverseMass(inverseMass);
    phyObj->InitCubeInertia();

    world->AddGameObject(cube);

    return cube;
}

void TutorialGame::InitSphereGridWorld(int numRows, int numCols, float rowSpacing, float colSpacing, float radius)
{
    for (int x = 0; x < numCols; ++x)
    {
        for (int z = 0; z < numRows; ++z)
        {
            Vector3 position = Vector3(x * colSpacing, 10.0f, z * rowSpacing);
            AddSphereToWorld(position, radius, 1.0f);
        }
    }
    AddFloorToWorld(Vector3(0, -2, 0));
}

void TutorialGame::InitMixedGridWorld(int numRows, int numCols, float rowSpacing, float colSpacing)
{
    float sphereRadius = 1.0f;
    Vector3 cubeDims = Vector3(1, 1, 1);

    for (int x = 0; x < numCols; ++x)
    {
        for (int z = 0; z < numRows; ++z)
        {
            Vector3 position = Vector3(x * colSpacing, 10.0f, z * rowSpacing);

            if (rand() % 2)
            {
                AddCubeToWorld(position, cubeDims);
            }
            else
            {
                AddSphereToWorld(position, sphereRadius);
            }
        }
    }
}

void TutorialGame::InitCubeGridWorld(int numRows, int numCols, float rowSpacing, float colSpacing, const Vector3& cubeDims)
{
    for (int x = 1; x < numCols + 1; ++x)
    {
        for (int z = 1; z < numRows + 1; ++z)
        {
            Vector3 position = Vector3(x * colSpacing, 10.0f, z * rowSpacing);
            AddCubeToWorld(position, cubeDims, 1.0f);
        }
    }
}

void TutorialGame::InitDefaultFloor()
{
    AddFloorToWorld(Vector3(0, -2, 0));
}

void TutorialGame::InitGameExamples()
{
    AddPlayerToWorld(Vector3(0, 5, 0));
    AddEnemyToWorld(Vector3(5, 5, 0));
    AddBonusToWorld(Vector3(10, 5, 0));
}

GameObject* TutorialGame::AddPlayerToWorld(const Vector3& position)
{
    float meshSize = 3.0f;
    float inverseMass = 0.5f;

    GameObject* character = new GameObject("character");
    character->AddComponent<AABBVolume>([&](Component* component) {dynamic_cast<AABBVolume*>(component)->SetHalfDimensions(Vector3(0.3f, 0.85f, 0.3f) * meshSize); });
    character->GetTransform()
        .SetScale(Vector3(meshSize, meshSize, meshSize))
        .SetPosition(position);

    if (rand() % 2)
    {
        character->SetRenderObject(new RenderObject(&character->GetTransform(), charMeshA, nullptr, basicShader));
    }
    else
    {
        character->SetRenderObject(new RenderObject(&character->GetTransform(), charMeshB, nullptr, basicShader));
    }
    PhysicsObject* phyObj = character->AddComponent<PhysicsObject>();
    phyObj->SetInverseMass(inverseMass);
    phyObj->InitSphereInertia();

    world->AddGameObject(character);

    // lockedObject = character;

    return character;
}

GameObject* TutorialGame::AddEnemyToWorld(const Vector3& position)
{
    float meshSize = 3.0f;
    float inverseMass = 0.5f;

    GameObject* character = new GameObject("character");

    character->AddComponent<AABBVolume>([&](Component* component) {dynamic_cast<AABBVolume*>(component)->SetHalfDimensions(Vector3(0.3f, 0.9f, 0.3f) * meshSize); });
    character->GetTransform()
        .SetScale(Vector3(meshSize, meshSize, meshSize))
        .SetPosition(position);

    character->SetRenderObject(new RenderObject(&character->GetTransform(), enemyMesh, nullptr, basicShader));
    PhysicsObject* phyObj = character->AddComponent<PhysicsObject>();
    phyObj->SetInverseMass(inverseMass);
    phyObj->InitSphereInertia();

    world->AddGameObject(character);

    return character;
}

GameObject* TutorialGame::AddBonusToWorld(const Vector3& position)
{
    GameObject* apple = new GameObject("apple");

    apple->AddComponent<SphereVolume>([&](Component* component) {dynamic_cast<SphereVolume*>(component)->SetRadius(0.25f); });
    apple->GetTransform()
        .SetScale(Vector3(0.25, 0.25, 0.25))
        .SetPosition(position);

    apple->SetRenderObject(new RenderObject(&apple->GetTransform(), bonusMesh, nullptr, basicShader));
    PhysicsObject* phyObj = apple->AddComponent<PhysicsObject>();
    phyObj->SetInverseMass(1.0f);
    phyObj->InitSphereInertia();

    world->AddGameObject(apple);

    return apple;
}

StateGameObject* NLS::Engine::TutorialGame::AddStateObjectToWorld(const Vector3& position)
{
    StateGameObject* apple = new StateGameObject("apple");

    apple->AddComponent<SphereVolume>([&](Component* component) {dynamic_cast<SphereVolume*>(component)->SetRadius(0.25f); });
    apple->GetTransform()
        .SetScale(Vector3(0.25, 0.25, 0.25))
        .SetPosition(position);

    apple->SetRenderObject(new RenderObject(&apple->GetTransform(), bonusMesh, nullptr, basicShader));
    PhysicsObject* phyObj = apple->AddComponent<PhysicsObject>();
    phyObj->SetInverseMass(1.0f);
    phyObj->InitSphereInertia();

    world->AddGameObject(apple);

    return apple;
}

/*

Every frame, this code will let you perform a raycast, to see if there's an object
underneath the cursor, and if so 'select it' into a pointer, so that it can be
manipulated later. Pressing Q will let you toggle between this behaviour and instead
letting you move the camera around.

*/
bool TutorialGame::SelectObject()
{
    if (Window::GetKeyboard()->KeyPressed(KeyboardKeys::Q))
    {
        inSelectionMode = !inSelectionMode;
        if (inSelectionMode)
        {
            Window::GetWindow()->ShowOSPointer(true);
            Window::GetWindow()->LockMouseToWindow(false);
        }
        else
        {
            Window::GetWindow()->ShowOSPointer(false);
            Window::GetWindow()->LockMouseToWindow(true);
        }
    }
    if (inSelectionMode)
    {
        renderer->DrawStringGray("Press Q to change to camera mode!", Vector2(5, 85));

        if (Window::GetMouse()->ButtonDown(NLS::MouseButtons::LEFT))
        {
            if (selectionObject)
            { // set colour to deselected;
                selectionObject->GetRenderObject()->SetColour(Vector4(1, 1, 1, 1));
                selectionObject = nullptr;
                lockedObject = nullptr;
            }

            Ray ray = CollisionDetection::BuildRayFromMouse(*world->GetMainCamera());

            RayCollision closestCollision;
            if (world->Raycast(ray, closestCollision, true))
            {
                selectionObject = (GameObject*)closestCollision.node;
                selectionObject->GetRenderObject()->SetColour(Vector4(0, 1, 0, 1));
                return true;
            }
            else
            {
                return false;
            }
        }
    }
    else
    {
        renderer->DrawStringGray("Press Q to change to select mode!", Vector2(5, 85));
    }

    if (lockedObject)
    {
        renderer->DrawStringGray("Press L to unlock object!", Vector2(5, 80));
    }

    else if (selectionObject)
    {
        renderer->DrawStringGray("Press L to lock selected object object!", Vector2(5, 80));
    }

    if (Window::GetKeyboard()->KeyPressed(NLS::KeyboardKeys::L))
    {
        if (selectionObject)
        {
            if (lockedObject == selectionObject)
            {
                lockedObject = nullptr;
            }
            else
            {
                lockedObject = selectionObject;
            }
        }
    }

    return false;
}

/*
If an object has been clicked, it can be pushed with the right mouse button, by an amount
determined by the scroll wheel. In the first tutorial this won't do anything, as we haven't
added linear motion into our physics system. After the second tutorial, objects will move in a straight
line - after the third, they'll be able to twist under torque aswell.
*/
void TutorialGame::MoveSelectedObject()
{
    renderer->DrawStringGray(" Click Force :" + std::to_string(forceMagnitude), Vector2(10, 20)); // Draw debug text at 10 ,20
    forceMagnitude += Window::GetMouse()->GetWheelMovement() * 100.0f;
    if (!selectionObject)
    {
        return; // we haven ’t selected anything !
    }
    // Push the selected object !
    if (Window::GetMouse()->ButtonPressed(NLS::MouseButtons::RIGHT))
    {
        Ray ray = CollisionDetection::BuildRayFromMouse(*world->GetMainCamera());
        RayCollision closestCollision;
        if (world->Raycast(ray, closestCollision, true))
        {
            if (closestCollision.node == selectionObject)
            {
                selectionObject->GetComponent<PhysicsObject>()->AddForceAtPosition(ray.GetDirection() * forceMagnitude, closestCollision.collidedAt);
            }
        }
    }
}