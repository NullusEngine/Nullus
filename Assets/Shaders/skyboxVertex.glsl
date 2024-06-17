#version 450 core

#ifdef VULKAN
layout(binding = 0) uniform UniformBufferObject {
	mat4 modelMatrix;
	mat4 viewMatrix;
	mat4 projMatrix;
} ubo;
#else
uniform mat4 modelMatrix;
uniform mat4 viewMatrix;
uniform mat4 projMatrix;
#endif

layout(location = 0) in  vec3 position;

layout(location = 0) out Vertex {
	vec3 viewDir;
} OUT;

void main(void) {
	vec3 pos = position;
#ifdef VULKAN
	mat4 invproj  = inverse(ubo.projMatrix);
#else
	mat4 invproj  = inverse(projMatrix);
#endif
	pos.xy	  *= vec2(invproj[0][0],invproj[1][1]);
	pos.z 	= -1.0f;

#ifdef VULKAN
	OUT.viewDir		= transpose(mat3(ubo.viewMatrix)) * normalize(pos);
#else
	OUT.viewDir		= transpose(mat3(viewMatrix)) * normalize(pos);
#endif
	gl_Position		= vec4(position, 1.0);
}
