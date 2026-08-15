#include "Rendering/MetalViewportRenderer.h"

#import <Metal/Metal.h>

#include <algorithm>
#include <any>
#include <chrono>
#include <cstring>
#include <string>
#include <utility>

#include "Debug/Logger.h"
#include "Components/LightComponent.h"
#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "Components/TransformComponent.h"
#include "GameObject.h"
#include "Math/Matrix4.h"
#include "Math/Vector4.h"
#include "SceneSystem/Scene.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Entities/Camera.h"
#include "Rendering/Entities/Light.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/RHI/Core/RHIMesh.h"
#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/Mesh.h"
#include "Rendering/Resources/Texture2D.h"
#include "Rendering/Settings/ELightType.h"

namespace NLS::Editor::Rendering
{
    namespace
    {
        struct MetalViewportUniforms
        {
            float viewportWidth = 1.0f;
            float viewportHeight = 1.0f;
            float cameraX = 0.0f;
            float cameraY = 0.0f;
            float cameraZ = 0.0f;
            float forwardX = 0.0f;
            float forwardY = 0.0f;
            float forwardZ = 1.0f;
            float rightX = 1.0f;
            float rightY = 0.0f;
            float rightZ = 0.0f;
            float upX = 0.0f;
            float upY = 1.0f;
            float upZ = 0.0f;
            float fovDegrees = 60.0f;
            float orthographic = 0.0f;
            float cameraSize = 5.0f;
            float nearClip = 0.01f;
            float farClip = 1000.0f;
            float elapsedSeconds = 0.0f;
            float viewKind = 0.0f;
        };

        struct MetalMeshUniforms
        {
            float model[16] = {};
            float viewportWidth = 1.0f;
            float viewportHeight = 1.0f;
            float cameraX = 0.0f;
            float cameraY = 0.0f;
            float cameraZ = 0.0f;
            float forwardX = 0.0f;
            float forwardY = 0.0f;
            float forwardZ = 1.0f;
            float rightX = 1.0f;
            float rightY = 0.0f;
            float rightZ = 0.0f;
            float upX = 0.0f;
            float upY = 1.0f;
            float upZ = 0.0f;
            float fovDegrees = 60.0f;
            float orthographic = 0.0f;
            float cameraSize = 5.0f;
            float nearClip = 0.01f;
            float farClip = 1000.0f;
            float baseColorR = 0.28f;
            float baseColorG = 0.58f;
            float baseColorB = 0.92f;
            float baseColorA = 1.0f;
            float lightDirectionX = -0.35f;
            float lightDirectionY = 0.8f;
            float lightDirectionZ = 0.45f;
            float lightIntensity = 0.85f;
            float lightColorR = 1.0f;
            float lightColorG = 1.0f;
            float lightColorB = 1.0f;
            float useBaseTexture = 0.0f;
            float metallic = 0.0f;
            float roughness = 1.0f;
            float ambientOcclusion = 1.0f;
            float emissiveR = 0.0f;
            float emissiveG = 0.0f;
            float emissiveB = 0.0f;
        };

        const std::any* FindMaterialParameter(
            const NLS::Render::Resources::Material& material,
            const char* const* names,
            const size_t nameCount)
        {
            const auto& parameters = material.GetParameterBlock();
            for (size_t index = 0u; index < nameCount; ++index)
            {
                if (const auto* parameter = parameters.TryGet(names[index]); parameter != nullptr)
                    return parameter;
            }
            return nullptr;
        }

        void ApplyMaterialBaseColor(
            const NLS::Render::Resources::Material* material,
            MetalMeshUniforms& uniforms)
        {
            if (material == nullptr)
                return;

            static constexpr const char* kBaseColorNames[] = {
                "_BaseColor",
                "u_Albedo",
                "u_Diffuse",
                "BaseColor",
                "Albedo",
                "Diffuse"
            };
            const auto* parameter = FindMaterialParameter(
                *material,
                kBaseColorNames,
                std::size(kBaseColorNames));
            if (parameter == nullptr || parameter->type() != typeid(NLS::Maths::Vector4))
                return;

            const auto& color = std::any_cast<const NLS::Maths::Vector4&>(*parameter);
            uniforms.baseColorR = color.x;
            uniforms.baseColorG = color.y;
            uniforms.baseColorB = color.z;
            uniforms.baseColorA = color.w;
        }

        void ApplyMaterialFloat(
            const NLS::Render::Resources::Material* material,
            const char* const* names,
            const size_t nameCount,
            float& value)
        {
            if (material == nullptr)
                return;

            const auto* parameter = FindMaterialParameter(*material, names, nameCount);
            if (parameter != nullptr && parameter->type() == typeid(float))
                value = std::any_cast<float>(*parameter);
        }

        void ApplyMaterialPbrParameters(
            const NLS::Render::Resources::Material* material,
            MetalMeshUniforms& uniforms)
        {
            static constexpr const char* kMetallicNames[] = { "_Metallic", "u_Metallic", "Metallic" };
            static constexpr const char* kRoughnessNames[] = { "_Roughness", "u_Roughness", "Roughness" };
            static constexpr const char* kAmbientOcclusionNames[] = {
                "_AmbientOcclusion",
                "u_AmbientOcclusion",
                "AmbientOcclusion"
            };
            ApplyMaterialFloat(material, kMetallicNames, std::size(kMetallicNames), uniforms.metallic);
            ApplyMaterialFloat(material, kRoughnessNames, std::size(kRoughnessNames), uniforms.roughness);
            ApplyMaterialFloat(
                material,
                kAmbientOcclusionNames,
                std::size(kAmbientOcclusionNames),
                uniforms.ambientOcclusion);

            if (material == nullptr)
                return;
            static constexpr const char* kEmissiveNames[] = {
                "_EmissiveColor",
                "u_Emissive",
                "EmissiveColor",
                "Emissive"
            };
            const auto* emissive = FindMaterialParameter(
                *material,
                kEmissiveNames,
                std::size(kEmissiveNames));
            if (emissive != nullptr && emissive->type() == typeid(NLS::Maths::Vector4))
            {
                const auto& color = std::any_cast<const NLS::Maths::Vector4&>(*emissive);
                uniforms.emissiveR = color.x;
                uniforms.emissiveG = color.y;
                uniforms.emissiveB = color.z;
            }
        }

