$input v_worldPos, v_texcoord0, v_wave, v_flow

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
uniform vec4 u_waterFogParams; // x = where the fog starts, y = where it is all fog, z = lightning flashing (Environment::lightning),
                               // w = how many rings there are on it (u_waterRipples)
uniform vec4 u_waterTint;      // rgb = the water's own colour, deep; w = how clear it is (metres to see a third of the way)
uniform vec4 u_waterTorchAt;    // xyz = the torch, w = how far it reaches (0: none)
uniform vec4 u_waterTorchAim;   // xyz = which way it points, w = the cosine of its cone's edge
uniform vec4 u_waterTorchLight; // rgb = its light, w = the cosine of its cone's full middle
uniform vec4 u_waterKind;       // x = what it is: 0 water, 1 lava, 2 acid, 3 ice; y = 1 seen from under it; w = 1 under a roof
uniform vec4 u_waterLift;       // y = how far the day reaches down through the water, z = how hazy the medium the eye is in, w = 1 the eye in water (as fs_mesh has them)
uniform vec4 u_waterRipples[16]; // rings going out from where something moved in it: x, z where; how long ago (seconds); how hard
uniform vec4 u_waterPosts[8];    // what stands up through it (a platform's legs): x, z where, how wide (radius); w = 1 one there
uniform vec4 u_waterLamps[16];  // the lamps near by, two each: where and how far it reaches (none: 0); its light and how big it is
uniform vec4 u_waves[10];       // the big waves, as vs_water has them (Waves): each its way (xy), length (z), height (w)
uniform vec4 u_waveParams;      // x = how many, y = how sharp their crests

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
// The chop's two layers of noise at a place: drifting past each other, each turned off the world's axes -- its lattice laid along
// them showed as a grid over the water, out where the eye takes in many cells at once.
float WaterChop(vec2 p, float t, float keepQ, float keepR)
{
	vec2 q = vec2(p.x * 0.825 - p.y * 0.565, p.x * 0.565 + p.y * 0.825) * 1.7 + vec2(t * 0.6, t * 0.35);
	vec2 r = vec2(p.x * 0.454 + p.y * 0.891, -p.x * 0.891 + p.y * 0.454) * 3.1 - vec2(t * 0.45, -t * 0.7);
	return WaterNoise(q) * keepQ + WaterNoise(r) * keepR;
}

