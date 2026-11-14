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
SAMPLER2D(s_held, 5);     // a picture kept, dissolved out of (PostProcess::Settings::dissolve)
uniform vec4 u_postDissolve; // x = how much of the kept picture is laid over this one (0: none)
uniform vec4 u_postFire;     // x = how hard the air burns round the craft (0: none), y = held closed, zw = where it flies, as the texture runs
uniform vec4 u_postFlip;  // x = 1 when the picture's first row is its bottom
uniform mat4 u_aoInvProj; // the picture's projection, back
uniform mat4 u_airView;   // the view's turn, undone: the view's directions into the world's
uniform vec4 u_air;       // x = how thick the air is with weather at the thickest, a share of the light taken each metre (0: clear);
                          // y = how far out it is drawn, metres; z = how high the eye is over the ground
uniform vec4 u_airColour; // rgb = its colour, lit
uniform vec4 u_airAt;     // xyz = where the eye is in its clouds (where it is, less how far they have rolled); w = how far out it begins
uniform vec4 u_cloud;        // the cloud deck: x = its underside, y = its top (heights), z = how much of the sky it covers (0: none), w = how thick
uniform vec4 u_cloudSun;     // xyz = the way towards the sun, w = its strength
uniform vec4 u_cloudLight;   // rgb = the sun's colour, w = how far the clouds have drifted (metres)
uniform vec4 u_cloudAmbient; // rgb = the sky's light in them, w = how far round the site's middle its land is made
uniform vec4 u_cloudAt;      // xyz = where the eye is
uniform vec4 u_cloudRing;    // xy = the site's middle (x, z)

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

