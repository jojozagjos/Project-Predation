$input v_depth, v_texcoord0

#include <bgfx_shader.sh>

uniform vec4 u_shadowRange; // x = how far this map reaches along its axis, in metres
SAMPLER2D(s_cutout, 0);     // the texture whose alpha cuts it out (Material::cutout)

// As fs_shadow, for a leaf on a card: a shadow only where the leaf is.
void main()
{
	if (texture2D(s_cutout, v_texcoord0).a < 0.5)
	{
		discard;
	}
	gl_FragColor = vec4(u_shadowRange.x - v_depth.x, 0.0, 0.0, 1.0);
}