vec2 WaterSlope(vec2 p, float t, float wind, float rain, float pixel)
{
	vec2 slope = vec2_splat(0.0);
	float strength = 0.25 + 0.06 * min(wind, 15.0);
	// A few long waves, each its own way (six, at angles that never line up: four at even steps drew a lattice over the sea).
	const int kWaves = 6;
	for (int i = 0; i < kWaves; ++i)
	{
		float fi = float(i);
		float angle = fi * 2.399 + 0.4;
		vec2 way = vec2(cos(angle), sin(angle));
		float wavelength = 9.0 / (1.0 + fi * 0.73);
		float k = 6.2831853 / wavelength;
		float speed = sqrt(9.8 / k);
		float height = 0.05 * wavelength / 9.0;
		slope += way * (k * height * cos(dot(p, way) * k - t * speed)) * strength * WaterKeep(1.0 / wavelength, pixel);
	}
	// And chop: two layers of noise drifting past each other, each turned off the world's axes (its lattice laid along them showed
	// as a grid over the water, out where the eye takes in many cells at once).
	float e = 0.05;
	float keepQ = WaterKeep(1.7, pixel);
	float keepR = WaterKeep(3.1, pixel) * 0.5;
	if (keepQ > 0.0)
	{
		float n0 = WaterChop(p, t, keepQ, keepR);
		float nx = WaterChop(p + vec2(e, 0.0), t, keepQ, keepR);
		float nz = WaterChop(p + vec2(0.0, e), t, keepQ, keepR);
		slope += vec2(nx - n0, nz - n0) / e * (0.06 / 1.7) * strength; // (by the world, not the first layer's finer grain: as strong as it was)
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

// Rings going out from where something moved in the water (u_waterRipples): a step, a stroke, somebody wading through it --
// a short train of little waves running out from it, slowing and spreading and dying away over a few seconds, lower the
// further they have gone.
vec2 RippleSlope(vec2 p, float pixel)
{
	vec2 slope = vec2_splat(0.0);
	float keep = WaterKeep(3.5, pixel);
	if (keep <= 0.0)
	{
		return slope;
	}
	for (int i = 0; i < 16; ++i)
	{
		if (float(i) < u_waterFogParams.w)
		{
			vec4 ring = u_waterRipples[i];
			vec2 from = p - ring.xy;
			float d = length(from);
			float age = ring.z;
			float front = 0.08 + 0.55 * age - 0.05 * age * age;
			float wave = sin((d - front) * 24.0) * exp(-abs(d - front) * (7.0 - 3.0 * min(age, 1.5))) * exp(-age * 1.2) * ring.w / (1.0 + d * 1.5);
			slope += (d > 1.0e-4 ? from / d : vec2_splat(0.0)) * wave * 1.4;
		}
	}
	return slope * keep;
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
	// (The waves by the pixel's area, not its longest side: at a low look the longest side grows fast with distance and the
	// waves all went within a short band of it -- a line across the water.)
	float waveArea = sqrt(max(footprint.x * footprint.y, 1.0e-8));
	// On a stream (its way of flowing in v_texcoord0.y, a hundred over), the ripples and the foam carried on down it -- each copy
	// of them only a few metres, fading out as another fades in behind it, so none is carried far: carried on and on, it went
	// kilometres in an hour, the pattern came apart into streaks, and each stretch's way of flowing a little different from the
	// next drew a zigzag between them. (Each patch of it at its own moment, and faded in and out gently: all at once and by a
	// straight ramp, the whole stream was seen starting over every few seconds.)
	bool flowing = v_texcoord0.y > 50.0;
	vec2 way = vec2(cos(v_texcoord0.y - 100.0), sin(v_texcoord0.y - 100.0));
	// (Gently across it, or the ripples are stretched where one patch's moment runs into the next's.)
	float stagger = WaterNoise(v_worldPos.xz * 0.06);
	float phase = fract(t * 0.2 + stagger);
	float phase2 = fract(t * 0.2 + stagger + 0.5);
	float mixed = 0.5 + 0.5 * cos(phase * 6.2831853);
	vec2 here = flowing ? v_worldPos.xz - way * phase * 4.5 : v_worldPos.xz;
	vec2 here2 = v_worldPos.xz - way * phase2 * 4.5 + vec2(7.3, 3.1);
	// (On top of the big waves' own lean, from vs_water.)
	// (Under a roof, water that has stood: still, as near a mirror as water gets -- no swell, no chop, only the rings of what
	// moves in it and the faintest slow stir. With the open water's waves on it, it was lumpy all over, the lamps in it broken
	// into specks.)
	bool still = u_waterKind.w > 0.5;
	vec2 slope = still ? (vec2(WaterNoise(v_worldPos.xz * 0.35 + vec2_splat(t * 0.03)), WaterNoise(v_worldPos.xz * 0.35 + vec2(5.2, 1.3) - vec2_splat(t * 0.025))) - vec2_splat(0.5)) * 0.012
	                   : WaterSlope(here, t, u_waterHorizon.w, u_waterSky.w, waveArea) + v_wave.xy;
	if (flowing && !still)
	{
		slope = mix(slope, WaterSlope(here2, t, u_waterHorizon.w, u_waterSky.w, waveArea) + v_wave.xy, mixed);
	}
	slope += RippleSlope(v_worldPos.xz, waveArea);
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

	// The big waves' crests here, worked out as they are drawn (u_waves: the same sums as vs_water), each pixel its own -- how high
	// over its middle the sea stands here (a share of as high as it goes) and how pinched together the surface is (where a crest is
	// folding over: the waves' Jacobian under one). Only those long enough on the screen to be seen. (Worked out per corner of the
	// grid the surface is drawn on, far off where the grid is coarse its foam came out in triangles, a row of teeth along the sea.)
	float crestUp = 0.0;
	float fold = 0.0;
	float heights = 0.0;
	if (u_waterKind.x < 0.5 && !flowing && !still && far < 700.0)
	{
		for (int i = 0; i < 10; ++i)
		{
			if (float(i) >= u_waveParams.x)
			{
				break;
			}
			vec4 wave = u_waves[i];
			float k = 6.2831853 / max(wave.z, 0.01);
			float phaseHere = k * dot(wave.xy, v_worldPos.xz) - sqrt(9.81 * k) * t + float(i) * 1.7;
			float height = wave.w * clamp(depth / max(wave.w * 3.0, 0.01), 0.0, 1.0) * WaterKeep(2.0 / max(wave.z, 0.01), waveArea);
			float s = sin(phaseHere);
			crestUp += height * s;
			fold += u_waveParams.y / max(k * wave.w * u_waveParams.x, 1.0e-4) * k * height * s;
			heights += height;
		}
	}
	float crest = heights > 1.0e-4 ? clamp(crestUp / heights, -1.0, 1.0) : 0.0;
	float pinch = fold / max(u_waveParams.y, 0.2);
	// The sun's glitter: a broad path of sparks across the waves to it -- every facet that catches it for a moment, twinkling --
	// round the sharp glint itself.
	// (Each a point of light a few centimetres across, round; far off, where they are too fine to draw, the path a soft sheen of their
	// average instead. As squares a span across they read as tiles on the water.)
	{
		vec2 at = v_worldPos.xz * 7.0;
		vec2 facet = floor(at);
		float lit = step(0.975, WaterHash(facet + vec2_splat(floor(t * 6.0 + WaterHash(facet * 0.37) * 9.0))));
		float speck = 1.0 - smoothstep(0.08, 0.22, length(fract(at) - vec2(0.5, 0.5) + (vec2(WaterHash(facet + vec2_splat(1.3)), WaterHash(facet + vec2_splat(4.1))) - vec2_splat(0.5)) * 0.4));
		float keep = WaterKeep(7.0, pixel);
		float lobe = pow(max(dot(R, L), 0.0), 35.0);
		glint += u_waterSunColor.rgb * u_waterSun.w * lobe * mix(0.25, lit * speck * 9.0, keep) * (still ? 0.0 : 1.0);
	}

	// The water itself: its colour lit by the sky and a little of the sun through it; the ground showing through it in the
	// shallows, the more the clearer it is and the shallower.
	vec3 body = u_waterTint.rgb * (u_waterAmbient.rgb + u_waterSunColor.rgb * u_waterSun.w * 0.15 * max(L.y, 0.0));
	body += u_waterTint.rgb * vec3(0.75, 0.8, 1.0) * flash * 0.6;
	// The light falling on the surface itself from the lamps and the torch, for what lies on it (the film, below).
	vec3 onTop = vec3_splat(0.0);
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
		onTop += u_waterTorchLight.rgb * reach * max(Lt.y, 0.0);
		// (And a broad sheen round that glint, as water gives a light near by back: what shows the rings on it in the beam.)
		torchGlint = u_waterTorchLight.rgb * reach * (pow(max(dot(R, Lt), 0.0), 120.0) * 4.0 + pow(max(dot(R, Lt), 0.0), 10.0) * 0.35);
	}
	// The lamps near by in it, as a mirror gives them back (u_waterLamps): each a bright smear where the look off the surface
	// meets it, broken up and moved by whatever moves the surface -- and a little of its light down into the water.
	for (int i = 0; i < 8; ++i)
	{
		vec4 lampAt = u_waterLamps[i * 2];
		if (lampAt.w > 0.0)
		{
			vec4 lampLight = u_waterLamps[i * 2 + 1];
			vec3 toLamp = lampAt.xyz - v_worldPos;
			float d = length(toLamp);
			vec3 Ll = toLamp / max(d, 1.0e-3);
			float fall = clamp(1.0 - pow(d / lampAt.w, 4.0), 0.0, 1.0) / max(d * d, lampLight.w * lampLight.w);
			// (Sharper the smaller it looks from here: a lamp far off a point, one near by a patch.)
			float sharp = clamp(d / lampLight.w, 4.0, 300.0) * 3.0;
			// (Not on still water, which has its mirror image instead, below: there the broad sheen was a great pale blob round it.)
			torchGlint += lampLight.rgb * fall * (pow(max(dot(R, Ll), 0.0), sharp) * (sharp + 2.0) * 0.05 + pow(max(dot(R, Ll), 0.0), 10.0) * 0.12) * (still ? 0.0 : 1.0);
			body += u_waterTint.rgb * lampLight.rgb * fall * 0.5;
			onTop += lampLight.rgb * fall * max(Ll.y, 0.0);
			// Still water a mirror: the lamp itself in it, as bright as its glowing face (about a fifth of what it gives out, as
			// LevelLights draws it), as big as it looks from here, wherever it is -- not only as far as its light reaches.
			if (still)
			{
				// (A lamp's face a bar wider than it is deep, as a ceiling strip is, seen from a way off; and round it the glow it
				// throws on the ceiling, faint.)
				float size = max(lampLight.w * 0.35 / max(d, 0.1), 0.004);
				vec3 across = normalize(cross(R, vec3(0.0, 1.0, 0.0)) + vec3(1.0e-5, 0.0, 0.0));
				vec3 upward = cross(across, R);
				vec3 miss = Ll - R * dot(R, Ll);
				float sideways = dot(miss, across) / (size * 2.2);
				float lengthways = dot(miss, upward) / (size * 0.8);
				float spread = sideways * sideways + lengthways * lengthways;
				float image = ((1.0 - smoothstep(0.5, 1.0, pow(abs(sideways), 4.0) + pow(abs(lengthways), 4.0))) + exp(-spread * 0.06) * 0.12) * step(0.0, dot(R, Ll));
				float fade = clamp(1.0 - (d - lampAt.w) / (lampAt.w * 3.0), 0.0, 1.0);
				torchGlint += lampLight.rgb * 0.22 * fresnel * fade * image;
			}
		}
	}
	// How much of the floor shows through: by how far the look goes through the water to reach it, not how deep it is --
	// looked across at a low angle, even shallow water is a long way through and hides its floor. By the depth alone the
	// floor showed far out, and where it falls away from its shelf, a straight edge across the sea.
	// (By the look's own slope, however low: held to no lower than a few degrees, the floor still came through faintly a
	// long way off -- and the straight line where the ground near the site meets the land round it with it.) And none at
	// all far off, where no water lets anybody see its floor.
	float path = depth / max(V.y, 0.002);
	float through = exp(-path / max(u_waterTint.w, 0.1)) * (1.0 - smoothstep(40.0, 150.0, far));
	// (Deep, nothing at all through it: a little let through showed the sea floor tens of metres down.)
	float alpha = mix(1.0, 0.15, through);
	// Under a roof, water that has stood a long time: what it gives back is the dim room over it, greyed, not a sky -- and on it a
	// thin film of dust drifted into streaks and rafts along where the water barely moves, dull where it lies (giving less
	// back), lit only by what lights the surface: the lamps over it, the torch. (Not seen from under it.) Darker than the floor
	// round it, never lighter: lit by the sky's light as it was, it stood out in a dark basement as pale blotches.
	float film = 0.0;
	if (still && u_waterKind.y < 0.5)
	{
		sky = vec3_splat(dot(sky, vec3_splat(0.333))) * vec3(1.0, 0.95, 0.85);
		vec2 drift = v_worldPos.xz + vec2(WaterNoise(v_worldPos.xz * 0.13), WaterNoise(v_worldPos.xz * 0.13 + vec2_splat(3.7))) * 2.5;
		// (Long and thin, as dust lies on still water: streaks along one way, broken across.)
		float streaks = WaterNoise(vec2(drift.x * 0.35 + drift.y * 0.9, drift.y * 0.35 - drift.x * 0.9) * vec2(0.6, 2.2));
		film = smoothstep(0.5, 0.8, streaks) * 0.45 + smoothstep(0.6, 0.9, WaterNoise(drift * 1.7)) * 0.2 * WaterKeep(1.7, pixel);
		film *= 0.6 + 0.4 * WaterNoise(drift * 0.25);
		film = clamp(film * 0.6, 0.0, 0.3);
	}
	vec3 colour = mix(body, sky, fresnel) + glint + torchGlint;
	alpha = max(alpha, fresnel);
	if (film > 0.0)
	{
		// (Dull and dark, lit or not: under a lamp, lit as brightly as it was, it showed as pale splotches on the water.)
		vec3 skin = (u_waterAmbient.rgb * 0.25 + onTop * 0.15) * vec3(0.36, 0.33, 0.28);
		colour = mix(colour, skin + torchGlint * 0.25, film);
		alpha = max(alpha, film * 0.9);
	}

	// Foam along the shore, broken and moving with the water.
	// (The shore's foam washing in and out with the swell, not standing in one line.)
	// (On a stream, both copies of it faded together as the ripples are: the foam carried by one copy only jumped back where
	// that copy started over, a line moving across the stream through its white edges.)
	float wash = 0.3 + 0.2 * sin(t * 0.9 + WaterNoise(here * 0.15) * 6.0);
	float broken = mix(0.5, WaterNoise(here * 2.3 + vec2(t * 0.3, -t * 0.2)), WaterKeep(2.3, pixel)) * WaterNoise(here * 0.6 - vec2(t * 0.1, 0.0));
	if (flowing)
	{
		float wash2 = 0.3 + 0.2 * sin(t * 0.9 + WaterNoise(here2 * 0.15) * 6.0);
		float broken2 = mix(0.5, WaterNoise(here2 * 2.3 + vec2(t * 0.3, -t * 0.2)), WaterKeep(2.3, pixel)) * WaterNoise(here2 * 0.6 - vec2(t * 0.1, 0.0));
		wash = mix(wash, wash2, mixed);
		broken = mix(broken, broken2, mixed);
	}
	// (None on water that has stood, under a roof: nothing washes up on anything there -- the foam round its steps and the rubble in
	// it were white splotches on a still pool.)
	float shore = still ? 0.0 : 1.0 - smoothstep(0.0, wash, depth);
	// And round whatever stands up through it -- a platform's legs: the sea broken white against each, more of it as each wave comes
	// up it and swirled away from it, trailing off downstream of the waves' way.
	if (!still)
	{
		for (int i = 0; i < 8; ++i)
		{
			vec4 post = u_waterPosts[i];
			if (post.w < 0.5)
			{
				continue;
			}
			vec2 from = v_worldPos.xz - post.xy;
			float d = length(from) - post.z;
			float surge = 0.6 + 0.4 * sin(t * 1.1 + float(i) * 2.3 + dot(from, vec2(0.7, 0.4)));
			float reach = (0.5 + 0.9 * min(v_wave.w, 2.5)) * surge;
			float ring = (1.0 - smoothstep(0.0, reach, d)) * smoothstep(-0.05, 0.05, d);
			float swirl = WaterNoise(vec2(atan2(from.y, from.x) * 3.0, d * 1.5 - t * 0.8) + post.xy) * 0.7 + 0.3;
			shore = max(shore, ring * swirl);
		}
	}
	float foam = shore * smoothstep(0.25, 0.6, broken + shore * 0.15) * 0.65;
	colour = mix(colour, (u_waterAmbient.rgb + u_waterSunColor.rgb * u_waterSun.w * 0.3) * 0.8, foam);
	alpha = max(alpha, foam * 0.8);
	// The sea's foam, out of the wind (from a fresh breeze up): whitecaps where the crests fold over and break -- frothing, broken up --
	// and behind them the foam they leave, drawn out into long streaks down the wind and drifting in old patches; more of all of it
	// the harder it blows.
	float caps = 0.0;
	if (heights > 1.0e-4)
	{
		float wind = u_waterHorizon.w;
		float breaking = smoothstep(5.0, 14.0, wind);
		float froth = WaterNoise(v_worldPos.xz * 1.3 + vec2(t * 0.4, -t * 0.3)) * 0.6 +
		              mix(0.5, WaterNoise(v_worldPos.xz * 4.1 - vec2(t * 0.2, t * 0.5)), WaterKeep(4.1, pixel)) * 0.4;
		float whitecap = smoothstep(0.55, 0.9, pinch) * breaking;
		whitecap *= smoothstep(0.3, 0.65, froth + whitecap * 0.35);
		vec2 windWay = normalize(u_waves[0].xy + vec2(1.0e-4, 0.0));
		vec2 local = vec2(dot(v_worldPos.xz, windWay), dot(v_worldPos.xz, vec2(-windWay.y, windWay.x)));
		float streak = WaterNoise(local * vec2(0.045, 0.9) - vec2(t * 0.6, 0.0)) * mix(0.5, WaterNoise(local * vec2(0.12, 2.3) + vec2(3.1, 1.7)), WaterKeep(2.3, pixel));
		// (Faint and broken, as wind-rows of foam are: drawn bold, they were white lines ruled across the sea.)
		float streaks = smoothstep(0.35, 0.55, streak) * smoothstep(0.35, 0.7, froth) * smoothstep(12.0, 24.0, wind) * 0.16;
		float patches = smoothstep(0.66, 0.86, WaterNoise(local * 0.08 - vec2(t * 0.3, 0.0)) * 0.6 + froth * 0.4) * breaking * 0.3 * (0.6 + 0.4 * crest);
		caps = max(whitecap, max(streaks, patches)) * (1.0 - smoothstep(250.0, 650.0, far));
		vec3 white = (u_waterAmbient.rgb + u_waterSunColor.rgb * u_waterSun.w * (0.25 + 0.55 * max(L.y, 0.0)) + onTop * 0.5) * 0.8;
		colour = mix(colour, white, caps * 0.85);
		alpha = max(alpha, caps * 0.85);
		// And the light through the crests: the sun behind a wave shining through its thin top, blue-green, as a breaking sea glows.
		float behind = pow(max(dot(normalize(vec3(-V.x, 0.0, -V.z) + vec3(0.0, 1.0e-4, 0.0)), normalize(vec3(L.x, 0.0, L.z) + vec3(0.0, 1.0e-4, 0.0))), 0.0), 3.0);
		// (Only the thin tops of the crests -- the sea's flanks are thick, and lit only by what the sky gives them.)
		float thin = smoothstep(0.55, 0.97, crest) * smoothstep(0.05, 0.6, heights);
		colour += u_waterTint.rgb * vec3(0.6, 1.5, 1.25) * u_waterSunColor.rgb * u_waterSun.w * behind * thin * 2.5 * (1.0 - fresnel) * smoothstep(-0.05, 0.25, L.y);
	}
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
		// How hot it is: by how far in from its edge, about -- cooler and crusted thick for the first stretch from the shore,
		// then the same everywhere (by depth all the way down, a shallow pool was all crust and a deep lake all glow: two
		// different things near and far).
		float heat = smoothstep(0.0, 0.8, depth);
		vec2 wander = vec2(fine, mix(0.5, WaterNoise(p * 0.7 + vec2_splat(4.0)), WaterKeep(0.7, area))) * 0.35;
		float seam = WaterPlates(p * 0.22 + warp * 1.5 + vec2(tt * 1.5, tt * 0.7) + wander);
		float width = 0.025 + 0.08 * fine * fine * (0.4 + heat);
		float drawnWidth = max(width, area * 0.22 * 1.5);
		float crack = (1.0 - smoothstep(0.0, drawnWidth, seam)) * (width / drawnWidth) * smoothstep(0.15, 0.6, plates + 0.3 * heat);
		// The crust along a crack hot too, a dull red fading into the plate (it is thinnest there).
		float halo = exp(-seam / max(drawnWidth * 3.5, 1.0e-3)) * (1.0 - crack) * heat;
		// And here and there the crust gone altogether: open molten rock, churning.
		float crusted = mix(0.95, 0.7, heat);
		float open = smoothstep(crusted, crusted + 0.1, rough * 0.5 + plates * 0.5);
		float molten = max(crack * (0.45 + 0.55 * heat), open);
		// Far off, where the plates are only a few pixels, what glows on average and nothing finer: a field of bright flecks
		// there was all that could be seen of it, and it glittered. (As much on average as near, so a lava lake far off and
		// a pool close by are one stuff; still in patches as the big plates lie: those are metres across.)
		float detail = WaterKeep(0.15, area);
		float patches = 0.3 + 1.5 * smoothstep(0.45, 0.85, plates + 0.15 * warp.x);
		molten = mix((0.03 + 0.045 * heat) * patches, molten, detail);
		halo *= detail;
		float churn = 0.8 + 0.2 * sin(t * 0.7 + plates * 12.0);
		// (Far off, the dull red of most of it rather than the white heat of the brightest.)
		vec3 hot = mix(vec3(1.6, 0.22, 0.02), vec3(4.5, 1.5, 0.3), fine * heat * detail) * churn * (0.5 + 0.9 * heat);
		vec3 lit = u_waterAmbient.rgb + u_waterSunColor.rgb * u_waterSun.w * max(L.y, 0.0) * 0.6;
		// The crust black and glassy: a little of the sky off it, glancing, and lit from below by its own heat.
		vec3 crust = vec3(0.03, 0.028, 0.027) * lit * (0.7 + 0.6 * fine) + hot * 0.006 + sky * fresnel * 0.12 + glint * 0.3
			+ vec3(0.5, 0.06, 0.005) * halo * (0.3 + 0.7 * churn) * 0.35;
		colour = mix(crust, hot, molten);
		// Along the shore, where the crust tears against the rock and the molten rock wells up between, a ragged line of
		// fire: what lights its banks, seen as it is.
		float shoreLine = (1.0 - smoothstep(0.03, 0.12, depth)) * smoothstep(0.5, 0.75, WaterNoise(p * 0.8 + vec2(tt * 4.0, 0.0))) * detail;
		colour = mix(colour, hot * 0.7, shoreLine * 0.6);
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

	// Falling or white water (a stream down a steep bed or over a cliff, v_flow.x): pouring down it, streaked and moving with it,
	// not a still sheet giving back the sky -- the same from behind.
	if (v_flow.x > 0.01 && u_waterKind.x < 0.5)
	{
		// Streaks long down it: by how far across it and how far down it a place is (v_flow.y, v_flow.z: laid along the stream
		// itself, so they follow it round its bends and down its falls rather than kinking where its pieces meet). Two copies of
		// them, each carried down a few metres and faded as the other comes in, so what is looked up in the noise stays near.
		float u = v_flow.y;
		float pour1 = fract(t * 0.7 + stagger);
		float pour2 = fract(t * 0.7 + stagger + 0.5);
		float blend = 0.5 + 0.5 * cos(pour1 * 6.2831853);
		float v1 = v_flow.z - pour1 * 6.0;
		float v2 = v_flow.z - pour2 * 6.0 + 17.0;
		float streak = mix(WaterNoise(vec2(u * 0.9, v1 * 0.12)) * 0.6 + WaterNoise(vec2(u * 2.4 + 3.0, v1 * 0.3)) * 0.5,
		                   WaterNoise(vec2(u * 0.9 + 5.0, v2 * 0.12)) * 0.6 + WaterNoise(vec2(u * 2.4 + 8.0, v2 * 0.3)) * 0.5, blend);
		vec3 white = (u_waterAmbient.rgb + u_waterSunColor.rgb * u_waterSun.w * 0.3) * 0.65;
		vec3 pour = mix(body * 1.5 + white * 0.25, white, smoothstep(0.35, 0.85, streak));
		float falling = smoothstep(0.2, 0.8, v_flow.x);
		colour = mix(colour, pour, falling);
		alpha = mix(alpha, 0.6 + 0.35 * smoothstep(0.3, 0.9, streak), falling);
	}

	// Frozen falling: hung in columns down it, as icicles grow -- ridged across (v_flow.y), each column paler on its edges
	// and bluer in its clear core, frosted where it is thin, not the flat ice's cracks.
	if (v_flow.x > 0.01 && u_waterKind.x > 2.5)
	{
		float u = v_flow.y;
		float down = v_flow.z;
		float columns = WaterNoise(vec2(u * 1.7, down * 0.04)) * 0.65 + WaterNoise(vec2(u * 4.5 + 2.0, down * 0.12)) * 0.35;
		float ridge = abs(fract(columns * 3.0) - 0.5) * 2.0;
		float clear = smoothstep(0.25, 0.75, WaterNoise(vec2(u * 0.6 + 7.0, down * 0.08)));
		vec3 core = vec3(0.32, 0.5, 0.62);
		vec3 rime = vec3(0.8, 0.86, 0.92);
		vec3 hung = mix(rime, core, clear * (1.0 - ridge) * 0.8) * mix(0.72, 1.08, ridge);
		vec3 lit = u_waterAmbient.rgb + u_waterSunColor.rgb * u_waterSun.w * (0.35 + 0.4 * ridge);
		float falling = smoothstep(0.2, 0.8, v_flow.x);
		colour = mix(colour, hung * lit + torchGlint * 0.5, falling);
		alpha = mix(alpha, 0.97, falling);
	}

	// From under it: the surface overhead bright with the light coming down through it, wavering with its waves, the sun a
	// smear where it is -- murk all the same (the fog, which under water is the water, has its colour). (And wherever this bit of
	// it is over the eye, whatever the eye is counted as: a crest seen from a trough is seen from under; drawn as if from over it,
	// it gave back the sky.)
	// (Still water only, a few metres over the eye at most: a stream or a fall up a slope over somebody in the open is not.)
	bool crestOver = u_waterKind.x < 0.5 && !flowing && v_flow.x < 0.01 && v_worldPos.y > u_waterEye.y + 0.02 && v_worldPos.y < u_waterEye.y + 3.0;
	if (u_waterKind.y > 0.5 || crestOver)
	{
		float waver = 0.85 + 0.3 * WaterNoise(v_worldPos.xz * 0.8 + slope * 3.0 + vec2(t * 0.4, -t * 0.3));
		// (Under a roof, no day over it to come down through it: only a little lighter than the murk.)
		colour = u_waterFog.rgb * mix(2.2, 1.15, u_waterKind.w) * waver + u_waterSunColor.rgb * u_waterSun.w * pow(max(dot(-V, L), 0.0), 40.0) * 0.6;
		alpha = 0.95;
	}

	// Fogged as everything is.
	float fogAmount = clamp((far - u_waterFogParams.x) / max(u_waterFogParams.y - u_waterFogParams.x, 1.0e-4), 0.0, 1.0);
	colour = mix(colour, u_waterFog.rgb, fogAmount);
	// From under it, the torch's beam in the water between the eye and this, as fs_mesh draws it in front of everything else and
	// fs_sky behind it: without it here, the beam stopped dead at the edge of anything in front of the surface overhead.
	if ((u_waterKind.y > 0.5 || crestOver) && u_waterTorchAt.w > 0.0 && u_waterLift.z > 0.001)
	{
		float medium = u_waterLift.w;
		float soak = max(u_waterLift.y * 0.5, 0.8);
		float reach = min(far, min(u_waterTorchAt.w, 30.0));
		vec3 way = -toEye / max(far, 1.0e-4);
		vec3 aim = normalize(u_waterTorchAim.xyz);
		float lit = 0.0;
		for (int i = 0; i < 16; ++i)
		{
			float u = (float(i) + 0.5) / 16.0;
			float s = 0.5 + u * u * max(reach - 0.5, 0.0);
			vec3 fromLight = u_waterEye.xyz + way * s - u_waterTorchAt.xyz;
			float d = length(fromLight);
			float cone = clamp((dot(fromLight / max(d, 1.0e-3), aim) - u_waterTorchAim.w) / max(u_waterTorchLight.w - u_waterTorchAim.w, 1.0e-4), 0.0, 1.0);
			float window = clamp(1.0 - pow(d / max(u_waterTorchAt.w, 1.0e-3), 4.0), 0.0, 1.0);
			float absorbed = mix(1.0, exp(-2.0 * s / soak), medium);
			lit += cone * cone * window * absorbed * 2.0 * u / max(d * d, mix(2.5, 5.0, medium));
		}
		vec3 tint = mix(vec3_splat(1.0), vec3(0.55, 0.85, 1.0), medium);
		colour += u_waterTorchLight.rgb * tint * lit * (max(reach - 0.5, 0.0) / 16.0) * u_waterLift.z * 0.006 * (1.0 + 1.2 * medium) * (1.0 - fogAmount * 0.5);
	}

	if (u_waterSunColor.w < 0.5)
	{
		colour *= max(u_waterAmbient.w, 0.0);
		colour = clamp((colour * (2.51 * colour + 0.03)) / (colour * (2.43 * colour + 0.59) + 0.14), 0.0, 1.0);
		colour = pow(colour, vec3_splat(1.0 / 2.2));
	}
	gl_FragColor = vec4(colour, clamp(alpha, 0.0, 1.0));
}
