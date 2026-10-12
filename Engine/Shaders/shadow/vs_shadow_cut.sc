$input a_position, a_texcoord0
$output v_depth, v_texcoord0

#include <bgfx_shader.sh>

// As vs_shadow, and the texture's coordinates passed on, for what is cut out by its texture (fs_shadow_cut).
void main()
{
	vec4 worldPosition = mul(u_model[0], vec4(a_position, 1.0));
	gl_Position = mul(u_viewProj, worldPosition);
	v_depth = vec4(-mul(u_view, worldPosition).z, 0.0, 0.0, 1.0);
	v_texcoord0 = a_texcoord0;
}
