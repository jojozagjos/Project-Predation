$input v_worldPos, v_texcoord0

#include <bgfx_shader.sh>

// The water's surface (WaterRenderer), blended over the picture already drawn: the sky in it at a glancing look and the
// water itself looking down into it, a glint of the sun, small waves moving on it with the wind, rings where rain falls
// on it; clear in the shallows (the ground showing through), darker and thicker the deeper it is, foam along the shore; and
// fogged as everything is. Not drawn where the ground is above it (how deep it is: v_texcoord0.x, from the ground under
// each corner).

uniform vec4 u_waterEye;       // xyz = the eye, w = seconds (the waves' clock)
uniform vec4 u_waterSky;       // rgb = the sky overhead, w = how hard it is raining (0 to 1)
uniform vec4 u_waterHorizon;   // rgb = the sky at the horizon, w = how hard the wind blows (metres a second)
uniform vec4 u_waterSun;       // xyz = towards the sun, w = how much of it reaches the water (0 at night, under cloud)
uniform vec4 u_waterSunColor;  // rgb = its light, w = 1 drawing linear light (for post-processing), 0 finished here
uniform vec4 u_waterAmbient;   // rgb = the light from the sky over everything, w = exposure (finished here)
uniform vec4 u_waterFog;       // rgb = the fog's colour, w = how far out in the open air the eye is (0 to 1)
uniform vec4 u_waterFogParams; // x = where the fog starts, y = where it is all fog, z = lightning flashing (Environment::lightning)
uniform vec4 u_waterTint;      // rgb = the water's own colour, deep; w = how clear it is (metres to see a third of the way)
uniform vec4 u_waterTorchAt;    // xyz = the torch, w = how far it reaches (0: none)
uniform vec4 u_waterTorchAim;   // xyz = which way it points, w = the cosine of its cone's edge
uniform vec4 u_waterTorchLight; // rgb = its light, w = the cosine of its cone's full middle
uniform vec4 u_waterKind;       // x = what it is: 0 water, 1 lava, 2 acid, 3 ice

float WaterHash(vec2 p)
{
	p = fract(p * vec2(0.1031, 0.1030));
	p += dot(p, p.yx + 33.33);
	return fract((p.x + p.y) * p.x);
}

