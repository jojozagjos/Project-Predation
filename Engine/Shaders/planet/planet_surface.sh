// A planet's surface from space, from a point on it: shared by the sky (a planet hung in it) and the system map, so a
// world looks like itself wherever it is seen. See Engine/Render/PlanetLook.h for what the numbers are.
//
//   a = ground colour A, sea's share of the surface
//   b = ground colour B, ice
//   c = sea colour, cloud
//   d = cloud colour, 1 for a gas giant
// `n` is the point on the unit sphere in the planet's own frame; `seed` moves the pattern; `drift` turns the cloud.

float PlanetHash(vec3 p)
{
	p = fract(p * 0.3183099 + vec3(0.71, 0.113, 0.419));
	p *= 17.0;
	return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float PlanetNoise(vec3 p)
{
	vec3 i = floor(p);
	vec3 f = fract(p);
	f = f * f * (vec3_splat(3.0) - f * 2.0);
	float a = PlanetHash(i);
	float b = PlanetHash(i + vec3(1.0, 0.0, 0.0));
	float c = PlanetHash(i + vec3(0.0, 1.0, 0.0));
	float d = PlanetHash(i + vec3(1.0, 1.0, 0.0));
	float e = PlanetHash(i + vec3(0.0, 0.0, 1.0));
	float g = PlanetHash(i + vec3(1.0, 0.0, 1.0));
	float h = PlanetHash(i + vec3(0.0, 1.0, 1.0));
	float k = PlanetHash(i + vec3(1.0, 1.0, 1.0));
	return mix(mix(mix(a, b, f.x), mix(c, d, f.x), f.y), mix(mix(e, g, f.x), mix(h, k, f.x), f.y), f.z);
}

float PlanetFbm(vec3 p)
{
	float value = 0.0;
	float amount = 0.5;
	for (int octave = 0; octave < 5; ++octave)
	{
		value += amount * PlanetNoise(p);
		p = p * 2.03 + vec3(1.7, 9.2, 3.1);
		amount *= 0.5;
	}
	return value;
}

// The colour of the ground or sea under the cloud (rgb), and how much cloud is over it (w); `sea` how much of it is
// open water, for the glint off it.
vec4 PlanetAlbedo(vec3 n, vec4 a, vec4 b, vec4 c, vec4 d, float seed, float drift, out float sea)
{
	vec3 offset = vec3(seed * 1.7, seed * 0.9, seed * 2.3);
	sea = 0.0;
	if (d.w > 0.5)
	{
		// A gas giant: bands by latitude, pulled about by the flow along them, with storms in them.
		float turbulence = PlanetFbm(vec3(n.x * 3.0, n.y * 11.0, n.z * 3.0) + offset);
		float band = sin(n.y * 17.0 + turbulence * 7.0 + seed) * 0.5 + 0.5;
		float fine = sin(n.y * 61.0 + turbulence * 13.0) * 0.5 + 0.5;
		vec3 colour = mix(a.rgb, b.rgb, band);
		colour = mix(colour, colour * 1.18, fine * 0.35);
		float storm = smoothstep(0.78, 0.86, PlanetFbm(n * 5.0 + offset * 2.0));
		colour = mix(colour, b.rgb * 0.7 + vec3(0.1, 0.05, 0.03), storm * 0.6);
		return vec4(colour, 0.0);
	}
	float continents = PlanetFbm(n * 2.1 + offset);
	float detail = PlanetFbm(n * 8.5 + offset * 1.3);
	// Where the sea comes up to: set so that about the asked-for share of the surface is under it.
	float shore = 0.5 + (a.w - 0.5) * 0.36;
	float land = a.w > 0.001 ? smoothstep(shore - 0.008, shore + 0.008, continents) : 1.0;
	vec3 ground = mix(a.rgb, b.rgb, smoothstep(0.32, 0.68, detail));
	ground *= 0.82 + 0.36 * PlanetNoise(n * 34.0 + offset);
	vec3 water = c.rgb * (0.65 + 0.45 * smoothstep(shore - 0.14, shore, continents));
	vec3 colour = mix(water, ground, land);
	sea = 1.0 - land;
	// Ice from the poles towards the middle, as far as there is ice, its edge broken by the ground under it.
	if (b.w > 0.001)
	{
		float polar = abs(n.y) + (detail - 0.5) * 0.35;
		float iceLine = 1.0 - b.w * 1.15;
		float ice = smoothstep(iceLine - 0.03, iceLine + 0.03, polar);
		// Not one flat white: drifts and older, greyer ice, cracked across.
		float drifts = PlanetFbm(n * 15.0 + offset * 0.7);
		vec3 iceColour = mix(vec3(0.66, 0.72, 0.8), vec3(0.93, 0.95, 0.98), smoothstep(0.3, 0.7, drifts));
		colour = mix(colour, iceColour, ice);
		sea *= 1.0 - ice;
	}
	// Cloud in banks and streaks, slowly going round.
	float cloud = 0.0;
	if (c.w > 0.001)
	{
		vec3 m = vec3(n.x * cos(drift) - n.z * sin(drift), n.y, n.x * sin(drift) + n.z * cos(drift));
		float banks = PlanetFbm(m * 3.6 + offset * 3.1);
		float streaks = PlanetFbm(m * vec3(14.0, 4.5, 14.0) + offset);
		float cover = 0.72 - c.w * 0.42;
		cloud = smoothstep(cover - 0.08, cover + 0.08, banks * 0.8 + streaks * 0.3);
	}
	return vec4(colour, cloud);
}