        id<MTLTexture> ResolveMaterialBaseTexture(const NLS::Render::Resources::Material* material)
        {
            if (material == nullptr)
                return nil;

            static constexpr const char* kBaseTextureNames[] = {
                "_BaseMap",
                "u_AlbedoMap",
                "u_DiffuseMap",
                "BaseColorTexture",
                "BaseColorMap",
                "DiffuseTexture",
                "DiffuseMap",
                "AlbedoTexture",
                "AlbedoMap"
            };
            const auto* parameter = FindMaterialParameter(
                *material,
                kBaseTextureNames,
                std::size(kBaseTextureNames));
            if (parameter == nullptr ||
                parameter->type() != typeid(NLS::Render::Resources::Texture2D*))
            {
                return nil;
            }

            const auto* texture = std::any_cast<NLS::Render::Resources::Texture2D*>(*parameter);
            if (texture == nullptr || texture->GetTextureHandle() == nullptr)
                return nil;

            const auto nativeTexture = texture->GetTextureHandle()->GetNativeImageHandle();
            if (nativeTexture.backend != NLS::Render::RHI::BackendType::Metal ||
                nativeTexture.handle == nullptr)
            {
                return nil;
            }
            return (__bridge id<MTLTexture>)nativeTexture.handle;
        }

        constexpr const char* kMetalViewportShaderSource = R"METAL(
            #include <metal_stdlib>
            using namespace metal;

            struct VertexOutput
            {
                float4 position [[position]];
                float2 uv;
            };

            struct MeshVertex
            {
                float3 position [[attribute(0)]];
                float2 uv [[attribute(1)]];
                float3 normal [[attribute(2)]];
            };

            struct MeshVertexOutput
            {
                float4 position [[position]];
                float2 uv;
                float3 normal;
                float3 worldPosition;
            };

            struct ViewportUniforms
            {
                float viewportWidth;
                float viewportHeight;
                float cameraX;
                float cameraY;
                float cameraZ;
                float forwardX;
                float forwardY;
                float forwardZ;
                float rightX;
                float rightY;
                float rightZ;
                float upX;
                float upY;
                float upZ;
                float fovDegrees;
                float orthographic;
                float cameraSize;
                float nearClip;
                float farClip;
                float elapsedSeconds;
                float viewKind;
            };

            struct MeshUniforms
            {
                float model[16];
                float viewportWidth;
                float viewportHeight;
                float cameraX;
                float cameraY;
                float cameraZ;
                float forwardX;
                float forwardY;
                float forwardZ;
                float rightX;
                float rightY;
                float rightZ;
                float upX;
                float upY;
                float upZ;
                float fovDegrees;
                float orthographic;
                float cameraSize;
                float nearClip;
                float farClip;
                float baseColorR;
                float baseColorG;
                float baseColorB;
                float baseColorA;
                float lightDirectionX;
                float lightDirectionY;
                float lightDirectionZ;
                float lightIntensity;
                float lightColorR;
                float lightColorG;
                float lightColorB;
                float useBaseTexture;
                float metallic;
                float roughness;
                float ambientOcclusion;
                float emissiveR;
                float emissiveG;
                float emissiveB;
            };

            float4 MultiplyRowMajorMatrix(constant float* matrix, float4 value)
            {
                return float4(
                    dot(float4(matrix[0], matrix[1], matrix[2], matrix[3]), value),
                    dot(float4(matrix[4], matrix[5], matrix[6], matrix[7]), value),
                    dot(float4(matrix[8], matrix[9], matrix[10], matrix[11]), value),
                    dot(float4(matrix[12], matrix[13], matrix[14], matrix[15]), value));
            }

            float3 TransformRowMajorDirection(constant float* matrix, float3 direction)
            {
                return float3(
                    dot(float3(matrix[0], matrix[1], matrix[2]), direction),
                    dot(float3(matrix[4], matrix[5], matrix[6]), direction),
                    dot(float3(matrix[8], matrix[9], matrix[10]), direction));
            }

            float3 SafeNormalize(float3 value, float3 fallback)
            {
                const float lengthSquared = dot(value, value);
                return lengthSquared > 1.0e-8 ? value * rsqrt(lengthSquared) : fallback;
            }

            float DistributionGGX(float ndoth, float perceptualRoughness)
            {
                constexpr float kPi = 3.14159265359;
                const float alpha = perceptualRoughness * perceptualRoughness;
                const float alphaSquared = alpha * alpha;
                const float denominatorTerm = ndoth * ndoth * (alphaSquared - 1.0) + 1.0;
                return alphaSquared / max(kPi * denominatorTerm * denominatorTerm, 1.0e-12);
            }

            float GeometrySchlickGGX(float ndotDirection, float perceptualRoughness)
            {
                const float remappedRoughness = perceptualRoughness + 1.0;
                const float k = remappedRoughness * remappedRoughness * 0.125;
                return ndotDirection / max(ndotDirection * (1.0 - k) + k, 1.0e-6);
            }

