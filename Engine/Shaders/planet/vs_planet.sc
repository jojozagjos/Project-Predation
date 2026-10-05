$input a_position, a_normal, a_texcoord0
$output v_objectPos, v_normal, v_worldPos, v_texcoord0

#include <bgfx_shader.sh>

void main()
{
	vec4 world = mul(u_model[0], vec4(a_position, 1.0));
	v_worldPos = world.xyz;
	v_objectPos = a_position;
	v_normal = mul(u_model[0], vec4(a_normal, 0.0)).xyz;
	v_texcoord0 = a_texcoord0;
	gl_Position = mul(u_viewProj, world);
}
