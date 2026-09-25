$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_scene, 0);
SAMPLER2D(s_bloom, 1);
uniform vec4 u_postTexel; // zw = the screen, in pixels
uniform vec4 u_postTone;  // x = exposure, y = contrast, z = how much glow, w = saturation
uniform vec4 u_postLens;  // x = vignette, y = grain, z = colour fringe at the edges, w = seconds
uniform vec4 u_postMood;  // x = fear, 0 to 1; y = cold in the shadows; z = warmth in the highlights; w = a red flash

float Hash(vec2 p)
{
	return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}

// The picture, finished: everything the scene wrote was light, in linear units; this is where it
// becomes an image. Glow added, exposed, through the filmic curve, encoded, graded cold in the dark
// and warm in the light, darkened at the edges, and grained like film -- and, with something close,
// the edges close in and drain of colour in time with a pulse.
void main()
{
	vec2 uv = v_texcoord0;
	vec2 fromMiddle = uv - vec2_splat(0.5);
	float edge = dot(fromMiddle, fromMiddle) * 2.0; // 0 in the middle, 1 in the corners

	// A lens's colour fringe, only towards the edges.
	vec2 fringe = fromMiddle * edge * u_postLens.z;
	vec3 color;
	color.r = texture2D(s_scene, uv + fringe).r;
	color.g = texture2D(s_scene, uv).g;
	color.b = texture2D(s_scene, uv - fringe).b;
	color += texture2D(s_bloom, uv).rgb * u_postTone.z;

	// Exposure, the Narkowicz approximation of the ACES curve, gamma, and contrast about middle grey.
	color *= max(u_postTone.x, 0.0);
	color = clamp((color * (2.51 * color + 0.03)) / (color * (2.43 * color + 0.59) + 0.14), 0.0, 1.0);
	color = pow(color, vec3_splat(1.0 / 2.2));
	color = clamp((color - vec3_splat(0.5)) * max(u_postTone.y, 0.0) + vec3_splat(0.5), 0.0, 1.0);

	// The grade.
	float fear = clamp(u_postMood.x, 0.0, 1.0);
	float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
	color = mix(vec3_splat(luma), color, max(u_postTone.w * (1.0 - 0.4 * fear), 0.0));
	color += vec3(-0.018, 0.006, 0.03) * u_postMood.y * (1.0 - smoothstep(0.0, 0.45, luma));
	color += vec3(0.03, 0.012, -0.02) * u_postMood.z * smoothstep(0.5, 1.0, luma);

	// The edges, darker; and closing in with fear, beating.
	float pulse = 0.5 + 0.5 * sin(u_postLens.w * 7.2);
	float vignette = u_postLens.x + fear * (0.35 + 0.2 * pulse);
	color *= 1.0 - clamp(edge, 0.0, 1.0) * clamp(vignette, 0.0, 0.95);
	color += vec3(0.045, 0.0, 0.0) * fear * clamp(edge * 1.5 - 0.4, 0.0, 1.0);

	// A red flash -- the moment something takes hold -- strongest at the edges.
	color = mix(color, vec3(0.55, 0.02, 0.02) * (0.4 + 0.6 * edge), clamp(u_postMood.w, 0.0, 1.0) * (0.35 + 0.4 * edge));

	// Grain, more in the dark than the light, different every frame.
	float grain = Hash(uv * u_postTexel.zw + vec2_splat(fract(u_postLens.w * 13.37) * 311.0)) - 0.5;
	color += vec3_splat(grain * u_postLens.y * (1.0 - 0.6 * luma));

	// And the dither that stops smooth dark gradients banding.
	float dither = fract(52.9829189 * fract(0.06711056 * gl_FragCoord.x + 0.00583715 * gl_FragCoord.y));
	color += vec3_splat((dither - 0.5) / 255.0);
	gl_FragColor = vec4(clamp(color, 0.0, 1.0), 1.0);
}
