#shader vertex
#version 430 core

layout (location = 0) in vec3 geo_Pos;
layout (location = 1) in vec2 geo_TexCoords;

out vec2 v_TexCoords;

void main()
{
    v_TexCoords = geo_TexCoords;
    gl_Position = vec4(geo_Pos, 1.0);
}

#shader fragment
#version 430 core

in vec2 v_TexCoords;
out vec4 FRAGMENT_COLOR;

layout(std430, binding = 0) buffer LightSSBO
{
    mat4 ssbo_Lights[];
};

struct ClusterRecord
{
    uint offset;
    uint count;
};

layout(std430, binding = 1) buffer ClusterRecordSSBO
{
    ClusterRecord ssbo_ClusterRecords[];
};

layout(std430, binding = 2) buffer ClusterIndexSSBO
{
    uint ssbo_ClusterLightIndices[];
};

layout(binding = 0) uniform sampler2D u_GBufferAlbedo;
layout(binding = 1) uniform sampler2D u_GBufferPosition;
layout(binding = 2) uniform sampler2D u_GBufferNormal;
layout(binding = 3) uniform sampler2D u_GBufferMaterial;
layout(binding = 4) uniform sampler2D u_GBufferDepth;
layout(location = 0) uniform vec3 u_ClusterDimensions = vec3(16.0, 9.0, 24.0);
layout(location = 1) uniform vec2 u_ScreenSize = vec2(1.0, 1.0);
layout(location = 2) uniform vec2 u_NearFar = vec2(0.1, 100.0);
layout(location = 3) uniform int u_ClusterLightIndexCount = 0;
layout(location = 4) uniform vec3 u_CameraWorldPos = vec3(0.0, 0.0, 0.0);

vec3 UnPack(float p_Target)
{
    return vec3
    (
        float((uint(p_Target) >> 24) & 0xff) * 0.003921568627451,
        float((uint(p_Target) >> 16) & 0xff) * 0.003921568627451,
        float((uint(p_Target) >> 8) & 0xff) * 0.003921568627451
    );
}

bool PointInAABB(vec3 p_Point, vec3 p_AabbCenter, vec3 p_AabbHalfSize)
{
    return
    (
        p_Point.x > p_AabbCenter.x - p_AabbHalfSize.x && p_Point.x < p_AabbCenter.x + p_AabbHalfSize.x &&
        p_Point.y > p_AabbCenter.y - p_AabbHalfSize.y && p_Point.y < p_AabbCenter.y + p_AabbHalfSize.y &&
        p_Point.z > p_AabbCenter.z - p_AabbHalfSize.z && p_Point.z < p_AabbCenter.z + p_AabbHalfSize.z
    );
}

float LuminosityFromAttenuation(mat4 p_Light, vec3 worldPosition)
{
    vec3 lightPosition = p_Light[0].rgb;
    float constant = p_Light[0][3];
    float linear = p_Light[1][3];
    float quadratic = p_Light[2][3];

    float distanceToLight = length(lightPosition - worldPosition);
    float attenuation = constant + linear * distanceToLight + quadratic * (distanceToLight * distanceToLight);
    return 1.0 / max(attenuation, 0.00001);
}

vec3 BlinnPhong(vec3 normal, vec3 viewDir, vec3 lightDir, vec3 lightColor, vec3 diffuseColor, vec3 specularColor, float shininess, float luminosity)
{
    vec3 halfwayDir = normalize(lightDir + viewDir);
    float diffuseCoefficient = max(dot(normal, lightDir), 0.0);
    float specularCoefficient = pow(max(dot(normal, halfwayDir), 0.0), max(shininess * 2.0, 1.0));

    vec3 diffuse = lightColor * diffuseColor * diffuseCoefficient * luminosity;
    vec3 specular = lightColor * specularColor * specularCoefficient * luminosity;
    return diffuse + ((luminosity > 0.0) ? specular : vec3(0.0));
}

