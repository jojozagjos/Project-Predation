$input a_position, a_normal, a_texcoord0
$output v_texcoord0

#include <bgfx_shader.sh>

// A square over the whole of what a planet's face is baked into (fs_planet_bake): straight through, no camera.
void main()
{
	v_texcoord0 = a_texcoord0;
	gl_Position = vec4(a_position.xy, 0.0, 1.0);
}
