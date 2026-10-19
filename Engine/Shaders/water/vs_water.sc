$input a_position, a_texcoord0
$output v_worldPos, v_texcoord0

#include <bgfx_shader.sh>

// The water's surface (WaterRenderer): flat at its level, laid out in the world already; how deep it is under each corner
// passed on (a_texcoord0.x), for its colour and its shore.
void main()
{
	v_worldPos = a_position;
	v_texcoord0 = a_texcoord0;
	gl_Position = mul(u_viewProj, vec4(a_position, 1.0));
}
