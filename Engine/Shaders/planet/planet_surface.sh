// A planet's surface from space, from a point on it: shared by the sky (a planet hung in it) and the system map, so a
// world looks like itself wherever it is seen. See Engine/Render/PlanetLook.h for what the numbers are.
//
//   a = ground colour A, sea's share of the surface
//   b = ground colour B, ice
//   c = sea colour, cloud
//   d = cloud colour, 1 for a gas giant
// `n` is the point on the unit sphere in the planet's own frame; `seed` moves the pattern; `drift` turns the cloud.
//
// The noise is value noise, but each octave turned against the last: on its own grid every octave lines up with the
// one before, and a planet filling the window showed its squares. Coastlines are warped by another noise, so they wind
// rather than blob, and there are enough octaves for a world seen from low orbit to have detail at the size of a pixel.

float PlanetHash(vec3 p)
{
	// A hash without sin, steady at the large coordinates the fine octaves reach.
	p = fract(p * 0.1031);
	p += dot(p, p.zyx + 31.32);
	return fract((p.x + p.y) * p.z);
}

// A direction at a corner of the lattice, from its hash.
vec3 PlanetGradient(vec3 i)
{
	float a = PlanetHash(i) * 6.2831853;
	float z = PlanetHash(i + vec3(17.3, 5.1, 9.7)) * 2.0 - 1.0;
	float r = sqrt(max(1.0 - z * z, 0.0));
	return vec3(r * cos(a), r * sin(a), z);
}

// Gradient noise, 0 to 1 about a middle of a half: smooth, and without the squares value noise leaves in relief.
float PlanetNoise(vec3 p)
{
	vec3 i = floor(p);
	vec3 f = fract(p);
	vec3 u = f * f * f * (f * (f * 6.0 - vec3_splat(15.0)) + vec3_splat(10.0));
	float a = dot(PlanetGradient(i), f);
	float b = dot(PlanetGradient(i + vec3(1.0, 0.0, 0.0)), f - vec3(1.0, 0.0, 0.0));
	float c = dot(PlanetGradient(i + vec3(0.0, 1.0, 0.0)), f - vec3(0.0, 1.0, 0.0));
	float d = dot(PlanetGradient(i + vec3(1.0, 1.0, 0.0)), f - vec3(1.0, 1.0, 0.0));
	float e = dot(PlanetGradient(i + vec3(0.0, 0.0, 1.0)), f - vec3(0.0, 0.0, 1.0));
	float g = dot(PlanetGradient(i + vec3(1.0, 0.0, 1.0)), f - vec3(1.0, 0.0, 1.0));
	float h = dot(PlanetGradient(i + vec3(0.0, 1.0, 1.0)), f - vec3(0.0, 1.0, 1.0));
	float k = dot(PlanetGradient(i + vec3(1.0, 1.0, 1.0)), f - vec3(1.0, 1.0, 1.0));
	float n = mix(mix(mix(a, b, u.x), mix(c, d, u.x), u.y), mix(mix(e, g, u.x), mix(h, k, u.x), u.y), u.z);
	return clamp(0.5 + n * 1.1, 0.0, 1.0);
}

// Each octave turned and scaled from the last (a rotation by an awkward angle about an awkward axis, times two).
vec3 PlanetNext(vec3 p)
{
	return vec3(1.60 * p.y + 1.20 * p.z, -1.60 * p.x + 0.72 * p.y - 0.96 * p.z, -1.20 * p.x - 0.96 * p.y + 1.28 * p.z) +
	       vec3(1.7, 9.2, 3.1);
}

float PlanetFbmN(vec3 p, int octaves)
{
	float value = 0.0;
	float amount = 0.5;
	float total = 0.0;
	for (int octave = 0; octave < 8; ++octave)
	{
		if (octave >= octaves)
		{
			break;
		}
		value += amount * PlanetNoise(p);
		total += amount;
		p = PlanetNext(p);
		amount *= 0.5;
	}
	return value / total;
}

float PlanetFbm(vec3 p)
{
	return PlanetFbmN(p, 5);
}

// Ridges: sharp crests where the noise crosses its middle -- mountain ranges rather than hills.
float PlanetRidges(vec3 p)
{
	float value = 0.0;
	float amount = 0.5;
	for (int octave = 0; octave < 4; ++octave)
	{
		// The crest rounded off a little (a soft abs): a knife edge reads as crumpled paper in the light.
		float x = PlanetNoise(p) * 2.0 - 1.0;
		float r = 1.0 - sqrt(x * x + 0.03);
		value += amount * r * r;
		p = PlanetNext(p);
		amount *= 0.5;
	}
	return value / 0.9375;
}

