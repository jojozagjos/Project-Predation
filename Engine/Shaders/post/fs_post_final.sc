$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_scene, 0);
SAMPLER2D(s_bloom, 1);
SAMPLER2D(s_adapt, 2); // the brightness the eye has adapted to, as its logarithm (fs_post_adapt)
SAMPLER2D(s_ao, 3);    // how open each point is (fs_post_ao, smoothed): 1 open, 0 shut in
uniform vec4 u_postTexel; // zw = the screen, in pixels
uniform vec4 u_postTone;  // x = exposure, y = contrast, z = how much glow, w = saturation
uniform vec4 u_postLens;  // x = vignette, y = grain, z = colour fringe at the edges, w = seconds
uniform vec4 u_postMood;  // x = fear, 0 to 1; y = cold in the shadows; z = warmth in the highlights; w = a red flash
uniform vec4 u_postAdapt; // x = the brightness the picture is set for, y = how far the eye adapts away from it (0 none), z, w = how
                          // much darker and brighter that can make it at most
uniform vec4 u_aoParams; // x = how much the shut-in parts are darkened (0: none)
uniform vec4 u_postSun;  // xy = where the sun is in the picture (as its texture runs), z = 1 when it is ahead, w = how much it flares
SAMPLER2D(s_depth, 4);    // the scene's depth, for the air thick with weather to be drawn through
uniform vec4 u_postFlip;  // x = 1 when the picture's first row is its bottom
uniform mat4 u_aoInvProj; // the picture's projection, back
uniform mat4 u_airView;   // the view's turn, undone: the view's directions into the world's
uniform vec4 u_air;       // x = how thick the air is with weather at the thickest, a share of the light taken each metre (0: clear);
                          // y = how far out it is drawn, metres; z = how high the eye is over the ground
uniform vec4 u_airColour; // rgb = its colour, lit
uniform vec4 u_airAt;     // xyz = where the eye is in its clouds (where it is, less how far they have rolled)

// Value noise in three dimensions, for the clouds of it: a number to each whole point, eased between.
float AirHash(vec3 p)
{
	p = fract(p * 0.1031);
	p += dot(p, p.zyx + 31.32);
	return fract((p.x + p.y) * p.z);
}