            float3 FresnelSchlick(float viewDotHalf, float3 f0)
            {
                const float factor = pow(1.0 - saturate(viewDotHalf), 5.0);
                return f0 + (1.0 - f0) * factor;
            }

            float3 ToneMapACES(float3 hdrColor)
            {
                hdrColor = max(hdrColor, 0.0);
                const float peakChannel = max(hdrColor.r, max(hdrColor.g, hdrColor.b));
                if (peakChannel > 1.0)
                {
                    const float shoulderExcess = peakChannel - 1.0;
                    const float compressedPeak = 1.0 + shoulderExcess / (1.0 + shoulderExcess);
                    hdrColor *= compressedPeak / peakChannel;
                }
                return saturate(
                    (hdrColor * (2.51 * hdrColor + 0.03)) /
                    max(hdrColor * (2.43 * hdrColor + 0.59) + 0.14, 1.0e-6));
            }

            vertex VertexOutput viewport_vertex(uint vertexId [[vertex_id]])
            {
                constexpr float2 positions[3] = {
                    float2(-1.0, -1.0),
                    float2( 3.0, -1.0),
                    float2(-1.0,  3.0)
                };

                VertexOutput output;
                output.position = float4(positions[vertexId], 0.0, 1.0);
                output.uv = positions[vertexId] * 0.5 + 0.5;
                return output;
            }

            fragment float4 viewport_fragment(
                VertexOutput input [[stage_in]],
                constant ViewportUniforms& uniforms [[buffer(0)]])
            {
                const float aspect = max(uniforms.viewportWidth / max(uniforms.viewportHeight, 1.0), 0.001);
                const float2 ndc = input.uv * 2.0 - 1.0;

                // Game view has no scene camera in this fallback path. Preserve its
                // simple preview while Scene View uses a camera-correct ground plane.
                if (uniforms.viewKind > 0.5)
                {
                    const float2 plane = float2(ndc.x * aspect, ndc.y);
                    const float vignette = smoothstep(1.15, 0.1, length(plane));
                    float3 color = float3(0.045, 0.105, 0.075);
                    color += 0.018 * sin(uniforms.elapsedSeconds + plane.xyx * float3(1.0, 1.7, 2.3));
                    return float4(color * vignette, 1.0);
                }

                const float3 cameraPosition = float3(uniforms.cameraX, uniforms.cameraY, uniforms.cameraZ);
                const float3 cameraForward = normalize(float3(uniforms.forwardX, uniforms.forwardY, uniforms.forwardZ));
                const float3 cameraRight = normalize(float3(uniforms.rightX, uniforms.rightY, uniforms.rightZ));
                const float3 cameraUp = normalize(float3(uniforms.upX, uniforms.upY, uniforms.upZ));

                float3 rayOrigin = cameraPosition;
                float3 rayDirection = cameraForward;
                if (uniforms.orthographic < 0.5)
                {
                    constexpr float kDegreesToRadians = 0.01745329252;
                    const float tanHalfFov = tan(clamp(uniforms.fovDegrees, 1.0, 179.0) * kDegreesToRadians * 0.5);
                    rayDirection = normalize(
                        cameraForward +
                        cameraRight * (ndc.x * tanHalfFov * aspect) +
                        cameraUp * (ndc.y * tanHalfFov));
                }
                else
                {
                    const float halfHeight = max(uniforms.cameraSize, 0.001);
                    rayOrigin += cameraRight * (ndc.x * halfHeight * aspect) + cameraUp * (ndc.y * halfHeight);
                }

                const float denominator = rayDirection.y;
                const bool hitsGround = abs(denominator) > 0.0001;
                const float groundDistance = hitsGround ? -rayOrigin.y / denominator : -1.0;
                const bool validGroundHit = hitsGround && groundDistance > uniforms.nearClip && groundDistance < uniforms.farClip;

                const float3 skyTop = float3(0.018, 0.032, 0.050);
                const float3 skyBottom = float3(0.055, 0.090, 0.125);
                float3 color = mix(skyBottom, skyTop, saturate(ndc.y * 0.5 + 0.5));
                if (!validGroundHit)
                    return float4(color, 1.0);

                const float3 hit = rayOrigin + rayDirection * groundDistance;
                const float distanceFade = 1.0 - smoothstep(8.0, uniforms.farClip * 0.65, distance(cameraPosition, hit));
                const float gridScale = max(0.001, pow(max(1.0, distance(cameraPosition, hit)) * 0.02, 0.35));
                const float2 gridPosition = hit.xz / gridScale;
                const float2 cell = abs(fract(gridPosition) - 0.5);
                const float lineWidth = 0.035;
                const float grid = 1.0 - smoothstep(lineWidth, lineWidth + 0.02, min(cell.x, cell.y));
                const float2 majorCell = abs(fract(gridPosition / 10.0) - 0.5);
                const float majorGrid = 1.0 - smoothstep(0.43, 0.5, max(majorCell.x, majorCell.y));
                const float axisWidth = max(0.018, gridScale * 0.028);
                const float xAxis = 1.0 - smoothstep(axisWidth, axisWidth * 1.8, abs(hit.z));
                const float zAxis = 1.0 - smoothstep(axisWidth, axisWidth * 1.8, abs(hit.x));

                color = float3(0.035, 0.060, 0.082);
                color += grid * float3(0.035, 0.070, 0.095);
                color += majorGrid * float3(0.045, 0.085, 0.105);
                color = mix(color, float3(0.75, 0.16, 0.08), xAxis);
                color = mix(color, float3(0.08, 0.32, 0.78), zAxis * (1.0 - xAxis));
                color = mix(skyBottom, color, saturate(distanceFade));
                return float4(color, 1.0);
            }

