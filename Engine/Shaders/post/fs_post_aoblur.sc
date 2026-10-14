$input v_texcoord0

#include <bgfx_shader.sh>

// The shut-in-ness (fs_post_ao) smoothed: the average of the pixels round each one -- only those at much the same depth, so
// the shade of a corner does not spread across the edge of a thing in front of it.

SAMPLER2D(s_scene, 0); // what fs_post_ao made
SAMPLER2D(s_bloom, 1); // the scene's depth
uniform vec4 u_postTexel; // xy = one pixel
uniform vec4 u_postFlip;
uniform mat4 u_aoInvProj;
uniform vec4 u_aoParams;  // w = 1 when depth runs -1 to 1

float DistanceAt(vec2 uv)
{
	float depth = texture2DLod(s_bloom, uv, 0.0).r;
	vec2 ndc = vec2(uv.x * 2.0 - 1.0, u_postFlip.x > 0.5 ? uv.y * 2.0 - 1.0 : 1.0 - uv.y * 2.0);
	vec4 at = mul(u_aoInvProj, vec4(ndc, u_aoParams.w > 0.5 ? depth * 2.0 - 1.0 : depth, 1.0));
	return -at.z / at.w;
}

void main()
{
	vec2 uv = v_texcoord0;
	float here = DistanceAt(uv);
	float sum = 0.0;
	float weights = 0.0;
	for (int y = 0; y < 4; ++y)
	{
		for (int x = 0; x < 4; ++x)
		{
			vec2 offset = (vec2(float(x), float(y)) - vec2_splat(1.5)) * u_postTexel.xy;
			float there = DistanceAt(uv + offset);
			float weight = 1.0 / (0.02 + abs(there - here) / max(here * 0.02, 0.01));
			sum += texture2DLod(s_scene, uv + offset, 0.0).r * weight;
			weights += weight;
		}
	}
	gl_FragColor = vec4_splat(sum / max(weights, 1.0e-5));
}
