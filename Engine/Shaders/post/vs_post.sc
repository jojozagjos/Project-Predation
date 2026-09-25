$input a_position
$output v_texcoord0

#include <bgfx_shader.sh>

uniform vec4 u_postFlip; // x = 1 where texture coordinates run up the screen (OpenGL), 0 where down

// One triangle over the whole target, and where on the source each pixel reads from.
void main()
{
	gl_Position = vec4(a_position.xy, 0.0, 1.0);
	vec2 uv = a_position.xy * 0.5 + vec2_splat(0.5);
	v_texcoord0 = vec2(uv.x, u_postFlip.x > 0.5 ? uv.y : 1.0 - uv.y);
}
