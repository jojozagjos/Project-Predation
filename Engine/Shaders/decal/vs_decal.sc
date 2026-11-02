$input a_position, a_color0
$output v_color0, v_worldPos

// A mark laid on a surface (Material::decal): only where it is and its colour.

#include <bgfx_shader.sh>

void main()
{
	v_worldPos = mul(u_model[0], vec4(a_position, 1.0)).xyz;
	gl_Position = mul(u_modelViewProj, vec4(a_position, 1.0));
	v_color0 = a_color0;
}
