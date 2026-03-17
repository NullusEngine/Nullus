#include "GameobjectSerialize.h"

#include <algorithm>
#include <any>
#include <memory>
#include <unordered_map>

#include "Core/ResourceManagement/ShaderManager.h"
#include "Core/ResourceManagement/TextureManager.h"
#include "Core/ServiceLocator.h"
#include "Components/Component.h"
#include "Components/CameraComponent.h"
#include "Components/LightComponent.h"
#include "Components/MaterialRenderer.h"
#include "Components/MeshRenderer.h"
#include "Components/TransformComponent.h"
#include "Debug/Logger.h"
#include "ExternalReflection.h"
#include "GameObject.h"
#include "Reflection/Field.h"
#include "Reflection/JsonConfig.h"
#include "Reflection/Variant.h"
#include "SceneSystem/Scene.h"
#include "Serialize/SceneSerializationData.h"
#include "Serialize/Serializer.h"

namespace
{
using namespace NLS;
using namespace NLS::Engine;
using namespace NLS::Engine::Components;
using namespace NLS::Engine::SceneSystem;
using namespace NLS::Engine::Serialize;

json SerializeWithReflection(const meta::Variant &variant)
{
    json output = json::object();
    Serializer::Instance()->SerializeObject(variant, output);
    return output;
}

void DeserializeWithReflection(meta::Variant &variant, const json &input)
{
    Serializer::Instance()->DeserializeObject(variant, input);
}

void CopyMatchingFields(const meta::Variant &source, meta::Variant &destination)
{
    const auto sourceType = source.GetType();
    const auto destinationType = destination.GetType();
    for (const auto &destinationField : destinationType.GetFields())
    {
        const auto &sourceField = sourceType.GetField(destinationField.GetName());
        if (!sourceField.IsValid())
            continue;

        destinationField.SetValue(destination, sourceField.GetValue(source));
    }
}

meta::Type ResolveComponentType(const std::string &typeName)
{
    auto type = meta::Type::GetFromName(typeName);
    if (type.IsValid())
        return type;

    type = meta::Type::GetFromName("NLS::Engine::Components::" + typeName);
    if (type.IsValid())
        return type;

    return meta::Type::Invalid();
}

NLS::Json SerializeComponentPayload(const Component &component)
{
    auto *mutableComponent = const_cast<Component *>(&component);
    meta::Variant componentVariant(mutableComponent, meta::variant_policy::WrapObject { });
    return component.GetType().SerializeJson(componentVariant);
}

Maths::Vector3 ReadVector3(const json &value, const Maths::Vector3 &fallback = {})
{
    if (!value.is_object())
        return fallback;

    return Maths::Vector3(
        value.value("x", fallback.x),
        value.value("y", fallback.y),
        value.value("z", fallback.z));
}

Maths::Vector4 ReadVector4(const json &value, const Maths::Vector4 &fallback = {})
{
    if (!value.is_object())
        return fallback;

    return Maths::Vector4(
        value.value("x", fallback.x),
        value.value("y", fallback.y),
        value.value("z", fallback.z),
        value.value("w", fallback.w));
}

Maths::Quaternion ReadQuaternion(const json &value, const Maths::Quaternion &fallback = Maths::Quaternion::Identity)
{
    if (!value.is_object())
        return fallback;

    return Maths::Quaternion(
        value.value("x", fallback.x),
        value.value("y", fallback.y),
        value.value("z", fallback.z),
        value.value("w", fallback.w));
}

void SetMaterialUniform(Render::Resources::Material &material, const std::string &uniformName, const json &uniformValue)
{
    if (uniformValue.is_null())
        return;

    if (uniformValue.is_number())
    {
        material.Set<float>(uniformName, uniformValue.get<float>());
        return;
    }

    if (uniformValue.is_boolean())
    {
        material.Set<bool>(uniformName, uniformValue.get<bool>());
        return;
    }

    if (uniformValue.is_string())
    {
        const auto texturePath = uniformValue.get<std::string>();
        if (!texturePath.empty())
        {
            auto *texture = NLS_SERVICE(Core::ResourceManagement::TextureManager)[texturePath];
            material.Set<Render::Resources::Texture2D *>(uniformName, texture);
        }
        return;
    }

    if (!uniformValue.is_object())
        return;

    const auto &object = uniformValue;
    const auto typeIt = object.find("type");
    const auto valueIt = object.find("value");
    if (typeIt == object.end() || valueIt == object.end() || !typeIt.value().is_string())
        return;

    const auto kind = typeIt.value().template get<std::string>();
    const auto &value = valueIt.value();

    if (kind == "Vector2" && value.is_object())
    {
        material.Set(uniformName, Maths::Vector2(
            value["x"].get<float>(),
            value["y"].get<float>()));
    }
    else if (kind == "Vector3" && value.is_object())
    {
        material.Set(uniformName, Maths::Vector3(
            value["x"].get<float>(),
            value["y"].get<float>(),
            value["z"].get<float>()));
    }
    else if (kind == "Vector4" && value.is_object())
    {
        material.Set(uniformName, Maths::Vector4(
            value["x"].get<float>(),
            value["y"].get<float>(),
            value["z"].get<float>(),
            value["w"].get<float>()));
    }
    else if (kind == "Texture2D")
    {
        if (value.is_null())
        {
            material.Set<Render::Resources::Texture2D *>(uniformName, nullptr);
        }
        else if (value.is_string())
        {
            const auto texturePath = value.template get<std::string>();
            auto *texture = NLS_SERVICE(Core::ResourceManagement::TextureManager)[texturePath];
            material.Set<Render::Resources::Texture2D *>(uniformName, texture);
        }
    }
}

void ApplyLegacyInlineMaterials(MaterialRenderer &component, const json &componentPayload)
{
    const auto &materialsJson = componentPayload["materials"];
    if (!materialsJson.is_array())
        return;

    static std::vector<std::unique_ptr<Render::Resources::Material>> s_runtimeSceneMaterials;

    component.RemoveAllMaterials();

    size_t materialIndex = 0;
    for (const auto &materialJson : materialsJson)
    {
        if (!materialJson.is_object() || materialIndex >= kMaxMaterialCount)
            break;

        auto *shader = NLS_SERVICE(Core::ResourceManagement::ShaderManager)[materialJson["shader"].get<std::string>()];
        auto runtimeMaterial = std::make_unique<Render::Resources::Material>(shader);

        runtimeMaterial->SetBlendable(materialJson.value("blendable", false));
        runtimeMaterial->SetBackfaceCulling(materialJson.value("backfaceCulling", true));
        runtimeMaterial->SetFrontfaceCulling(materialJson.value("frontfaceCulling", false));
        runtimeMaterial->SetDepthTest(materialJson.value("depthTest", true));
        runtimeMaterial->SetDepthWriting(materialJson.value("depthWriting", true));
        runtimeMaterial->SetColorWriting(materialJson.value("colorWriting", true));
        runtimeMaterial->SetGPUInstances(materialJson.value("gpuInstances", 1));

        const auto &uniforms = materialJson["uniforms"];
        if (uniforms.is_object())
        {
            for (const auto &[uniformName, uniformValue] : uniforms.items())
                SetMaterialUniform(*runtimeMaterial, uniformName, uniformValue);
        }

        component.SetMaterialAtIndex(static_cast<uint8_t>(materialIndex++), *runtimeMaterial);
        s_runtimeSceneMaterials.push_back(std::move(runtimeMaterial));
    }
}

void ApplyLegacyComponentFallbacks(Component &component, const json &payloadJson)
{
    if (!payloadJson.is_object())
        return;

    if (auto *transform = dynamic_cast<TransformComponent *>(&component))
    {
        if (payloadJson.contains("localPosition"))
            transform->SetLocalPosition(ReadVector3(payloadJson["localPosition"], transform->GetLocalPosition()));
        if (payloadJson.contains("localRotation"))
            transform->SetLocalRotation(ReadQuaternion(payloadJson["localRotation"], transform->GetLocalRotation()));
        if (payloadJson.contains("localScale"))
            transform->SetLocalScale(ReadVector3(payloadJson["localScale"], transform->GetLocalScale()));
        return;
    }

    if (auto *camera = dynamic_cast<CameraComponent *>(&component))
    {
        if (payloadJson.contains("fov")) camera->SetFov(payloadJson["fov"].get<float>());
        if (payloadJson.contains("size")) camera->SetSize(payloadJson["size"].get<float>());
        if (payloadJson.contains("near")) camera->SetNear(payloadJson["near"].get<float>());
        if (payloadJson.contains("far")) camera->SetFar(payloadJson["far"].get<float>());
        if (payloadJson.contains("clearColor")) camera->SetClearColor(ReadVector3(payloadJson["clearColor"], camera->GetClearColor()));
        if (payloadJson.contains("frustumGeometryCulling")) camera->SetFrustumGeometryCulling(payloadJson["frustumGeometryCulling"].get<bool>());
        if (payloadJson.contains("frustumLightCulling")) camera->SetFrustumLightCulling(payloadJson["frustumLightCulling"].get<bool>());
        if (payloadJson.contains("projectionMode")) camera->SetProjectionMode(static_cast<Render::Settings::EProjectionMode>(payloadJson["projectionMode"].get<int>()));
        return;
    }

    if (auto *light = dynamic_cast<LightComponent *>(&component))
    {
        if (payloadJson.contains("lightType")) light->SetLightType(static_cast<Render::Settings::ELightType>(payloadJson["lightType"].get<int>()));
        if (payloadJson.contains("color")) light->SetColor(ReadVector3(payloadJson["color"], light->GetColor()));
        if (payloadJson.contains("intensity")) light->SetIntensity(payloadJson["intensity"].get<float>());
        if (payloadJson.contains("constant")) light->SetConstant(payloadJson["constant"].get<float>());
        if (payloadJson.contains("linear")) light->SetLinear(payloadJson["linear"].get<float>());
        if (payloadJson.contains("quadratic")) light->SetQuadratic(payloadJson["quadratic"].get<float>());
        if (payloadJson.contains("cutoff")) light->SetCutoff(payloadJson["cutoff"].get<float>());
        if (payloadJson.contains("outerCutoff")) light->SetOuterCutoff(payloadJson["outerCutoff"].get<float>());
        if (payloadJson.contains("radius")) light->SetRadius(payloadJson["radius"].get<float>());
        if (payloadJson.contains("size")) light->SetSize(ReadVector3(payloadJson["size"], light->GetSize()));
        return;
    }

    if (auto *meshRenderer = dynamic_cast<MeshRenderer *>(&component))
    {
        if (payloadJson.contains("model") && payloadJson["model"].is_string())
            NLS::Engine::Reflection::SetModelPath(*meshRenderer, payloadJson["model"].get<std::string>());
        if (payloadJson.contains("frustumBehaviour"))
            meshRenderer->SetFrustumBehaviour(static_cast<MeshRenderer::EFrustumBehaviour>(payloadJson["frustumBehaviour"].get<int>()));
        if (payloadJson.contains("customBoundingSphere"))
        {
            Render::Geometry::BoundingSphere sphere = meshRenderer->GetCustomBoundingSphere();
            sphere.position = ReadVector3(payloadJson["customBoundingSphere"]["position"], sphere.position);
            sphere.radius = payloadJson["customBoundingSphere"].value("radius", sphere.radius);
            meshRenderer->SetCustomBoundingSphere(sphere);
        }
        return;
    }

    if (auto *materialRenderer = dynamic_cast<MaterialRenderer *>(&component))
    {
        if (payloadJson.contains("userMatrix") && payloadJson["userMatrix"].is_array())
        {
            uint32_t index = 0;
            for (const auto &element : payloadJson["userMatrix"])
            {
                if (index >= 16)
                    break;
                materialRenderer->SetUserMatrixElement(index / 4, index % 4, element.get<float>());
                ++index;
            }
        }

        if (payloadJson.contains("materials") && payloadJson["materials"].is_array() && !payloadJson["materials"].empty())
        {
            if (payloadJson["materials"][0].is_object())
                ApplyLegacyInlineMaterials(*materialRenderer, payloadJson);
            else
            {
                NLS::Array<std::string> paths;
                for (const auto &entry : payloadJson["materials"])
                    paths.push_back(entry.is_string() ? entry.get<std::string>() : std::string {});
                NLS::Engine::Reflection::SetMaterialPaths(*materialRenderer, paths);
            }
        }
    }
}

SerializedComponentData SerializeComponentRecord(const Component &component)
{
    SerializedComponentData record;
    record.type = component.GetType().GetName();
    record.data = SerializeComponentPayload(component).dump();
    return record;
}

NLS::Json GetComponentPayloadJson(const SerializedComponentData &record)
{
    if (record.data.empty())
        return NLS::Json::object { };

    std::string parseError;
    const auto parsed = NLS::Json::parse(record.data, parseError, json11::JsonParse::STANDARD);
    if (!parseError.empty())
        return NLS::Json::object { };

    return parsed;
}

Component *FindOrCreateComponent(GameObject &gameObject, const meta::Type &type)
{
    if (!type.IsValid() || !type.DerivesFrom(typeof(Component)))
        return nullptr;

    if (type == typeof(TransformComponent))
        return gameObject.GetTransform();

    if (auto *existing = gameObject.GetComponent(type, false))
        return existing;

    return gameObject.AddComponent(type);
}

SerializedActorData SerializeActorRecord(const GameObject &actor)
{
    auto *mutableActor = const_cast<GameObject *>(&actor);
    meta::Variant actorVariant(mutableActor, meta::variant_policy::WrapObject { });
    SerializedActorData record;
    meta::Variant recordVariant(record, meta::variant_policy::NoCopy { });
    CopyMatchingFields(actorVariant, recordVariant);
    record.parent = actor.GetParentID();

    for (const auto &component : actor.GetComponents())
    {
        if (component)
            record.components.push_back(SerializeComponentRecord(*component));
    }
    return record;
}

void DeserializeComponentRecord(Component &component, const SerializedComponentData &record)
{
    auto *mutableComponent = &component;
    meta::Variant componentVariant(mutableComponent, meta::variant_policy::WrapObject { });
    const auto payload = GetComponentPayloadJson(record);
    component.GetType().DeserializeJson(componentVariant, payload);
    const auto payloadJson = json::parse(payload.dump(), nullptr, false);
    ApplyLegacyComponentFallbacks(component, payloadJson);

    if (auto *materialRenderer = dynamic_cast<MaterialRenderer *>(&component))
    {
        if (payloadJson.is_object() && payloadJson["materials"].is_array() && !payloadJson["materials"].empty() && payloadJson["materials"][0].is_object())
            ApplyLegacyInlineMaterials(*materialRenderer, payloadJson);
    }
}

void DeserializeActor(GameObject &actor, const SerializedActorData &record)
{
    auto *mutableActor = &actor;
    meta::Variant actorVariant(mutableActor, meta::variant_policy::WrapObject { });
    meta::Variant recordVariant(const_cast<SerializedActorData &>(record), meta::variant_policy::NoCopy { });
    CopyMatchingFields(recordVariant, actorVariant);

    for (const auto &componentRecord : record.components)
    {
        const auto componentType = ResolveComponentType(componentRecord.type);
        auto *component = FindOrCreateComponent(actor, componentType);
        if (!component)
        {
            NLS_LOG_WARNING("Skipping unsupported component during scene load: " + componentRecord.type);
            continue;
        }

        DeserializeComponentRecord(*component, componentRecord);
    }
}

void SerializeSceneImpl(const Scene &scene, json &output)
{
    json actorsJson = json::array();
    for (const auto *actor : scene.GetActors())
    {
        if (actor)
        {
            const auto record = SerializeActorRecord(*actor);
            json componentsJson = json::array();
            for (const auto &componentRecord : record.components)
            {
                componentsJson.push_back(json::object({
                    {"type", componentRecord.type},
                    {"data", json::parse(GetComponentPayloadJson(componentRecord).dump())}
                }));
            }

            actorsJson.push_back(json::object({
                {"name", record.name},
                {"tag", record.tag},
                {"active", record.active},
                {"id", record.worldID},
                {"worldID", record.worldID},
                {"parent", record.parent},
                {"components", componentsJson}
            }));
        }
    }
    output = json::object({
        {"version", 1},
        {"actors", actorsJson}
    });
}

void DeserializeSceneImpl(Scene &scene, const json &input)
{
    SerializedSceneData sceneData;

    const auto &actorsJson = input["actors"];
    if (actorsJson.is_array())
    {
        for (const auto &actorJson : actorsJson)
        {
            if (!actorJson.is_object())
                continue;

            SerializedActorData actorRecord;
            actorRecord.name = actorJson.value("name", std::string {});
            actorRecord.tag = actorJson.value("tag", std::string {});
            actorRecord.active = actorJson.contains("active") ? actorJson["active"].get<bool>() : true;
            actorRecord.worldID = actorJson.contains("worldID") ? actorJson["worldID"].get<int>() : actorJson.value("id", 0);
            actorRecord.parent = actorJson.value("parent", 0LL);

            const auto &componentsJson = actorJson["components"];
            if (componentsJson.is_array())
            {
                for (const auto &componentJson : componentsJson)
                {
                    if (!componentJson.is_object())
                        continue;

                    SerializedComponentData componentRecord;
                    componentRecord.type = componentJson.value("type", std::string {});
                    if (componentJson["data"].is_string())
                        componentRecord.data = componentJson["data"].get<std::string>();
                    else
                        componentRecord.data = componentJson["data"].dump();

                    actorRecord.components.push_back(componentRecord);
                }
            }

            sceneData.actors.push_back(actorRecord);
        }
    }

    auto &actors = scene.GetActors();
    for (auto *actor : actors)
        delete actor;
    actors.clear();
    scene.SetAvailableID(1);

    std::unordered_map<int64_t, GameObject *> actorById;
    int64_t nextAvailableID = 1;

    for (const auto &actorRecord : sceneData.actors)
    {
        auto &actor = scene.CreateGameObject(
            actorRecord.name.empty() ? std::string("GameObject") : actorRecord.name,
            actorRecord.tag);

        DeserializeActor(actor, actorRecord);
        actorById[actor.GetWorldID()] = &actor;
        nextAvailableID = std::max(nextAvailableID, static_cast<int64_t>(actor.GetWorldID() + 1));
    }

    for (const auto &actorRecord : sceneData.actors)
    {
        const auto actorId = static_cast<int64_t>(actorRecord.worldID);
        const auto parentId = actorRecord.parent;

        if (parentId == 0)
            continue;

        auto actorIt = actorById.find(actorId);
        auto parentIt = actorById.find(parentId);
        if (actorIt != actorById.end() && parentIt != actorById.end())
            actorIt->second->SetParent(*parentIt->second);
    }

    scene.SetAvailableID(nextAvailableID);
}
} // namespace