            vertex MeshVertexOutput mesh_vertex(
                MeshVertex input [[stage_in]],
                constant MeshUniforms& uniforms [[buffer(1)]])
            {
                const float3 cameraPosition = float3(uniforms.cameraX, uniforms.cameraY, uniforms.cameraZ);
                const float3 cameraForward = normalize(float3(uniforms.forwardX, uniforms.forwardY, uniforms.forwardZ));
                const float3 cameraRight = normalize(float3(uniforms.rightX, uniforms.rightY, uniforms.rightZ));
                const float3 cameraUp = normalize(float3(uniforms.upX, uniforms.upY, uniforms.upZ));
                const float4 worldPosition = MultiplyRowMajorMatrix(uniforms.model, float4(input.position, 1.0));
                const float3 relative = worldPosition.xyz - cameraPosition;
                const float viewX = dot(relative, cameraRight);
                const float viewY = dot(relative, cameraUp);
                const float viewZ = dot(relative, cameraForward);
                const float aspect = max(uniforms.viewportWidth / max(uniforms.viewportHeight, 1.0), 0.001);

                MeshVertexOutput output;
                if (uniforms.orthographic < 0.5)
                {
                    constexpr float kDegreesToRadians = 0.01745329252;
                    const float tanHalfFov = tan(clamp(uniforms.fovDegrees, 1.0, 179.0) * kDegreesToRadians * 0.5);
                    const float clipRange = max(uniforms.farClip - uniforms.nearClip, 0.00001);
                    output.position = float4(
                        viewX / max(tanHalfFov * aspect, 0.00001),
                        viewY / max(tanHalfFov, 0.00001),
                        viewZ * uniforms.farClip / clipRange -
                            uniforms.nearClip * uniforms.farClip / clipRange,
                        viewZ);
                }
                else
                {
                    output.position = float4(
                        viewX / max(uniforms.cameraSize * aspect, 0.00001),
                        viewY / max(uniforms.cameraSize, 0.00001),
                        (viewZ - uniforms.nearClip) /
                            max(uniforms.farClip - uniforms.nearClip, 0.00001),
                        1.0);
                }

                output.uv = input.uv;
                output.normal = normalize(TransformRowMajorDirection(uniforms.model, input.normal));
                output.worldPosition = worldPosition.xyz;
                return output;
            }

            fragment float4 mesh_fragment(
                MeshVertexOutput input [[stage_in]],
                constant MeshUniforms& uniforms [[buffer(1)]],
                texture2d<float> baseTexture [[texture(0)]],
                sampler baseSampler [[sampler(0)]],
                bool frontFacing [[front_facing]])
            {
                constexpr float kPi = 3.14159265359;
                const float3 lightDirection = SafeNormalize(float3(
                    uniforms.lightDirectionX,
                    uniforms.lightDirectionY,
                    uniforms.lightDirectionZ), float3(0.0, 1.0, 0.0));
                const float3 lightColor = max(float3(
                    uniforms.lightColorR,
                    uniforms.lightColorG,
                    uniforms.lightColorB), 0.0);
                const float3 normal = SafeNormalize(
                    frontFacing ? input.normal : -input.normal,
                    float3(0.0, 1.0, 0.0));
                const float3 cameraPosition = float3(
                    uniforms.cameraX,
                    uniforms.cameraY,
                    uniforms.cameraZ);
                const float3 viewDirection = SafeNormalize(cameraPosition - input.worldPosition, normal);
                const float4 textureColor = uniforms.useBaseTexture > 0.5
                    ? baseTexture.sample(baseSampler, input.uv)
                    : float4(1.0);
                const float4 baseColor = textureColor * float4(
                    uniforms.baseColorR,
                    uniforms.baseColorG,
                    uniforms.baseColorB,
                    uniforms.baseColorA);
                const float3 albedo = saturate(baseColor.rgb);
                const float metallic = saturate(uniforms.metallic);
                const float roughness = clamp(uniforms.roughness, 0.045, 1.0);
                const float ambientOcclusion = saturate(uniforms.ambientOcclusion);
                float3 lighting = albedo * (0.18 * ambientOcclusion);

                const float ndotv = saturate(dot(normal, viewDirection));
                const float ndotl = saturate(dot(normal, lightDirection));
                if (ndotv > 0.0 && ndotl > 0.0)
                {
                    const float3 halfVector = SafeNormalize(lightDirection + viewDirection, normal);
                    const float ndoth = saturate(dot(normal, halfVector));
                    const float viewDotHalf = saturate(dot(viewDirection, halfVector));
                    const float3 f0 = mix(float3(0.04), albedo, metallic);
                    const float3 fresnel = FresnelSchlick(viewDotHalf, f0);
                    const float distribution = DistributionGGX(ndoth, roughness);
                    const float geometry = GeometrySchlickGGX(ndotv, roughness) *
                        GeometrySchlickGGX(ndotl, roughness);
                    const float3 specular = distribution * geometry * fresnel /
                        max(4.0 * ndotv * ndotl, 1.0e-6);
                    const float3 diffuse = (1.0 - fresnel) * (1.0 - metallic) * albedo / kPi;
                    const float3 radiance = lightColor *
                        (max(uniforms.lightIntensity, 0.0) * kPi);
                    lighting += (diffuse + specular) * radiance * ndotl;
                }

                const float3 emissive = max(float3(
                    uniforms.emissiveR,
                    uniforms.emissiveG,
                    uniforms.emissiveB), 0.0);
                return float4(ToneMapACES(lighting + emissive), saturate(baseColor.a));
            }
        )METAL";

