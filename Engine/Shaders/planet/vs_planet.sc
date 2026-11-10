$input a_position, a_normal, a_texcoord0
$output v_objectPos, v_normal, v_worldPos, v_texcoord0, v_lightObj

#include <bgfx_shader.sh>

uniform vec4 u_planetLight; // xyz towards the light, in the world

void main()
{
	vec4 world = mul(u_model[0], vec4(a_position, 1.0));
	v_worldPos = world.xyz;
	v_objectPos = a_position;
	v_normal = mul(u_model[0], vec4(a_normal, 0.0)).xyz;
	v_texcoord0 = a_texcoord0;
	// The way to the light in the body's own frame (its turn undone): for its cloud's shadows and relief (fs_planet).
	v_lightObj = mul(vec4(u_planetLight.xyz, 0.0), u_model[0]).xyz;
	gl_Position = mul(u_viewProj, world);
}