namespace NLS
{
void GameObjectSerializeHandler::SerializeImpl(const meta::Variant &obj, json &output) const
{
    const auto record = SerializeActorRecord(obj.GetValue<GameObject>());
    meta::Variant recordVariant(const_cast<SerializedActorData &>(record), meta::variant_policy::NoCopy { });
    output = SerializeWithReflection(recordVariant);
}

void GameObjectSerializeHandler::DeserializeImpl(meta::Variant &obj, const json &input) const
{
    SerializedActorData record;
    meta::Variant recordVariant(record, meta::variant_policy::NoCopy { });
    DeserializeWithReflection(recordVariant, input);
    DeserializeActor(obj.GetValue<GameObject>(), record);
}

uint32_t GameObjectSerializeHandler::CalcMatchLevel(const meta::Type &type, bool isPointer) const
{
    if (!isPointer && type == typeof(Engine::GameObject))
        return 0;
    return NoMatch;
}

void SceneSerializeHandler::SerializeImpl(const meta::Variant &obj, json &output) const
{
    SerializeSceneImpl(obj.GetValue<Engine::SceneSystem::Scene>(), output);
}

void SceneSerializeHandler::DeserializeImpl(meta::Variant &obj, const json &input) const
{
    DeserializeSceneImpl(obj.GetValue<Engine::SceneSystem::Scene>(), input);
}

uint32_t SceneSerializeHandler::CalcMatchLevel(const meta::Type &type, bool isPointer) const
{
    if (!isPointer && type == typeof(Engine::SceneSystem::Scene))
        return 0;
    return NoMatch;
}
} // namespace NLS