float WaterNoise(vec2 p)
{
	vec2 i = floor(p);
	vec2 f = fract(p);
	f = f * f * (vec2_splat(3.0) - f * 2.0);
	float a = WaterHash(i);
	float b = WaterHash(i + vec2(1.0, 0.0));
	float c = WaterHash(i + vec2(0.0, 1.0));
	float d = WaterHash(i + vec2(1.0, 1.0));
	return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// Plates, as a cooling crust breaks into: how near a place is to the line between the nearest two of them (0 on it), from
// a point scattered in each cell of a grid.
float WaterPlates(vec2 p)
{
	vec2 cell = floor(p);
	float first = 8.0;
	float second = 8.0;
	for (int y = -1; y <= 1; ++y)
	{
		for (int x = -1; x <= 1; ++x)
		{
			vec2 c = cell + vec2(float(x), float(y));
			vec2 seed = c + vec2(WaterHash(c * 1.31 + vec2_splat(2.7)), WaterHash(c * 0.73 + vec2_splat(8.1)));
			float d = length(p - seed);
			second = d < first ? first : min(second, d);
			first = min(first, d);
		}
	}
	return second - first;
}

// How much of a pattern of `frequency` (cycles a metre) is left where one pixel covers `pixel` metres: all of it while it
// is several pixels across, none once it is finer than about two -- below that it cannot be drawn, only shimmer.
float WaterKeep(float frequency, float pixel)
{
	return 1.0 - smoothstep(0.18, 0.45, frequency * pixel);
}

// How the surface leans here: long swells and short chop, moving with the wind (more and steeper the harder it blows),
// and the rings of drops landing on it in rain. Each only while it is big enough on the screen to be drawn (`pixel`: the
// metres one pixel covers there).
vec2 WaterSlope(vec2 p, float t, float wind, float rain, float pixel)
{
	vec2 slope = vec2_splat(0.0);
	float strength = 0.25 + 0.06 * min(wind, 15.0);
	// A few long waves, each its own way.
	const int kWaves = 4;
	for (int i = 0; i < kWaves; ++i)
	{
		float fi = float(i);
		float angle = fi * 1.7 + 0.4;
		vec2 way = vec2(cos(angle), sin(angle));
		float wavelength = 9.0 / (1.0 + fi * 0.9);
		float k = 6.2831853 / wavelength;
		float speed = sqrt(9.8 / k);
		float height = 0.05 * wavelength / 9.0;
		slope += way * (k * height * cos(dot(p, way) * k - t * speed)) * strength * WaterKeep(1.0 / wavelength, pixel);
	}
	// And chop: two layers of noise drifting past each other.
	float e = 0.08;
	vec2 q = p * 1.7 + vec2(t * 0.6, t * 0.35);
	vec2 r = p * 3.1 - vec2(t * 0.45, -t * 0.7);
	float keepQ = WaterKeep(1.7, pixel);
	float keepR = WaterKeep(3.1, pixel) * 0.5;
	if (keepQ > 0.0)
	{
		float n0 = WaterNoise(q) * keepQ + WaterNoise(r) * keepR;
		float nx = WaterNoise(q + vec2(e, 0.0)) * keepQ + WaterNoise(r + vec2(e, 0.0)) * keepR;
		float nz = WaterNoise(q + vec2(0.0, e)) * keepQ + WaterNoise(r + vec2(0.0, e)) * keepR;
		slope += vec2(nx - n0, nz - n0) / e * 0.06 * strength;
	}
	// Rain: in cells a third of a metre across, a drop landing now and then and its ring going out.
	float keepRain = WaterKeep(6.0, pixel);
	if (rain > 0.01 && keepRain > 0.0)
	{
		vec2 cellSize = vec2_splat(0.35);
		vec2 cell = floor(p / cellSize);
		vec2 inCell = fract(p / cellSize) - vec2_splat(0.5);
		float h = WaterHash(cell * 1.37 + vec2_splat(7.1));
		float age = fract(t * 1.3 + h);
		vec2 off = (vec2(WaterHash(cell + vec2_splat(3.3)), WaterHash(cell + vec2_splat(9.7))) - vec2_splat(0.5)) * 0.5;
		vec2 fromDrop = inCell - off;
		float d = length(fromDrop);
		float ring = sin((d - age * 0.45) * 60.0) * exp(-abs(d - age * 0.45) * 30.0) * (1.0 - age);
		slope += (d > 1.0e-4 ? fromDrop / d : vec2_splat(0.0)) * ring * 0.6 * rain * step(h, 0.3 + 0.7 * rain) * keepRain;
	}
	return slope;
}

void main()
{
	float depth = v_texcoord0.x;
	if (depth <= 0.0)
	{
		discard;
	}
	vec3 toEye = u_waterEye.xyz - v_worldPos;
	float far = length(toEye);
	vec3 V = toEye / max(far, 1.0e-4);
	float t = u_waterEye.w;
	// How much of the surface one pixel covers here, along whichever way it is longest (far off and at a glancing look, a
	// long way): what is finer than that is left out rather than shimmering (WaterKeep).
	vec2 footprint = fwidth(v_worldPos.xz);
	float pixel = max(footprint.x, footprint.y);
	float near = 1.0 - smoothstep(60.0, 400.0, far);
	vec2 slope = WaterSlope(v_worldPos.xz, t, u_waterHorizon.w, u_waterSky.w, pixel);
	vec3 N = normalize(vec3(-slope.x, 1.0, -slope.y));

	// The sky in it, more of it the more glancing the look.
	float facing = max(dot(N, V), 0.0);
	float fresnel = 0.02 + 0.98 * pow(1.0 - facing, 5.0);
	vec3 R = reflect(-V, N);
	vec3 sky = mix(u_waterHorizon.rgb, u_waterSky.rgb, pow(clamp(R.y, 0.0, 1.0), 0.45));
	// A lightning flash: the whole sky in it lit at once.
	float flash = max(u_waterFogParams.z, 0.0);
	sky += vec3(0.75, 0.8, 1.0) * flash * 1.5;
	vec3 L = normalize(u_waterSun.xyz);
	vec3 glint = u_waterSunColor.rgb * pow(max(dot(R, L), 0.0), 600.0) * 40.0 * u_waterSun.w;

	// The water itself: its colour lit by the sky and a little of the sun through it; the ground showing through it in the
	// shallows, the more the clearer it is and the shallower.
	vec3 body = u_waterTint.rgb * (u_waterAmbient.rgb + u_waterSunColor.rgb * u_waterSun.w * 0.15 * max(L.y, 0.0));
	body += u_waterTint.rgb * vec3(0.75, 0.8, 1.0) * flash * 0.6;
	// The torch on it: its light down into the water, and its glint off the surface.
	vec3 torchGlint = vec3_splat(0.0);
	if (u_waterTorchAt.w > 0.0)
	{
		vec3 toTorch = u_waterTorchAt.xyz - v_worldPos;
		float d = length(toTorch);
		vec3 Lt = toTorch / max(d, 1.0e-3);
		float cone = clamp((dot(-Lt, normalize(u_waterTorchAim.xyz)) - u_waterTorchAim.w) / max(u_waterTorchLight.w - u_waterTorchAim.w, 1.0e-4), 0.0, 1.0);
		float reach = cone * cone * clamp(1.0 - pow(d / u_waterTorchAt.w, 4.0), 0.0, 1.0) / max(d * d, 1.0);
		body += u_waterTint.rgb * u_waterTorchLight.rgb * reach * 2.0;
		torchGlint = u_waterTorchLight.rgb * reach * pow(max(dot(R, Lt), 0.0), 120.0) * 4.0;
	}
	float through = exp(-depth / max(u_waterTint.w, 0.1));
	float alpha = mix(0.96, 0.15, through);
	vec3 colour = mix(body, sky, fresnel) + glint + torchGlint;
	alpha = max(alpha, fresnel);

	// Foam along the shore, broken and moving with the water.
	// (The shore's foam washing in and out with the swell, not standing in one line.)
	float wash = 0.3 + 0.2 * sin(t * 0.9 + WaterNoise(v_worldPos.xz * 0.15) * 6.0);
	float shore = 1.0 - smoothstep(0.0, wash, depth);
	float broken = mix(0.5, WaterNoise(v_worldPos.xz * 2.3 + vec2(t * 0.3, -t * 0.2)), WaterKeep(2.3, pixel)) * WaterNoise(v_worldPos.xz * 0.6 - vec2(t * 0.1, 0.0));
	float foam = shore * smoothstep(0.25, 0.6, broken + shore * 0.15) * 0.65;
	colour = mix(colour, (u_waterAmbient.rgb + u_waterSunColor.rgb * u_waterSun.w * 0.3) * 0.8, foam);
	alpha = max(alpha, foam * 0.8);
	// Whitecaps in a strong wind: foam breaking on the crests, more the harder it blows.
	float caps = smoothstep(0.78, 0.95, WaterNoise(v_worldPos.xz * 0.7 + vec2(t * 0.5, t * 0.2))) * smoothstep(8.0, 18.0, u_waterHorizon.w) * near;
	colour = mix(colour, (u_waterAmbient.rgb + u_waterSunColor.rgb * u_waterSun.w * 0.3) * 0.75, caps * 0.6);
	alpha = max(alpha, caps * 0.6);
	// And the very edge thin, so the shore does not end on a line.
	alpha *= smoothstep(0.0, 0.06, depth);

	// Acid: thick and opaque, its own sickly colour more than the sky's (which it gives back yellowed), a little glow of its
	// own in the murk, its scum and froth yellow.
	if (u_waterKind.x > 1.5 && u_waterKind.x < 2.5)
	{
		vec3 scum = (u_waterAmbient.rgb + u_waterSunColor.rgb * u_waterSun.w * 0.3) * vec3(0.7, 0.72, 0.32);
		colour = mix(body + vec3(0.014, 0.02, 0.0), sky * vec3(0.75, 0.85, 0.4), fresnel * 0.5) + glint * 0.6 + torchGlint;
		colour = mix(colour, scum, max(foam, caps * 0.6));
		alpha = max(max(mix(0.97, 0.45, through), fresnel * 0.5), foam * 0.8) * smoothstep(0.0, 0.06, depth);
	}
	// Lava: a crust of cooled rock in plates, drifting slowly as it is carried, and between them and wherever it is thin --
	// further from its edge, where it is hottest -- the molten rock glowing through, flickering as it churns.
	else if (u_waterKind.x > 0.5 && u_waterKind.x < 1.5)
	{
		vec2 p = v_worldPos.xz;
		float tt = t * 0.04;
		// Its own pixel: across the look and along it together (pixel is the longer of the two), so a field of it seen at a
		// glancing angle keeps its plates further out; it moves too slowly to shimmer as the waves would.
		float area = sqrt(max(footprint.x * footprint.y, 1.0e-8));
		vec2 warp = vec2(WaterNoise(p * 0.05 + vec2(tt, 0.0)), WaterNoise(p * 0.05 - vec2(0.0, tt) + vec2_splat(5.0)));
		float plates = WaterNoise(p * 0.12 + warp * 3.0 + vec2(tt * 2.0, tt));
		// The fine churn and the cracks' wander only while big enough on the screen; the cracks no thinner than a pixel and a
		// half, dimmed by as much as they are widened, so far off they are a glow along the seams, not glitter.
		float keepFine = WaterKeep(0.9, area);
		float rough = WaterNoise(p * 0.9 + warp * 5.0 - vec2_splat(tt * 3.0));
		float fine = mix(0.5, rough, keepFine);
		float heat = smoothstep(0.0, 2.5, depth);
		vec2 wander = vec2(fine, mix(0.5, WaterNoise(p * 0.7 + vec2_splat(4.0)), WaterKeep(0.7, area))) * 0.35;
		float seam = WaterPlates(p * 0.22 + warp * 1.5 + vec2(tt * 1.5, tt * 0.7) + wander);
		float width = 0.02 + 0.09 * fine * fine * (0.4 + heat);
		float drawnWidth = max(width, area * 0.22 * 1.5);
		float crack = (1.0 - smoothstep(0.0, drawnWidth, seam)) * (width / drawnWidth) * smoothstep(0.15, 0.6, plates + 0.3 * heat);
		float crusted = mix(0.95, 0.62, heat);
		float molten = max(crack * (0.4 + 0.6 * heat), smoothstep(crusted, crusted + 0.12, rough * 0.5 + plates * 0.5));
		// Far off, where the plates are only a few pixels, what glows on average and nothing finer: a field of bright flecks
		// there was all that could be seen of it, and it glittered.
		float detail = WaterKeep(0.25, area);
		// (Still in patches as the big plates lie: those are metres across and are drawn as far as it can be seen.)
		float patches = 0.25 + 1.6 * smoothstep(0.45, 0.8, plates + 0.15 * warp.x);
		molten = mix((0.015 + 0.035 * heat) * patches, molten, detail);
		float churn = 0.8 + 0.2 * sin(t * 0.7 + plates * 12.0);
		// (Far off, the dull red of most of it rather than the white heat of the brightest.)
		vec3 hot = mix(vec3(1.6, 0.22, 0.02), vec3(4.5, 1.5, 0.3), fine * heat * detail) * churn * (0.5 + 0.9 * heat);
		vec3 lit = u_waterAmbient.rgb + u_waterSunColor.rgb * u_waterSun.w * max(L.y, 0.0) * 0.6;
		vec3 crust = vec3(0.03, 0.028, 0.027) * lit * (0.7 + 0.6 * fine) + hot * 0.006;
		colour = mix(crust, hot, molten);
		alpha = smoothstep(0.0, 0.08, depth);
	}
	// Ice: white and frosted where it is thin, bluer where the water under it is deeper, cracked across in long lines, the
	// sky only a little in it; solid, so no waves and no foam.
	else if (u_waterKind.x > 2.5)
	{
		vec2 p = v_worldPos.xz;
		float frost = WaterNoise(p * 0.08) * 0.6 + mix(0.5, WaterNoise(p * 0.6), WaterKeep(0.6, pixel)) * 0.4;
		// The cracks no thinner than a pixel and a half, dimmed as they are widened (as the lava's).
		float wideA = max(0.02, pixel * 0.31 * 1.5);
		float wideB = max(0.015, pixel * 1.3 * 1.5);
		float cracks = (1.0 - smoothstep(0.0, wideA, abs(WaterNoise(p * 0.31) - 0.5))) * 0.7 * (0.02 / wideA) +
		               (1.0 - smoothstep(0.0, wideB, abs(WaterNoise(p * 1.3 + vec2_splat(3.0)) - 0.5))) * 0.3 * (0.015 / wideB);
		vec3 ice = mix(vec3(0.4, 0.53, 0.63), vec3(0.78, 0.84, 0.9), smoothstep(0.3, 0.8, frost));
		ice *= mix(1.0, 0.7, smoothstep(0.5, 4.0, depth)) * (1.0 - 0.35 * cracks);
		vec3 lit = u_waterAmbient.rgb + u_waterSunColor.rgb * u_waterSun.w * max(L.y, 0.0);
		vec3 level = vec3(0.0, 1.0, 0.0);
		float sheen = 0.02 + 0.98 * pow(1.0 - max(dot(level, V), 0.0), 5.0);
		vec3 Rf = reflect(-V, level);
		vec3 skyIn = mix(u_waterHorizon.rgb, u_waterSky.rgb, pow(clamp(Rf.y, 0.0, 1.0), 0.45));
		colour = mix(ice * lit, skyIn, sheen * 0.35) + u_waterSunColor.rgb * pow(max(dot(Rf, L), 0.0), 200.0) * 6.0 * u_waterSun.w * (1.0 - frost);
		colour += ice * vec3(0.75, 0.8, 1.0) * flash * 0.8 + torchGlint * 0.5;
		alpha = smoothstep(0.0, 0.04, depth) * 0.97;
	}

	// Fogged as everything is.
	float fogAmount = clamp((far - u_waterFogParams.x) / max(u_waterFogParams.y - u_waterFogParams.x, 1.0e-4), 0.0, 1.0);
	colour = mix(colour, u_waterFog.rgb, fogAmount);

	if (u_waterSunColor.w < 0.5)
	{
		colour *= max(u_waterAmbient.w, 0.0);
		colour = clamp((colour * (2.51 * colour + 0.03)) / (colour * (2.43 * colour + 0.59) + 0.14), 0.0, 1.0);
		colour = pow(colour, vec3_splat(1.0 / 2.2));
	}
	gl_FragColor = vec4(colour, clamp(alpha, 0.0, 1.0));
}
