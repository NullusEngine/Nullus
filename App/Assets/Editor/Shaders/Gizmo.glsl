#shader vertex
#version 430 core

layout (location = 0) in vec3 geo_Pos;
layout (location = 1) in vec2 geo_TexCoords;
layout (location = 2) in vec3 geo_Normal;

layout (std140) uniform EngineUBO
{
    mat4    ubo_Model;
    mat4    ubo_View;
    mat4    ubo_Projection;
    vec3    ubo_ViewPos;
    float   ubo_Time;
};

out VS_OUT
{
    vec3 Color;
} vs_out;

uniform bool u_IsBall;
uniform bool u_IsPickable;
uniform int u_HighlightedAxis;

mat4 rotationMatrix(vec3 axis, float angle)
{
    axis = normalize(axis);
    float s = sin(angle);
    float c = cos(angle);
    float oc = 1.0 - c;
    
    return mat4
    (
        oc * axis.x * axis.x + c,           oc * axis.x * axis.y - axis.z * s,  oc * axis.z * axis.x + axis.y * s,  0.0,
        oc * axis.x * axis.y + axis.z * s,  oc * axis.y * axis.y + c,           oc * axis.y * axis.z - axis.x * s,  0.0,
        oc * axis.z * axis.x - axis.y * s,  oc * axis.y * axis.z + axis.x * s,  oc * axis.z * axis.z + c,           0.0,
        0.0,                                0.0,                                0.0,                                1.0
    );
}

void main()
{
    mat4 instanceModel = ubo_Model;

    if (gl_InstanceID == 1) 
        instanceModel *= rotationMatrix(vec3(0, 1, 0), radians(-90)); /* X axis */
    else if (gl_InstanceID == 2) 
        instanceModel *= rotationMatrix(vec3(1, 0, 0), radians(90)); /* Y axis */

    float distanceToCamera = distance(ubo_ViewPos, instanceModel[3].xyz);

	vec3 pos = geo_Pos;

    vec3 fragPos = vec3(instanceModel * vec4(pos * distanceToCamera * 0.1f, 1.0));

	if (u_IsPickable)
	{
		int blueComponent = 0;

		if (gl_InstanceID == 1)
			blueComponent = 252;

		if (gl_InstanceID == 2)
			blueComponent = 253;

		if (gl_InstanceID == 0)
			blueComponent = 254;

		vs_out.Color = vec3(1.0f, 1.0f, blueComponent / 255.0f);
	}
	else
	{
		if (u_IsBall)
		{
			vs_out.Color = vec3(1.0f);
		}
		else
		{
			float red	= float(gl_InstanceID == 1); // X
			float green = float(gl_InstanceID == 2); // Y
			float blue	= float(gl_InstanceID == 0); // Z

			if (!u_IsPickable && ((gl_InstanceID == 1 && u_HighlightedAxis == 0) || (gl_InstanceID == 2 && u_HighlightedAxis == 1) || (gl_InstanceID == 0 && u_HighlightedAxis == 2)))
			{
				vs_out.Color = vec3(1.0f, 1.0f, 0.0f);
			}	
			else
			{
				vs_out.Color = vec3(red, green, blue);
			}
		}
	}

    gl_Position = ubo_Projection * ubo_View * vec4(fragPos, 1.0);
}

#shader fragment
#version 430 core

out vec4 FRAGMENT_COLOR;

in VS_OUT
{
    vec3 Color;
} fs_in;

uniform bool u_IsPickable;

void main()
{
	FRAGMENT_COLOR = vec4(fs_in.Color, 1.0f);
}