// Craters on a body with no air to wear them down: a cell of space at a time, a crater in about half of them, each a bowl
// with a raised rim. Only the eight cells nearest the point are looked in (a crater is smaller than its cell). x is how
// high the ground is from them (the bowl below, the rim above), y how much fresh ejecta is there (bright round the young).
vec2 PlanetCraterCells(vec3 p, float seed)
{
	vec3 cell = floor(p);
	vec3 f = fract(p);
	vec3 toward = step(vec3_splat(0.5), f) * 2.0 - vec3_splat(1.0);
	float height = 0.0;
	float fresh = 0.0;
	for (int k = 0; k < 8; ++k)
	{
		vec3 o = vec3(float(k / 4), float((k / 2) - (k / 4) * 2), float(k - (k / 2) * 2));
		vec3 c = cell + o * toward;
		if (PlanetHash(c + vec3_splat(seed)) > 0.5)
		{
			continue;
		}
		vec3 centre = c + vec3(0.2, 0.2, 0.2) + 0.6 * vec3(PlanetHash(c + vec3(1.3, seed, 2.9)), PlanetHash(c + vec3(7.1, 0.4, seed)), PlanetHash(c + vec3(seed, 3.7, 5.3)));
		float radius = 0.18 + 0.22 * PlanetHash(c + vec3(5.5, 5.5, seed));
		float d = length(p - centre) / radius;
		float bowl = d < 1.0 ? -(1.0 - d * d) * 0.8 : 0.0;
		float rim = exp(-(d - 1.0) * (d - 1.0) * 30.0) * 0.45;
		height += bowl + rim;
		float young = step(0.8, PlanetHash(c + vec3(9.9, seed, 9.9)));
		fresh += young * exp(-max(d - 0.9, 0.0) * 2.5) * step(d, 2.4);
	}
	return vec2(height, min(fresh, 1.0));
}

vec2 PlanetCraters(vec3 n, float seed)
{
	vec2 large = PlanetCraterCells(n * 4.0 + vec3_splat(seed), seed);
	vec2 small = PlanetCraterCells(n * 13.0 + vec3(seed * 2.0, 1.7, 3.1), seed + 7.0);
	return vec2(large.x + small.x * 0.45, max(large.y, small.y * 0.7));
}

// How high the ground is, 0 at the sea to about 1 on the tallest ranges: for the light across it.
float PlanetContinents(vec3 n, vec3 offset)
{
	vec3 warp = vec3(PlanetFbmN(n * 1.7 + offset + vec3(5.2, 1.3, 7.7), 4), PlanetFbmN(n * 1.7 + offset + vec3(1.1, 8.3, 2.8), 4),
	                 PlanetFbmN(n * 1.7 + offset + vec3(9.4, 4.6, 0.3), 4));
	return PlanetFbmN(n * 2.1 + offset + (warp - vec3_splat(0.5)) * 0.9, 7);
}

float PlanetRelief(vec3 n, vec4 a, float seed, float craters)
{
	vec3 offset = vec3(seed * 1.7, seed * 0.9, seed * 2.3);
	float continents = PlanetContinents(n, offset);
	float shore = 0.5 + (a.w - 0.5) * 0.36;
	float land = a.w > 0.001 ? smoothstep(shore - 0.01, shore + 0.06, continents) : 1.0;
	float ridges = PlanetRidges(n * 6.0 + offset * 0.5);
	float height = land * (0.35 * continents + 0.65 * ridges * smoothstep(0.35, 0.75, continents + 0.2));
	// The large craters only, for the light: the small are in the colour.
	if (craters > 0.0)
	{
		height = height * (1.0 - craters * 0.5) + PlanetCraterCells(n * 4.0 + vec3_splat(seed), seed).x * craters * 0.5;
	}
	return height;
}

