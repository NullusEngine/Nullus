#version 450 core

uniform mat4 modelMatrix;
uniform mat4 viewMatrix;
uniform mat4 projMatrix;

layout(location = 0) in  vec3 position;

layout(location = 0) out Vertex {
	vec3 viewDir;
} OUT;

void main(void) {
	vec3 pos = position;
	mat4 invproj  = inverse(projMatrix);
	pos.xy	  *= vec2(invproj[0][0],invproj[1][1]);
	pos.z 	= -1.0f;

	OUT.viewDir		= transpose(mat3(viewMatrix)) * normalize(pos);
	gl_Position		= vec4(position, 1.0);
}