// The cloud deck's density at a point of the world: billows of noise thinned to how much of the sky it covers, flat underneath
// and rounded on top, drifting with the wind -- closing over entirely out past where the site's land is made.
float CloudDensity(vec3 w, float detail)
{
	float h = (w.y - u_cloud.x) / max(u_cloud.y - u_cloud.x, 1.0);
	if (h <= 0.0 || h >= 1.0)
	{
		return 0.0;
	}
	vec3 q = vec3((w.x + u_cloudLight.w) * 0.0009, w.y * 0.0013, (w.z + u_cloudLight.w * 0.35) * 0.0009);
	float n = AirNoise(q) * 0.62 + AirNoise(q * 2.13 + vec3(1.7, 9.2, 3.1)) * 0.38 * detail;
	n = n + (AirNoise(q * 5.7 + vec3(8.3, 2.8, 5.9)) - 0.5) * 0.16 * detail;
	float away = length(w.xz - u_cloudRing.xy);
	// Seen from over the deck, coming down to it, thinning away past where the site's land is made: the world's own cloud there,
	// round the globe (closed over solid instead, nothing of the world showed round the site until the shuttle was down through it).
	float over = smoothstep(u_cloud.y, u_cloud.y + 1500.0, u_cloudAt.y);
	float cover = u_cloud.z * (1.0 - smoothstep(u_cloudAmbient.w * 0.8, u_cloudAmbient.w * 2.5, away) * over);
	float profile = smoothstep(0.0, 0.12, h) * smoothstep(1.0, 0.45, h);
	return clamp((n - (1.0 - cover)) * 3.2, 0.0, 1.0) * profile;
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
	// Under water (u_air.w), the picture as seen through it: wavering a little, slowly, the way the water moves in front of the eye.
	float water = clamp(u_air.w, 0.0, 1.0);
	if (water > 0.0)
	{
		float t = u_postLens.w;
		uv += water * 0.0011 * vec2(sin(uv.y * 31.0 + t * 1.9) + 0.5 * sin(uv.y * 77.0 - t * 2.7),
		                            cos(uv.x * 27.0 + t * 1.5) + 0.5 * cos(uv.x * 61.0 + t * 2.3));
	}
	vec2 fromMiddle = uv - vec2_splat(0.5);
	float edge = dot(fromMiddle, fromMiddle) * 2.0; // 0 in the middle, 1 in the corners

	// A lens's colour fringe, only towards the edges.
	vec2 fringe = fromMiddle * edge * u_postLens.z;
	vec3 color;
	color.r = texture2D(s_scene, uv + fringe).r;
	color.g = texture2D(s_scene, uv).g;
	color.b = texture2D(s_scene, uv - fringe).b;
	// (The world round the eye just changed for another -- out of space into a site's sky: the picture before it, dissolving out.)
	if (u_postDissolve.x > 0.0)
	{
		color = mix(color, texture2D(s_held, uv).rgb, u_postDissolve.x);
	}
	// (And softened: nothing is quite sharp through water.)
	if (water > 0.0)
	{
		vec2 px = u_postTexel.xy * 1.6;
		vec3 around = texture2D(s_scene, uv + vec2(px.x, 0.0)).rgb + texture2D(s_scene, uv - vec2(px.x, 0.0)).rgb +
		             texture2D(s_scene, uv + vec2(0.0, px.y)).rgb + texture2D(s_scene, uv - vec2(0.0, px.y)).rgb;
		color = mix(color, around * 0.25, 0.45 * water);
	}
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
		// (From as far out as it begins -- from a cabin, past its walls: the storm seen through its windows, not in the cabin.)
		float from = min(u_airAt.w, far);
		float step = max(min(far, u_air.y + from) - from, 0.0) / 8.0;
		// (Started a different way into the first step each pixel, so the steps do not show as bands.)
		float t = from + step * fract(52.9829189 * fract(0.06711056 * gl_FragCoord.x + 0.00583715 * gl_FragCoord.y));
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

	// The cloud deck: from the eye along the picture's line through the slab of sky it fills, as far as the picture shows (the
	// sky: as far as it goes), in steps -- each where there is cloud taking its share of the light and giving its own, lit by the
	// sun through what of the cloud lies towards it, brighter towards the sun (its silver lining), and by the sky. From the ground
	// a sky of it; from above, a sea of it over the land; going down, through it.
	if (u_cloud.z > 0.0)
	{
		float depth = texture2DLod(s_depth, uv, 0.0).r;
		vec2 ndc = vec2(uv.x * 2.0 - 1.0, u_postFlip.x > 0.5 ? uv.y * 2.0 - 1.0 : 1.0 - uv.y * 2.0);
		vec4 middle = mul(u_aoInvProj, vec4(ndc, 0.5, 1.0));
		vec4 there = mul(u_aoInvProj, vec4(ndc, u_aoParams.w > 0.5 ? depth * 2.0 - 1.0 : depth, 1.0));
		float far = abs(depth - u_aoParams.z) < 1.0e-7 ? 1.0e6 : length(there.xyz / there.w);
		vec3 way = normalize(mul(u_airView, vec4(normalize(middle.xyz / middle.w), 0.0)).xyz);
		vec3 eye = u_cloudAt.xyz;
		float enter = 0.0;
		float leave = 0.0;
		if (abs(way.y) > 1.0e-4)
		{
			float a = (u_cloud.x - eye.y) / way.y;
			float b = (u_cloud.y - eye.y) / way.y;
			enter = max(min(a, b), 0.0);
			// (Out to well past where the shuttle comes in from: forty-odd kilometres off and thirty up, the deck is further than sixty
			// away along the line down to it, and it came on all at once partway down.)
			leave = min(max(a, b), min(far, 150000.0));
		}
		else if (eye.y > u_cloud.x && eye.y < u_cloud.y)
		{
			leave = min(far, 30000.0);
		}
		// Looking near level at a deck too far off to reach -- under it towards the horizon, or over it the same way -- all of it far
		// cloud, as the distant part of what is reached is (a bright band of clear sky along the horizon otherwise) -- and, under it, any
		// sky seen level or below level past the edge of the ground and the sea (a bright line along the horizon otherwise).
		bool sky = far > 1.0e5;
		if (sky && leave <= enter && (eye.y < u_cloud.x || (eye.y > u_cloud.y && way.y < 1.0e-4)))
		{
			vec3 farCloud = u_cloudAmbient.rgb * 0.8 + u_cloudLight.rgb * u_cloudSun.w * 0.1;
			color = mix(color, farCloud, u_cloud.z);
		}
		if (leave > enter)
		{
			float step = (leave - enter) / 24.0;
			float t = enter + step * fract(52.9829189 * fract(0.06711056 * gl_FragCoord.x + 0.00583715 * gl_FragCoord.y));
			float through = 1.0;
			vec3 gathered = vec3_splat(0.0);
			float towardSun = dot(way, u_cloudSun.xyz);
			float lining = 0.55 + 1.6 * pow(max(towardSun, 0.0), 6.0) + 0.25 * towardSun;
			for (int i = 0; i < 24; ++i)
			{
				vec3 p = eye + way * t;
				float d = CloudDensity(p, 1.0);
				if (d > 0.002)
				{
					// The sun through what of it lies towards the sun, from this point; the sky's light more at the top.
					float shade = CloudDensity(p + u_cloudSun.xyz * 140.0, 0.0) + 0.7 * CloudDensity(p + u_cloudSun.xyz * 420.0, 0.0);
					float sunlit = exp(-shade * u_cloud.w * 300.0);
					float high = clamp((p.y - u_cloud.x) / max(u_cloud.y - u_cloud.x, 1.0), 0.0, 1.0);
					vec3 lit = u_cloudLight.rgb * u_cloudSun.w * sunlit * lining * 0.32 + u_cloudAmbient.rgb * (0.55 + 0.45 * high);
					float keep = exp(-d * u_cloud.w * step);
					gathered += through * (1.0 - keep) * lit;
					through *= keep;
					if (through < 0.02)
					{
						break;
					}
				}
				t += step;
			}
			// Far off, lost in the haze of the air between -- into the grey of more cloud, as much of the sky as it covers, not into the
			// clear sky behind it: from under a storm's deck that left a bright line along the whole horizon.
			float haze = smoothstep(18000.0, 50000.0, enter);
			vec3 farCloud = u_cloudAmbient.rgb * 0.8 + u_cloudLight.rgb * u_cloudSun.w * 0.1;
			color = mix(color * through + gathered, mix(color, farCloud, u_cloud.z), haze);
		}
	}

	// The air burning round a craft going in, seen out of its windows: tongues of it streaming out from the way it flies, torn back
	// along the flow and flickering, thin straight ahead (the shock seen face on) and thickest round the edges and under it, pink
	// and violet where it is thin, orange, and near white where hottest -- a haze the world is seen through between them, never a
	// hole and never a ring. Only past what is close round the eye (the cabin is not on fire). Held closed while the world outside
	// is changed for the site's (y).
	if (u_postFire.x > 0.0 || u_postFire.y > 0.0)
	{
		float depth = texture2DLod(s_depth, uv, 0.0).r;
		vec2 ndc = vec2(uv.x * 2.0 - 1.0, u_postFlip.x > 0.5 ? uv.y * 2.0 - 1.0 : 1.0 - uv.y * 2.0);
		vec4 there = mul(u_aoInvProj, vec4(ndc, u_aoParams.w > 0.5 ? depth * 2.0 - 1.0 : depth, 1.0));
		float far = abs(depth - u_aoParams.z) < 1.0e-7 ? 1.0e6 : length(there.xyz / there.w);
		float outside = smoothstep(4.5, 9.0, far);
		vec2 d = (uv - u_postFire.zw) * vec2(u_postTexel.z / max(u_postTexel.w, 1.0), 1.0);
		float r = length(d);
		vec2 around = d / max(r, 1.0e-4);
		float t = u_postLens.w;
		// A boiling glow over the whole view -- its noise worked in the picture itself, so nothing converges anywhere -- with broad
		// tongues carried out from where it flies (from which way round, as a direction, and how far out: no seam round it).
		float boil = AirNoise(vec3(d * 2.2, t * 0.8)) * 0.6 + AirNoise(vec3(d * 5.1 + vec2(4.1, 1.3), t * 1.6)) * 0.4;
		float flow = AirNoise(vec3(around * 1.6 + vec2(3.0, 5.0), r * 2.6 - t * 2.2));
		// (The flow fades out straight ahead, where which way round has no meaning: a pinch there otherwise.)
		float tongue = smoothstep(0.3, 0.8, boil * 0.55 + mix(boil, flow, smoothstep(0.0, 0.2, r)) * 0.55);
		float edge = smoothstep(0.05, 0.7, r);
		float low = u_postFlip.x > 0.5 ? 1.0 - uv.y : uv.y;
		float under = mix(0.45, 1.0, smoothstep(0.1, 1.0, low));
		float glow = u_postFire.x * (0.45 + 0.55 * tongue) * mix(0.6, 1.0, edge) * under * (0.9 + 0.1 * sin(t * 27.0 + boil * 6.0));
		vec3 fire = mix(vec3(0.8, 0.2, 0.55), vec3(1.0, 0.36, 0.06), smoothstep(0.1, 0.5, glow));
		fire = mix(fire, vec3(1.0, 0.85, 0.55), smoothstep(0.8, 1.0, glow));
		float cover = max(clamp(glow * 1.5, 0.0, 0.9), u_postFire.y);
		fire = mix(fire, vec3(1.0, 0.42, 0.1), u_postFire.y * (1.0 - glow));
		// As bright as the eye is set for the light: it is the brightest thing there is, whatever the world beyond is doing.
		float setFor = u_postAdapt.y > 0.0 ? exp(texture2D(s_adapt, vec2_splat(0.5)).r) : u_postAdapt.x;
		color = mix(color, fire * setFor * (4.0 + 10.0 * glow), cover * outside);
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
