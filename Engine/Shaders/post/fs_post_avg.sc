$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_scene, 0);

// The whole picture's brightness in one number: the average of the 64-square's logarithms, the middle of the picture
// counting for more than its edges, as an eye looks at what is in front of it.
void main()
{
	float sum = 0.0;
	float weights = 0.0;
	for (int y = 0; y < 8; ++y)
	{
		for (int x = 0; x < 8; ++x)
		{
			vec2 at = (vec2(float(x), float(y)) + vec2_splat(0.5)) / 8.0;
			vec2 off = at - vec2_splat(0.5);
			float weight = 1.0 - 0.7 * min(dot(off, off) * 4.0, 1.0);
			sum += texture2D(s_scene, at).r * weight;
			weights += weight;
		}
	}
	gl_FragColor = vec4(sum / weights, 0.0, 0.0, 1.0);
}
