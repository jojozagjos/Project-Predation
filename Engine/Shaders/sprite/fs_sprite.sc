$input v_texcoord0, v_color0

#include <bgfx_shader.sh>

// A soft round blot of colour, fading out to nothing at its edge: a flake, a drop's streak, a grain, a puff of dust or mist.
// Its colour is linear light, lit and fogged already (SpriteRenderer's caller), and blended over the picture.
void main()
{
	float r = length(v_texcoord0 * 2.0 - 1.0);
	float alpha = v_color0.a * (1.0 - smoothstep(0.25, 1.0, r));
	if (alpha < 0.002)
	{
		discard;
	}
	gl_FragColor = vec4(v_color0.rgb, alpha);
}
