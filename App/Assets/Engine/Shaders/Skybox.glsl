#shader vertex
#version 430 core

layout (location = 0) in vec3 aPos;

out vec3 TexCoords;

/* Global information sent by the engine */
layout (std140) uniform EngineUBO
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
    gl_Position = ubo_Projection * view * vec4(aPos, 1.0);
}

#shader fragment
#version 430 core

out vec4 FragColor;

in vec3 TexCoords;

uniform samplerCube cubeTex;

void main()
{
	// vec4 samp = texture(cubeTex,normalize(IN.viewDir));
	// fragColour = pow(samp, vec4(2.2f));

    FragColor = texture(cubeTex, TexCoords);
}
