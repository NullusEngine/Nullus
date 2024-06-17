#version 450 core

layout(binding = 1) uniform samplerCube cubeTex;

layout(location = 0) in Vertex {
	vec3 viewDir;
} IN;

layout(location = 0) out vec4 fragColour;

void main(void)	{
	vec4 samp = texture(cubeTex,normalize(IN.viewDir));
	fragColour = pow(samp, vec4(2.2f));
}