float AirNoise(vec3 p)
{
	vec3 i = floor(p);
	vec3 f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	float a = mix(AirHash(i), AirHash(i + vec3(1.0, 0.0, 0.0)), f.x);
	float b = mix(AirHash(i + vec3(0.0, 1.0, 0.0)), AirHash(i + vec3(1.0, 1.0, 0.0)), f.x);
	float c = mix(AirHash(i + vec3(0.0, 0.0, 1.0)), AirHash(i + vec3(1.0, 0.0, 1.0)), f.x);
	float d = mix(AirHash(i + vec3(0.0, 1.0, 1.0)), AirHash(i + vec3(1.0, 1.0, 1.0)), f.x);
	return mix(mix(a, b, f.y), mix(c, d, f.y), f.z);
}

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
	// Darker where it is shut in -- a corner, under a crate, at a boot -- for the light that comes from all round, not the
	// sun or a lamp straight on it: so less of it the brighter the point is against what the eye is set for.
	if (u_aoParams.x > 0.0)
	{
		float open = texture2D(s_ao, uv).r;
		float setFor = u_postAdapt.y > 0.0 ? exp(texture2D(s_adapt, vec2_splat(0.5)).r) : u_postAdapt.x;
		float lit = dot(color, vec3(0.2126, 0.7152, 0.0722)) / max(setFor, 1.0e-4);
		color *= mix(1.0, open, u_aoParams.x * (1.0 - 0.75 * smoothstep(1.5, 6.0, lit)));
	}
	color += texture2D(s_bloom, uv).rgb * u_postTone.z;

	// The air thick with weather -- a sandstorm's dust, a blizzard's snow -- in clouds of it, thick and thin, rolling past
	// with the wind: gone through from the eye out to what the picture shows there (or as far as it is drawn), a few
	// steps of it, each taking its share of the light and giving the air's own colour in its place. Thickest low down.
	if (u_air.x > 0.0)
	{
		float depth = texture2DLod(s_depth, uv, 0.0).r;
		vec2 ndc = vec2(uv.x * 2.0 - 1.0, u_postFlip.x > 0.5 ? uv.y * 2.0 - 1.0 : 1.0 - uv.y * 2.0);
		// Which way it lies, from any depth on the line; how far, from the depth there (nothing drawn: as far as it goes).
		vec4 middle = mul(u_aoInvProj, vec4(ndc, 0.5, 1.0));
		vec4 there = mul(u_aoInvProj, vec4(ndc, u_aoParams.w > 0.5 ? depth * 2.0 - 1.0 : depth, 1.0));
		float far = abs(depth - u_aoParams.z) < 1.0e-7 ? 1.0e6 : length(there.xyz / there.w);
		vec3 way = normalize(mul(u_airView, vec4(normalize(middle.xyz / middle.w), 0.0)).xyz);
		float step = min(far, u_air.y) / 8.0;
		// (Started a different way into the first step each pixel, so the steps do not show as bands.)
		float t = step * fract(52.9829189 * fract(0.06711056 * gl_FragCoord.x + 0.00583715 * gl_FragCoord.y));
		float through = 1.0;
		for (int i = 0; i < 8; ++i)
		{
			vec3 p = way * t;
			vec3 q = (u_airAt.xyz + p) * vec3(0.06, 0.12, 0.06);
			float n = AirNoise(q) * 0.65 + AirNoise(q * 2.3 + vec3(7.1, 3.3, 1.7)) * 0.35;
			float high = max(p.y + u_air.z, 0.0);
			through *= exp(-u_air.x * (0.08 + 2.4 * smoothstep(0.42, 0.75, n)) * exp(-high / 9.0) * step);
			t += step;
		}
		color = color * through + u_airColour.rgb * (1.0 - through);
	}

	// A soft lens flare off the sun: a few faint ghosts of it along the line from it through the middle of the picture, as
	// much as the sun itself shows -- the picture there as bright as a sun is (a wall, a hill or a cloud in front of it: none).
	if (u_postSun.z > 0.5 && u_postSun.w > 0.0 && all(greaterThan(u_postSun.xy, vec2_splat(-0.05))) && all(lessThan(u_postSun.xy, vec2_splat(1.05))))
	{
		float setFor = u_postAdapt.y > 0.0 ? exp(texture2D(s_adapt, vec2_splat(0.5)).r) : u_postAdapt.x;
		vec2 tap = u_postTexel.xy * 3.0;
		float seen = 0.0;
		for (int i = 0; i < 5; ++i)
		{
			vec2 offset = i == 0 ? vec2_splat(0.0) : vec2(i == 1 ? tap.x : i == 2 ? -tap.x : 0.0, i == 3 ? tap.y : i == 4 ? -tap.y : 0.0);
			vec3 there = texture2DLod(s_scene, clamp(u_postSun.xy + offset, vec2_splat(0.0), vec2_splat(1.0)), 0.0).rgb;
			seen += smoothstep(1.8, 4.0, dot(there, vec3(0.2126, 0.7152, 0.0722)) / max(setFor, 1.0e-4)) * 0.2;
		}
		float aspect = u_postTexel.z / max(u_postTexel.w, 1.0);
		vec2 axis = vec2_splat(0.5) - u_postSun.xy;
		vec3 flare = vec3_splat(0.0);
		for (int k = 0; k < 4; ++k)
		{
			float fk = float(k);
			float along = k == 0 ? 0.55 : k == 1 ? 1.0 : k == 2 ? 1.35 : 1.8;
			float radius = k == 0 ? 0.035 : k == 1 ? 0.07 : k == 2 ? 0.025 : 0.11;
			vec3 tint = k == 0 ? vec3(1.0, 0.8, 0.55) : k == 1 ? vec3(0.55, 0.75, 1.0) : k == 2 ? vec3(0.9, 0.6, 1.0) : vec3(0.5, 0.9, 0.7);
			vec2 apart = (uv - (u_postSun.xy + axis * along)) * vec2(aspect, 1.0);
			flare += tint * smoothstep(radius, radius * 0.55, length(apart)) * (k == 3 ? 0.35 : 0.6) / (1.0 + fk * 0.2);
		}
		color += flare * seen * u_postSun.w * setFor * 1.5;
	}

	// The eye, adapted to what it is looking at: brought partway towards the brightness the picture is set for -- only
	// partway, so a dark room is still dark once the eyes are used to it, and a lit field still bright.
	if (u_postAdapt.y > 0.0)
	{
		float adapted = exp(texture2D(s_adapt, vec2_splat(0.5)).r);
		color *= clamp(pow(u_postAdapt.x / max(adapted, 1.0e-4), u_postAdapt.y), u_postAdapt.z, u_postAdapt.w);
	}

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