vec3 EvaluateLight(mat4 lightData, vec3 worldPosition, vec3 normal, vec3 viewDir, vec3 diffuseColor, vec3 specularColor, float shininess)
{
    int lightType = int(lightData[3][0]);
    vec3 lightColor = UnPack(lightData[2][0]);
    float intensity = lightData[3][3];

    if (lightType == 1)
    {
        return BlinnPhong(normal, viewDir, normalize(-lightData[1].rgb), lightColor, diffuseColor, specularColor, shininess, intensity);
    }

    if (lightType == 3)
    {
        vec3 extent = vec3(lightData[0][3], lightData[1][3], lightData[2][3]);
        return PointInAABB(worldPosition, lightData[0].rgb, extent) ? diffuseColor * lightColor * intensity : vec3(0.0);
    }

    if (lightType == 4)
    {
        return distance(lightData[0].rgb, worldPosition) <= lightData[0][3] ? diffuseColor * lightColor * intensity : vec3(0.0);
    }

    vec3 lightDirection = normalize(lightData[0].rgb - worldPosition);
    float luminosity = LuminosityFromAttenuation(lightData, worldPosition);

    if (lightType == 2)
    {
        float cutOff = cos(radians(lightData[3][1]));
        float outerCutOff = cos(radians(lightData[3][1] + lightData[3][2]));
        float theta = dot(lightDirection, normalize(-lightData[1].rgb));
        float epsilon = max(cutOff - outerCutOff, 0.00001);
        float spotIntensity = clamp((theta - outerCutOff) / epsilon, 0.0, 1.0);
        luminosity *= spotIntensity;
    }

    return BlinnPhong(normal, viewDir, lightDirection, lightColor, diffuseColor, specularColor, shininess, intensity * luminosity);
}

uint GetClusterIndex(vec3 viewPosition)
{
    uint clusterX = min(uint(gl_FragCoord.x / max(u_ScreenSize.x / max(u_ClusterDimensions.x, 1.0), 1.0)), uint(max(u_ClusterDimensions.x - 1.0, 0.0)));
    uint clusterY = min(uint(gl_FragCoord.y / max(u_ScreenSize.y / max(u_ClusterDimensions.y, 1.0), 1.0)), uint(max(u_ClusterDimensions.y - 1.0, 0.0)));

    float depth = clamp((-viewPosition.z - u_NearFar.x) / max(u_NearFar.y - u_NearFar.x, 0.00001), 0.0, 1.0);
    uint clusterZ = min(uint(depth * max(u_ClusterDimensions.z - 1.0, 0.0)), uint(max(u_ClusterDimensions.z - 1.0, 0.0)));

    uint dimX = uint(u_ClusterDimensions.x);
    uint dimY = uint(u_ClusterDimensions.y);
    return clusterX + clusterY * dimX + clusterZ * dimX * dimY;
}

void main()
{
    vec4 albedo = texture(u_GBufferAlbedo, v_TexCoords);
    if (albedo.a <= 0.0)
    {
        discard;
    }

    vec3 worldPosition = texture(u_GBufferPosition, v_TexCoords).xyz;
    vec3 normal = texture(u_GBufferNormal, v_TexCoords).xyz * 2.0 - 1.0;
    vec4 material = texture(u_GBufferMaterial, v_TexCoords);
    float depth = texture(u_GBufferDepth, v_TexCoords).r;

    vec3 viewDir = normalize(u_CameraWorldPos - worldPosition);
    float shininess = max(material.a * 256.0, 1.0);

    vec3 lightSum = vec3(0.0);
    uint clusterIndex = GetClusterIndex(vec3(0.0, 0.0, -mix(u_NearFar.x, u_NearFar.y, depth)));

    if (clusterIndex < ssbo_ClusterRecords.length())
    {
        ClusterRecord record = ssbo_ClusterRecords[clusterIndex];
        for (uint i = 0u; i < record.count; ++i)
        {
            uint lightIndexAddress = record.offset + i;
            if (lightIndexAddress >= uint(u_ClusterLightIndexCount))
            {
                break;
            }

            uint lightIndex = ssbo_ClusterLightIndices[lightIndexAddress];
            if (lightIndex < ssbo_Lights.length())
            {
                lightSum += EvaluateLight(ssbo_Lights[lightIndex], worldPosition, normalize(normal), viewDir, albedo.rgb, material.rgb, shininess);
            }
        }
    }

    FRAGMENT_COLOR = vec4(lightSum, albedo.a);
}
