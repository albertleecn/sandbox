#version 330 core

layout (location = 0) in vec3 inPosition;
layout (location = 1) in vec3 inNormal;
layout (location = 3) in vec2 inTexCoord;

uniform mat4 u_modelMatrix;
uniform mat4 u_modelMatrixIT;
uniform mat4 u_viewProj;
uniform mat4 u_dirLightViewProjectionMat;

out vec2 v_texcoord;
out vec3 v_normal;
out vec3 v_world_position;
out vec4 v_camera_directional_light;

void main()
{
    vec4 worldPosition = u_modelMatrix * vec4(inPosition, 1.0);
    gl_Position = u_viewProj * worldPosition;
    v_texcoord = inTexCoord;
    v_normal = normalize((u_modelMatrixIT * vec4(inNormal, 1.0)).xyz);
    v_world_position = worldPosition.xyz;
    v_camera_directional_light = u_dirLightViewProjectionMat * worldPosition;
}