// The normal turned by the lie of the ground, for the light to show its relief; flat for a gas giant.
vec3 PlanetBump(vec3 n, vec4 a, vec4 d, float seed, float strength, float craters)
{
	if (d.w > 0.5 || strength <= 0.0)
	{
		return n;
	}
	vec3 t1 = normalize(cross(n, abs(n.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
	vec3 t2 = cross(n, t1);
	const float e = 0.005;
	float h0 = PlanetRelief(n, a, seed, craters);
	float h1 = PlanetRelief(normalize(n + t1 * e), a, seed, craters);
	float h2 = PlanetRelief(normalize(n + t2 * e), a, seed, craters);
	return normalize(n - (t1 * (h1 - h0) + t2 * (h2 - h0)) * (strength / e) * 0.02);
}

// The colour of the ground or sea under the cloud (rgb), and how much cloud is over it (w); `sea` how much of it is
// open water, for the glint off it. `craters` (0 to 1) how cratered it is: an airless body's face.
vec4 PlanetAlbedo(vec3 n, vec4 a, vec4 b, vec4 c, vec4 d, float seed, float drift, float craters, out float sea)
{
	vec3 offset = vec3(seed * 1.7, seed * 0.9, seed * 2.3);
	sea = 0.0;
	if (d.w > 0.5)
	{
		// A gas giant: bands by latitude, pulled about by the flow along them, eddies at their edges, and storms.
		vec3 flow = vec3(n.x * 3.0, n.y * 11.0, n.z * 3.0) + offset;
		float turbulence = PlanetFbmN(flow, 6);
		float eddies = PlanetFbmN(flow * 3.0 + vec3_splat(turbulence * 2.0), 4);
		float band = sin(n.y * 17.0 + turbulence * 7.0 + eddies * 1.5 + seed) * 0.5 + 0.5;
		float fine = sin(n.y * 61.0 + turbulence * 13.0 + eddies * 4.0) * 0.5 + 0.5;
		vec3 colour = mix(a.rgb, b.rgb, band);
		colour = mix(colour, colour * 1.18, fine * 0.35);
		colour *= 0.92 + 0.16 * eddies;
		float storm = smoothstep(0.74, 0.82, PlanetFbmN(n * 5.0 + offset * 2.0, 5));
		colour = mix(colour, b.rgb * 0.7 + vec3(0.1, 0.05, 0.03), storm * 0.6);
		return vec4(colour, 0.0);
	}
	float continents = PlanetContinents(n, offset);
	float detail = PlanetFbmN(n * 8.5 + offset * 1.3, 6);
	// Where the sea comes up to: set so that about the asked-for share of the surface is under it.
	float shore = 0.5 + (a.w - 0.5) * 0.36;
	float land = a.w > 0.001 ? smoothstep(shore - 0.004, shore + 0.004, continents) : 1.0;
	vec3 ground = mix(a.rgb, b.rgb, smoothstep(0.3, 0.7, detail));
	// Highlands lighter, lowlands by the coast darker and a little greener-brown; and the grain of it all, fine.
	float height = smoothstep(shore, shore + 0.3, continents);
	ground = mix(ground * 0.85, ground * 1.12, height);
	ground *= 0.84 + 0.32 * PlanetFbmN(n * 40.0 + offset, 4);
	if (craters > 0.0)
	{
		// Dark old plains in the low ground, the craters' floors shaded and their rims and fresh rays bright.
		vec2 crater = PlanetCraters(n, seed);
		float maria = smoothstep(0.42, 0.36, PlanetFbmN(n * 1.6 + offset * 0.4, 5));
		ground *= 1.0 - maria * 0.35 * craters;
		ground *= 1.0 + clamp(crater.x, -0.8, 0.6) * 0.35 * craters;
		ground = mix(ground, ground * 1.6 + vec3_splat(0.08), crater.y * 0.5 * craters);
	}
	vec3 water = c.rgb * (0.6 + 0.5 * smoothstep(shore - 0.16, shore, continents));
	vec3 colour = mix(water, ground, land);
	sea = 1.0 - land;
	// Ice from the poles towards the middle, as far as there is ice, its edge broken by the ground under it.
	if (b.w > 0.001)
	{
		float polar = abs(n.y) + (detail - 0.5) * 0.35 + (continents - 0.5) * 0.1;
		float iceLine = 1.0 - b.w * 1.15;
		float ice = smoothstep(iceLine - 0.02, iceLine + 0.02, polar);
		// Not one flat white: drifts and older, greyer ice, cracked across.
		float drifts = PlanetFbmN(n * 15.0 + offset * 0.7, 6);
		vec3 iceColour = mix(vec3(0.66, 0.72, 0.8), vec3(0.93, 0.95, 0.98), smoothstep(0.3, 0.7, drifts));
		colour = mix(colour, iceColour, ice);
		sea *= 1.0 - ice;
	}
	// Cloud in banks and streaks, slowly going round, thinning at their edges into wisps.
	float cloud = 0.0;
	if (c.w > 0.001)
	{
		vec3 m = vec3(n.x * cos(drift) - n.z * sin(drift), n.y, n.x * sin(drift) + n.z * cos(drift));
		vec3 swirl = vec3(PlanetFbmN(m * 2.0 + offset * 2.1, 3), PlanetFbmN(m * 2.0 + offset * 1.7, 3), 0.0);
		float banks = PlanetFbmN(m * 3.6 + offset * 3.1 + swirl, 6);
		float streaks = PlanetFbmN(m * vec3(14.0, 4.5, 14.0) + offset, 5);
		float cover = 0.72 - c.w * 0.42;
		cloud = smoothstep(cover - 0.06, cover + 0.1, banks * 0.8 + streaks * 0.3);
	}
	return vec4(colour, cloud);
}