        bool IsGameViewport(const std::string& debugName)
        {
            return debugName.find("Game") != std::string::npos;
        }
    }

    struct MetalViewportRenderer::Impl
    {
        explicit Impl(std::string name)
            : debugName(std::move(name))
            , gameViewport(IsGameViewport(debugName))
        {
        }

        ~Impl()
        {
            [pipeline release];
            [meshPipeline release];
            [depthState release];
            [depthTexture release];
            [baseSampler release];
            [whiteTexture release];
            [queue release];
            [device release];
        }

        id<MTLDevice> device = nil;
        id<MTLCommandQueue> queue = nil;
        id<MTLRenderPipelineState> pipeline = nil;
        id<MTLRenderPipelineState> meshPipeline = nil;
        id<MTLDepthStencilState> depthState = nil;
        id<MTLTexture> depthTexture = nil;
        id<MTLSamplerState> baseSampler = nil;
        id<MTLTexture> whiteTexture = nil;
        std::shared_ptr<NLS::Render::RHI::RHIDevice> rhiDevice;
        std::shared_ptr<NLS::Render::RHI::RHITexture> outputTexture;
        std::shared_ptr<NLS::Render::RHI::RHITextureView> outputView;
        std::string debugName;
        uint16_t width = 0u;
        uint16_t height = 0u;
        bool gameViewport = false;
        bool loggedFirstFrame = false;
        std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();
    };

    MetalViewportRenderer::MetalViewportRenderer(std::unique_ptr<Impl> impl)
        : m_impl(std::move(impl))
    {
    }

    MetalViewportRenderer::~MetalViewportRenderer() = default;

    std::unique_ptr<MetalViewportRenderer> CreateMetalViewportRenderer(
        NLS::Render::Context::Driver& driver,
        std::string debugName)
    {
        const auto nativeInfo = NLS::Render::Context::DriverUIAccess::GetNativeDeviceInfo(driver);
        const auto rhiDevice = NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(driver);
        if (nativeInfo.backend != NLS::Render::RHI::NativeBackendType::Metal ||
            nativeInfo.device == nullptr ||
            nativeInfo.graphicsQueue == nullptr ||
            rhiDevice == nullptr)
        {
            return nullptr;
        }

        auto impl = std::make_unique<MetalViewportRenderer::Impl>(std::move(debugName));
        impl->device = [(__bridge id<MTLDevice>)nativeInfo.device retain];
        impl->queue = [(__bridge id<MTLCommandQueue>)nativeInfo.graphicsQueue retain];
        impl->rhiDevice = rhiDevice;

        NSError* error = nil;
        id<MTLLibrary> library = [impl->device newLibraryWithSource:@(kMetalViewportShaderSource)
            options:nil
            error:&error];
        if (library == nil)
        {
            NLS_LOG_ERROR(
                "MetalViewportRenderer: failed to compile viewport shader: " +
                std::string(error != nil ? error.localizedDescription.UTF8String : "unknown error"));
            return nullptr;
        }

        id<MTLFunction> vertexFunction = [library newFunctionWithName:@"viewport_vertex"];
        id<MTLFunction> fragmentFunction = [library newFunctionWithName:@"viewport_fragment"];
        MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
        descriptor.vertexFunction = vertexFunction;
        descriptor.fragmentFunction = fragmentFunction;
        descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        impl->pipeline = [impl->device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        [descriptor release];
        [fragmentFunction release];
        [vertexFunction release];

        if (impl->pipeline == nil)
        {
            NLS_LOG_ERROR(
                "MetalViewportRenderer: failed to create viewport pipeline: " +
                std::string(error != nil ? error.localizedDescription.UTF8String : "unknown error"));
            return nullptr;
        }

        id<MTLFunction> meshVertexFunction = [library newFunctionWithName:@"mesh_vertex"];
        id<MTLFunction> meshFragmentFunction = [library newFunctionWithName:@"mesh_fragment"];
        MTLRenderPipelineDescriptor* meshDescriptor = [[MTLRenderPipelineDescriptor alloc] init];
        meshDescriptor.vertexFunction = meshVertexFunction;
        meshDescriptor.fragmentFunction = meshFragmentFunction;
        meshDescriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        meshDescriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;

        MTLVertexDescriptor* vertexDescriptor = [[MTLVertexDescriptor alloc] init];
        vertexDescriptor.attributes[0].format = MTLVertexFormatFloat3;
        vertexDescriptor.attributes[0].offset = 0u;
        vertexDescriptor.attributes[0].bufferIndex = 0u;
        vertexDescriptor.attributes[1].format = MTLVertexFormatFloat2;
        vertexDescriptor.attributes[1].offset = sizeof(float) * 3u;
        vertexDescriptor.attributes[1].bufferIndex = 0u;
        vertexDescriptor.attributes[2].format = MTLVertexFormatFloat3;
        vertexDescriptor.attributes[2].offset = sizeof(float) * 5u;
        vertexDescriptor.attributes[2].bufferIndex = 0u;
        vertexDescriptor.layouts[0].stride = sizeof(NLS::Render::Geometry::Vertex);
        vertexDescriptor.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
        meshDescriptor.vertexDescriptor = vertexDescriptor;
        impl->meshPipeline = [impl->device newRenderPipelineStateWithDescriptor:meshDescriptor error:&error];
        [vertexDescriptor release];
        [meshDescriptor release];
        [meshFragmentFunction release];
        [meshVertexFunction release];
        [library release];

        if (impl->meshPipeline == nil)
        {
            NLS_LOG_ERROR(
                "MetalViewportRenderer: failed to create mesh pipeline: " +
                std::string(error != nil ? error.localizedDescription.UTF8String : "unknown error"));
            return nullptr;
        }

        MTLDepthStencilDescriptor* depthDescriptor = [[MTLDepthStencilDescriptor alloc] init];
        depthDescriptor.depthCompareFunction = MTLCompareFunctionLessEqual;
        depthDescriptor.depthWriteEnabled = YES;
        impl->depthState = [impl->device newDepthStencilStateWithDescriptor:depthDescriptor];
        [depthDescriptor release];
        if (impl->depthState == nil)
        {
            NLS_LOG_ERROR("MetalViewportRenderer: failed to create depth state");
            return nullptr;
        }

        MTLSamplerDescriptor* samplerDescriptor = [[MTLSamplerDescriptor alloc] init];
        samplerDescriptor.minFilter = MTLSamplerMinMagFilterLinear;
        samplerDescriptor.magFilter = MTLSamplerMinMagFilterLinear;
        samplerDescriptor.mipFilter = MTLSamplerMipFilterLinear;
        samplerDescriptor.sAddressMode = MTLSamplerAddressModeRepeat;
        samplerDescriptor.tAddressMode = MTLSamplerAddressModeRepeat;
        impl->baseSampler = [impl->device newSamplerStateWithDescriptor:samplerDescriptor];
        [samplerDescriptor release];
        if (impl->baseSampler == nil)
        {
            NLS_LOG_ERROR("MetalViewportRenderer: failed to create base texture sampler");
            return nullptr;
        }

        MTLTextureDescriptor* whiteTextureDescriptor = [[MTLTextureDescriptor alloc] init];
        whiteTextureDescriptor.textureType = MTLTextureType2D;
        whiteTextureDescriptor.pixelFormat = MTLPixelFormatRGBA8Unorm;
        whiteTextureDescriptor.width = 1u;
        whiteTextureDescriptor.height = 1u;
        whiteTextureDescriptor.depth = 1u;
        whiteTextureDescriptor.mipmapLevelCount = 1u;
        whiteTextureDescriptor.storageMode = MTLStorageModeShared;
        whiteTextureDescriptor.usage = MTLTextureUsageShaderRead;
        impl->whiteTexture = [impl->device newTextureWithDescriptor:whiteTextureDescriptor];
        [whiteTextureDescriptor release];
        if (impl->whiteTexture == nil)
        {
            NLS_LOG_ERROR("MetalViewportRenderer: failed to create fallback base texture");
            return nullptr;
        }
        const uint32_t whitePixel = 0xFFFFFFFFu;
        [impl->whiteTexture replaceRegion:MTLRegionMake2D(0u, 0u, 1u, 1u)
                             mipmapLevel:0u
                               withBytes:&whitePixel
                             bytesPerRow:sizeof(whitePixel)];

        NLS_LOG_INFO("MetalViewportRenderer: created native preview renderer for " + impl->debugName);
        return std::unique_ptr<MetalViewportRenderer>(new MetalViewportRenderer(std::move(impl)));
    }

    bool MetalViewportRenderer::Render(
        const uint16_t width,
        const uint16_t height,
        const NLS::Render::Entities::Camera* camera,
        const NLS::Engine::SceneSystem::Scene* scene)
    {
        if (m_impl == nullptr || width == 0u || height == 0u ||
            m_impl->device == nil || m_impl->queue == nil || m_impl->pipeline == nil ||
            m_impl->meshPipeline == nil || m_impl->depthState == nil)
        {
            return false;
        }

        if (m_impl->outputTexture == nullptr || m_impl->width != width || m_impl->height != height)
        {
            NLS::Render::RHI::RHITextureDesc textureDesc{};
            textureDesc.extent = { width, height, 1u };
            textureDesc.dimension = NLS::Render::RHI::TextureDimension::Texture2D;
            textureDesc.format = NLS::Render::RHI::TextureFormat::RGBA8;
            textureDesc.usage = NLS::Render::RHI::TextureUsageFlags::Sampled |
                NLS::Render::RHI::TextureUsageFlags::ColorAttachment;
            textureDesc.debugName = m_impl->debugName + ".MetalPreview";
            m_impl->outputTexture = m_impl->rhiDevice->CreateTexture(textureDesc);
            if (m_impl->outputTexture == nullptr)
                return false;

            NLS::Render::RHI::RHITextureViewDesc viewDesc{};
            viewDesc.viewType = NLS::Render::RHI::TextureViewType::Texture2D;
            viewDesc.format = textureDesc.format;
            viewDesc.debugName = textureDesc.debugName + ".View";
            m_impl->outputView = m_impl->rhiDevice->CreateTextureView(m_impl->outputTexture, viewDesc);
            if (m_impl->outputView == nullptr)
                return false;

            [m_impl->depthTexture release];
            MTLTextureDescriptor* depthDescriptor = [[MTLTextureDescriptor alloc] init];
            depthDescriptor.textureType = MTLTextureType2D;
            depthDescriptor.pixelFormat = MTLPixelFormatDepth32Float;
            depthDescriptor.width = width;
            depthDescriptor.height = height;
            depthDescriptor.depth = 1u;
            depthDescriptor.mipmapLevelCount = 1u;
            depthDescriptor.usage = MTLTextureUsageRenderTarget;
            depthDescriptor.storageMode = MTLStorageModePrivate;
            m_impl->depthTexture = [m_impl->device newTextureWithDescriptor:depthDescriptor];
            [depthDescriptor release];
            if (m_impl->depthTexture == nil)
                return false;

            m_impl->width = width;
            m_impl->height = height;
        }

        const auto nativeTexture = m_impl->outputTexture->GetNativeImageHandle();
        if (nativeTexture.backend != NLS::Render::RHI::BackendType::Metal || nativeTexture.handle == nullptr)
            return false;

        id<MTLTexture> texture = (__bridge id<MTLTexture>)nativeTexture.handle;
        id<MTLCommandBuffer> commandBuffer = [m_impl->queue commandBuffer];
        MTLRenderPassDescriptor* renderPass = [MTLRenderPassDescriptor renderPassDescriptor];
        auto* colorAttachment = renderPass.colorAttachments[0];
        colorAttachment.texture = texture;
        colorAttachment.loadAction = MTLLoadActionClear;
        colorAttachment.storeAction = MTLStoreActionStore;
        colorAttachment.clearColor = MTLClearColorMake(0.02, 0.03, 0.04, 1.0);
        auto* depthAttachment = renderPass.depthAttachment;
        depthAttachment.texture = m_impl->depthTexture;
        depthAttachment.loadAction = MTLLoadActionClear;
        depthAttachment.storeAction = MTLStoreActionDontCare;
        depthAttachment.clearDepth = 1.0;

        id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:renderPass];
        [encoder setRenderPipelineState:m_impl->pipeline];
        const auto elapsedSeconds = std::chrono::duration<float>(
            std::chrono::steady_clock::now() - m_impl->startTime).count();
        MetalViewportUniforms uniforms{};
        uniforms.viewportWidth = static_cast<float>(width);
        uniforms.viewportHeight = static_cast<float>(height);
        uniforms.elapsedSeconds = elapsedSeconds;
        uniforms.viewKind = m_impl->gameViewport ? 1.0f : 0.0f;
        if (camera != nullptr)
        {
            const auto cameraPosition = camera->GetPosition();
            const auto cameraForward = camera->transform != nullptr
                ? camera->transform->GetWorldForward()
                : NLS::Maths::Vector3::Forward;
            const auto cameraRight = camera->transform != nullptr
                ? camera->transform->GetWorldRight()
                : NLS::Maths::Vector3::Right;
            const auto cameraUp = camera->transform != nullptr
                ? camera->transform->GetWorldUp()
                : NLS::Maths::Vector3::Up;
            uniforms.cameraX = cameraPosition.x;
            uniforms.cameraY = cameraPosition.y;
            uniforms.cameraZ = cameraPosition.z;
            uniforms.forwardX = cameraForward.x;
            uniforms.forwardY = cameraForward.y;
            uniforms.forwardZ = cameraForward.z;
            uniforms.rightX = cameraRight.x;
            uniforms.rightY = cameraRight.y;
            uniforms.rightZ = cameraRight.z;
            uniforms.upX = cameraUp.x;
            uniforms.upY = cameraUp.y;
            uniforms.upZ = cameraUp.z;
            uniforms.fovDegrees = camera->GetFov();
            uniforms.orthographic = camera->GetProjectionMode() ==
                NLS::Render::Settings::EProjectionMode::ORTHOGRAPHIC ? 1.0f : 0.0f;
            uniforms.cameraSize = camera->GetSize();
            uniforms.nearClip = camera->GetNear();
            uniforms.farClip = camera->GetFar();
        }
        [encoder setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];

        uint32_t renderedMeshCount = 0u;
        if (scene != nullptr && camera != nullptr)
        {
            [encoder setRenderPipelineState:m_impl->meshPipeline];
            [encoder setDepthStencilState:m_impl->depthState];

            MetalMeshUniforms meshUniforms{};
            const auto cameraPosition = camera->GetPosition();
            const auto cameraForward = camera->transform != nullptr
                ? camera->transform->GetWorldForward()
                : NLS::Maths::Vector3::Forward;
            const auto cameraRight = camera->transform != nullptr
                ? camera->transform->GetWorldRight()
                : NLS::Maths::Vector3::Right;
            const auto cameraUp = camera->transform != nullptr
                ? camera->transform->GetWorldUp()
                : NLS::Maths::Vector3::Up;
            meshUniforms.viewportWidth = static_cast<float>(width);
            meshUniforms.viewportHeight = static_cast<float>(height);
            meshUniforms.cameraX = cameraPosition.x;
            meshUniforms.cameraY = cameraPosition.y;
            meshUniforms.cameraZ = cameraPosition.z;
            meshUniforms.forwardX = cameraForward.x;
            meshUniforms.forwardY = cameraForward.y;
            meshUniforms.forwardZ = cameraForward.z;
            meshUniforms.rightX = cameraRight.x;
            meshUniforms.rightY = cameraRight.y;
            meshUniforms.rightZ = cameraRight.z;
            meshUniforms.upX = cameraUp.x;
            meshUniforms.upY = cameraUp.y;
            meshUniforms.upZ = cameraUp.z;
            meshUniforms.fovDegrees = camera->GetFov();
            meshUniforms.orthographic = camera->GetProjectionMode() ==
                NLS::Render::Settings::EProjectionMode::ORTHOGRAPHIC ? 1.0f : 0.0f;
            meshUniforms.cameraSize = camera->GetSize();
            meshUniforms.nearClip = camera->GetNear();
            meshUniforms.farClip = camera->GetFar();

            for (auto* light : scene->GetFastAccessComponents().lights)
            {
                if (light == nullptr || !light->IsSelfEnabled() ||
                    light->GetLightType() != NLS::Render::Settings::ELightType::DIRECTIONAL)
                {
                    continue;
                }

                auto* lightOwner = light->gameobject();
                const auto* lightData = light->GetData();
                if (lightOwner == nullptr || !lightOwner->IsActive() ||
                    lightData == nullptr || lightData->transform == nullptr)
                {
                    continue;
                }

                const auto forward = lightData->transform->GetWorldForward();
                const auto color = light->GetColor();
                meshUniforms.lightDirectionX = -forward.x;
                meshUniforms.lightDirectionY = -forward.y;
                meshUniforms.lightDirectionZ = -forward.z;
                meshUniforms.lightIntensity = std::max(light->GetIntensity(), 0.0f);
                meshUniforms.lightColorR = std::max(color.x, 0.0f);
                meshUniforms.lightColorG = std::max(color.y, 0.0f);
                meshUniforms.lightColorB = std::max(color.z, 0.0f);
                break;
            }

            for (auto* meshRenderer : scene->GetFastAccessComponents().modelRenderers)
            {
                if (meshRenderer == nullptr || !meshRenderer->IsSelfEnabled())
                    continue;
                auto* owner = meshRenderer->gameobject();
                if (owner == nullptr || !owner->IsActive() || owner->GetTransform() == nullptr)
                    continue;

                auto* meshFilter = owner->GetComponent<NLS::Engine::Components::MeshFilter>();
                auto* mesh = meshFilter != nullptr ? meshFilter->ResolveMesh() : nullptr;
                auto rhiMesh = mesh != nullptr ? mesh->GetRHIMesh() : nullptr;
                if (rhiMesh == nullptr || rhiMesh->GetVertexBuffer() == nullptr ||
                    rhiMesh->GetVertexCount() == 0u)
                    continue;

                const auto nativeVertexBuffer = rhiMesh->GetVertexBuffer()->GetNativeBufferHandle();
                if (nativeVertexBuffer.backend != NLS::Render::RHI::BackendType::Metal ||
                    nativeVertexBuffer.handle == nullptr)
                    continue;
                id<MTLBuffer> vertexBuffer = (__bridge id<MTLBuffer>)nativeVertexBuffer.handle;
                if (vertexBuffer == nil)
                    continue;

                meshUniforms.baseColorR = 0.72f;
                meshUniforms.baseColorG = 0.74f;
                meshUniforms.baseColorB = 0.78f;
                meshUniforms.baseColorA = 1.0f;
                meshUniforms.useBaseTexture = 0.0f;
                meshUniforms.metallic = 0.0f;
                meshUniforms.roughness = 1.0f;
                meshUniforms.ambientOcclusion = 1.0f;
                meshUniforms.emissiveR = 0.0f;
                meshUniforms.emissiveG = 0.0f;
                meshUniforms.emissiveB = 0.0f;
                auto* material = meshRenderer->ResolveMaterialAtIndex(mesh->GetMaterialIndex());
                ApplyMaterialBaseColor(material, meshUniforms);
                ApplyMaterialPbrParameters(material, meshUniforms);
                id<MTLTexture> baseTexture = ResolveMaterialBaseTexture(material);
                if (baseTexture != nil)
                    meshUniforms.useBaseTexture = 1.0f;
                else
                    baseTexture = m_impl->whiteTexture;

                std::memcpy(meshUniforms.model, owner->GetTransform()->GetWorldMatrix().data,
                    sizeof(meshUniforms.model));
                [encoder setVertexBytes:&meshUniforms length:sizeof(meshUniforms) atIndex:1];
                [encoder setFragmentBytes:&meshUniforms length:sizeof(meshUniforms) atIndex:1];
                [encoder setFragmentTexture:baseTexture atIndex:0];
                [encoder setFragmentSamplerState:m_impl->baseSampler atIndex:0];
                [encoder setVertexBuffer:vertexBuffer offset:0u atIndex:0];

                const auto indexBuffer = rhiMesh->GetIndexBuffer();
                const auto indexCount = rhiMesh->GetIndexCount();
                if (indexBuffer != nullptr && indexCount > 0u)
                {
                    const auto nativeIndexBuffer = indexBuffer->GetNativeBufferHandle();
                    if (nativeIndexBuffer.backend != NLS::Render::RHI::BackendType::Metal ||
                        nativeIndexBuffer.handle == nullptr)
                        continue;
                    id<MTLBuffer> metalIndexBuffer = (__bridge id<MTLBuffer>)nativeIndexBuffer.handle;
                    if (metalIndexBuffer == nil)
                        continue;
                    [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                         indexCount:indexCount
                                          indexType:rhiMesh->GetIndexType() == NLS::Render::RHI::IndexType::UInt16
                                              ? MTLIndexTypeUInt16
                                              : MTLIndexTypeUInt32
                                        indexBuffer:metalIndexBuffer
                                  indexBufferOffset:0u];
                }
                else
                {
                    [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                                vertexStart:0u
                                vertexCount:rhiMesh->GetVertexCount()];
                }
                ++renderedMeshCount;
            }
        }
        [encoder endEncoding];
        [commandBuffer commit];

        if (!m_impl->loggedFirstFrame)
        {
            NLS_LOG_INFO("MetalViewportRenderer: encoded first native Metal preview frame for " + m_impl->debugName +
                ", rendered mesh count=" + std::to_string(renderedMeshCount));
            m_impl->loggedFirstFrame = true;
        }
        return true;
    }

    const std::shared_ptr<NLS::Render::RHI::RHITextureView>& MetalViewportRenderer::GetOutputTextureView() const
    {
        static const std::shared_ptr<NLS::Render::RHI::RHITextureView> empty;
        return m_impl != nullptr ? m_impl->outputView : empty;
    }
}
