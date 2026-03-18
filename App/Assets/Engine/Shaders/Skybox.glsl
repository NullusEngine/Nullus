#shader vertex
#version 430 core

layout (location = 0) in vec3 aPos;

out vec3 TexCoords;

/* Global information sent by the engine */
layout (std140, binding = 0) uniform EngineUBO
{
    mat4    ubo_Model;
    mat4    ubo_View;
    mat4    ubo_Projection;
    vec3    ubo_ViewPos;
    float   ubo_Time;
};

void main()
{
	// vec3 pos = aPos;
	// mat4 invproj  = inverse(ubo_Projection);
	// pos.xy	  *= vec2(invproj[0][0],invproj[1][1]);
	// pos.z 	= -1.0f;

	// TexCoords		= transpose(mat3(ubo_View)) * normalize(pos); // viewDir
	// gl_Position		= vec4(aPos, 1.0);

    TexCoords = aPos;
	mat4 view = mat4(mat3(ubo_View));
    vec4 pos = ubo_Projection * view * vec4(aPos, 1.0);
    gl_Position = pos.xyww;
}

#shader fragment
#version 430 core

out vec4 FragColor;

in vec3 TexCoords;

layout(binding = 0) uniform samplerCube cubeTex;
uniform bool u_UseProceduralSky = true;
uniform vec3 u_SkyTint = vec3(0.50, 0.62, 0.82);
uniform vec3 u_GroundColor = vec3(0.46, 0.44, 0.42);
uniform vec3 u_SunDirection = normalize(vec3(-0.35, 0.78, -0.18));
uniform float u_Exposure = 1.0;
uniform float u_AtmosphereThickness = 1.0;
uniform float u_SunSize = 0.045;
uniform float u_SunSizeConvergence = 5.0;

vec3 EvalProceduralSky(vec3 dir)
{
    float atmosphere = clamp(u_AtmosphereThickness, 0.25, 2.0);
    float horizon = clamp(dir.y * 0.5 + 0.5, 0.0, 1.0);

    vec3 zenithColor = mix(vec3(0.42, 0.56, 0.79), u_SkyTint, 0.60);
    vec3 skyMidColor = mix(vec3(0.58, 0.74, 0.92), u_SkyTint, 0.30);
    vec3 horizonColor = vec3(0.86, 0.97, 0.99);
    vec3 groundColor = u_GroundColor;

    float upperBlend = pow(smoothstep(0.52, 1.0, horizon), mix(1.10, 0.82, atmosphere * 0.45));
    float midBlend = smoothstep(0.50, 0.74, horizon);
    float horizonGlow = 1.0 - smoothstep(0.47, 0.56, horizon);
    float skyBlend = smoothstep(0.49, 0.53, horizon);

    vec3 sky = mix(horizonColor, skyMidColor, midBlend);
    sky = mix(sky, zenithColor, upperBlend);
    sky += horizonColor * horizonGlow * 0.06;

    vec3 color = mix(groundColor, sky, skyBlend);

    return color * u_Exposure;
}

void main()
{
    vec3 dir = normalize(TexCoords);

    if (!u_UseProceduralSky)
    {
        FragColor = texture(cubeTex, dir);
        return;
    }

    FragColor = vec4(EvalProceduralSky(dir), 1.0);
}
