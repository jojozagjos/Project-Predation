$input v_ray

#include <bgfx_shader.sh>

uniform vec4 u_skyZenith;  // rgb
uniform vec4 u_skyHorizon; // rgb
uniform vec4 u_skyGround;  // rgb
uniform vec4 u_skySun;     // xyz = direction towards the sun, w = how sharp the glow is
uniform vec4 u_skySunColor; // rgb, w = how bright
uniform vec4 u_skyGrade;    // x = exposure, y = contrast
uniform vec4 u_skySpace;       // x = stars (0 none, 1 all), y = a planet's radius on the sky (radians; 0 none), z = its air, w = brightness
uniform vec4 u_skyPlanet;      // xyz = towards the planet's middle, w = how warm its air glows (0 blue, 1 amber)
// How it looks (Shaders/planet/planet_surface.sh); e is the air's colour and the pattern's seed.
uniform vec4 u_planetA;
uniform vec4 u_planetB;
uniform vec4 u_planetC;
uniform vec4 u_planetD;
uniform vec4 u_planetE;
// The rest of a system, further off: per body, xyz which way (its length how bright it is as a point) and w its radius on
// the sky (0 none), then its colour and air.
uniform vec4 u_skyBodies[24];
uniform vec4 u_skyRings;     // x, y = the planet's rings, inside and outside, in its radii (0 none); z = the sun's disc, radians
uniform vec4 u_skyRingColor; // rgb

#include "planet/planet_surface.sh"

// The planet's own frame on the sky: its pole tipped a little towards the eye (so its rings are seen from a little above,
// not edge on) but never at it (or every planet below would show nothing but its ice cap).
void SkyPlanetFrame(vec3 toPlanet, out vec3 pole, out vec3 east, out vec3 third)
{
	vec3 across = cross(toPlanet, vec3(1.0, 0.0, 0.0));
	across = dot(across, across) < 0.01 ? cross(toPlanet, vec3(0.0, 0.0, 1.0)) : across;
	pole = normalize(normalize(across) - toPlanet * 0.38);
	east = normalize(cross(pole, toPlanet));
	third = cross(east, pole);
}

// How much of the sun the rings hide from a point on the planet (0 none).
float SkyRingAt(vec3 spot, vec3 towardsSun, vec3 centre, vec3 pole, float radius)
{
	if (u_skyRings.y <= 0.0)
	{
		return 0.0;
	}
	float facing = dot(towardsSun, pole);
	if (abs(facing) < 0.0001)
	{
		return 0.0;
	}
	float t = dot(centre - spot, pole) / facing;
	if (t <= 0.0)
	{
		return 0.0;
	}
	float r = length(spot + towardsSun * t - centre) / radius;
	return step(u_skyRings.x, r) * step(r, u_skyRings.y);
}

// Noise over directions, for the planet's ground and cloud and the faint band of the galaxy.
float SkyHash(vec3 p)
{
	p = fract(p * 0.3183099 + vec3(0.71, 0.113, 0.419));
	p *= 17.0;
	return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float SkyNoise(vec3 p)
{
	vec3 i = floor(p);
	vec3 f = fract(p);
	f = f * f * (vec3_splat(3.0) - f * 2.0);
	float a = SkyHash(i);
	float b = SkyHash(i + vec3(1.0, 0.0, 0.0));
	float c = SkyHash(i + vec3(0.0, 1.0, 0.0));
	float d = SkyHash(i + vec3(1.0, 1.0, 0.0));
	float e = SkyHash(i + vec3(0.0, 0.0, 1.0));
	float g = SkyHash(i + vec3(1.0, 0.0, 1.0));
	float h = SkyHash(i + vec3(0.0, 1.0, 1.0));
	float k = SkyHash(i + vec3(1.0, 1.0, 1.0));
	return mix(mix(mix(a, b, f.x), mix(c, d, f.x), f.y), mix(mix(e, g, f.x), mix(h, k, f.x), f.y), f.z);
}

float SkyFbm(vec3 p)
{
	float value = 0.0;
	float amount = 0.5;
	for (int octave = 0; octave < 5; ++octave)
	{
		value += amount * SkyNoise(p);
		p = p * 2.03 + vec3(1.7, 9.2, 3.1);
		amount *= 0.5;
	}
	return value;
}

// Stars: the sky cut into small cells, a few of which hold one, somewhere in it, of its own brightness and colour.
vec3 SkyStars(vec3 ray)
{
	vec3 p = ray * 180.0;
	vec3 light = vec3_splat(0.0);
	// A soft point, spread over at least the pixel it falls in: a star smaller than a pixel sampled at the pixel's middle
	// was there one frame and gone the next as the view moved. And looked for in the eight cells nearest the pixel, not
	// only the one it is in: a star near the edge of its cell was cut off by the pixels just over the edge, and as the
	// view moved stars broke up and flickered at their cells' edges -- the sky seen to shake.
	float pixel = length(fwidth(p));
	float spread = sqrt(0.12 * 0.12 + 0.6 * pixel * pixel);
	vec3 base = floor(p - vec3_splat(0.5));
	for (int i = 0; i < 8; ++i)
	{
		vec3 cell = base + vec3(float(i & 1), float((i >> 1) & 1), float((i >> 2) & 1));
		float h = SkyHash(cell);
		if (h > 0.93)
		{
			vec3 jitter = vec3(SkyHash(cell + vec3_splat(7.1)), SkyHash(cell + vec3_splat(3.7)), SkyHash(cell + vec3_splat(11.3)));
			// Where it is, fixed in its cell: never pushed onto the sky after, which could put it outside the cells looked in.
			vec3 centre = cell + vec3_splat(0.5) + (jitter - vec3_splat(0.5)) * 0.6;
			float bright = (h - 0.93) / 0.07;
			float d = length(p - centre);
			float star = exp(-d * d / (2.0 * spread * spread)) * (0.12 * 0.12) / (spread * spread) * (0.12 + 3.2 * bright * bright * bright);
			light += mix(vec3(0.72, 0.8, 1.0), vec3(1.0, 0.86, 0.7), SkyHash(cell + vec3_splat(5.3))) * star;
		}
	}
	// And the galaxy, a faint band across it.
	float across = dot(ray, normalize(vec3(0.35, 0.82, 0.45)));
	light += vec3(0.5, 0.55, 0.7) * exp(-across * across * 18.0) * SkyFbm(ray * 7.0) * 0.05;
	return light;
}

void main()
{
	vec3 ray = normalize(v_ray);

	// Two gradients meeting at the horizon, not one. The sky does not fade smoothly from overhead to
	// underfoot: it is brightest just above the horizon, where the light has most air to scatter
	// through, and the ground below is a different colour entirely. A single lerp from zenith to
	// ground gives the flat grey band that says "programmer sky" from across a room.
	float up = ray.y;
	float height = pow(clamp(up, 0.0, 1.0), 0.45);
	vec3 color = mix(u_skyHorizon.rgb, u_skyZenith.rgb, height);
	// Below the horizon it turns towards the ground colour over a few degrees rather than at a line,
	// because a hard edge there reads as a wall rather than as distance.
	color = mix(color, u_skyGround.rgb, clamp(-up * 6.0, 0.0, 1.0));

	// And the sun, as a glow rather than a disc. A disc needs to be the right size and to survive
	// being looked straight at; a glow is what is actually visible through air and costs one dot
	// product. The tight core sits inside a wide halo, which is the difference between a light in
	// the sky and a white circle painted on it.
	float toSun = max(dot(ray, normalize(u_skySun.xyz)), 0.0);
	float halo = pow(toSun, max(u_skySun.w, 1.0));
	float core = pow(toSun, max(u_skySun.w, 1.0) * 24.0);
	// In space there is no horizon and no air to light: black all round, but for the stars and the sun wherever it is.
	// (It was the sky's zenith dimmed, which is the colour of a sky, and read as grey.)
	float inSpace = step(0.001, u_skySpace.x);
	color = mix(color, vec3(0.0006, 0.0007, 0.001), inSpace);
	color += u_skySunColor.rgb * u_skySunColor.w * (halo * 0.35 + core) * clamp(up * 4.0 + 0.4, 0.0, 1.0) * (1.0 - inSpace);
	// In space, with no air to spread it, the sun is a small hard disc, blinding, darker at its rim; a tight corona
	// round it and a faint wide glow; and the thin rays a lens throws off anything that bright, four long and four short.
	if (inSpace > 0.5)
	{
		// As big as the star really is from here (and never smaller than a few pixels), its rim darker and redder than its
		// middle, a tight glare round it that a camera's lens spreads, and four faint spikes -- not a star drawn by a child.
		vec3 sunDir = normalize(u_skySun.xyz);
		float angle = acos(clamp(dot(ray, sunDir), -1.0, 1.0));
		float pixelAngle = max(length(fwidth(ray)), 0.00005);
		float discR = max(u_skyRings.z, pixelAngle * 2.5);
		float disc = 1.0 - smoothstep(discR - pixelAngle, discR + pixelAngle, angle);
		float mu = sqrt(max(1.0 - (angle / discR) * (angle / discR), 0.0));
		vec3 hot = mix(u_skySunColor.rgb, vec3(1.0, 0.98, 0.95), 0.75);
		vec3 rim = u_skySunColor.rgb * vec3(1.0, 0.78, 0.6);
		vec3 face = mix(rim, hot, 0.35 + 0.65 * mu) * (0.45 + 0.55 * mu);
		float outside = max(angle - discR, 0.0);
		float glare = exp(-outside / (discR * 1.2 + pixelAngle * 2.0)) * 0.7 + exp(-outside / (discR * 7.0 + 0.01)) * 0.12 +
					  exp(-angle / 0.4) * 0.02;
		vec3 across = abs(sunDir.y) < 0.95 ? normalize(cross(sunDir, vec3(0.0, 1.0, 0.0))) : vec3(1.0, 0.0, 0.0);
		vec3 over = cross(across, sunDir);
		vec2 p = vec2(dot(ray, across), dot(ray, over));
		vec2 q = vec2(p.x + p.y, p.x - p.y) * 0.7071;
		float thin = 1.0 / max(pixelAngle * 1.5, 0.0003);
		float spikes = (exp(-abs(q.y) * thin) * exp(-abs(q.x) * 18.0) + exp(-abs(q.x) * thin) * exp(-abs(q.y) * 18.0)) *
					   step(0.0, dot(ray, sunDir));
		// A star is far too bright to look at: its disc is white whatever its colour, the colour is in the light round it.
		vec3 discColour = mix(face, vec3(1.0, 0.99, 0.97) * (0.5 + 0.5 * mu), 0.6);
		color += (discColour * disc * 80.0 + mix(hot, u_skySunColor.rgb, 0.4) * (glare + spikes * 0.18)) * u_skySunColor.w;
	}

	// A planet: a sphere one unit away, as big on the sky as it is asked to be, lit by the sun, with ice and cloud
	// over it, and its air glowing at the edge on the lit side -- and a little beyond the edge.
	float starsShow = 1.0;
	if (u_skySpace.y > 0.0)
	{
		vec3 toPlanet = normalize(u_skyPlanet.xyz);
		vec3 towardsSun = normalize(u_skySun.xyz);
		vec3 air = mix(u_planetE.rgb, vec3(1.0, 0.68, 0.36), u_skyPlanet.w) * u_skySpace.z;
		float radius = sin(u_skySpace.y);
		float along = dot(ray, toPlanet);
		float hit = along * along - (1.0 - radius * radius);
		if (hit > 0.0 && along > 0.0)
		{
			vec3 normal = (ray * (along - sqrt(hit)) - toPlanet) / radius;
			float lit = dot(normal, towardsSun);
			// Its ground, seas, ice and cloud, as the system map draws it.
			float sea = 0.0;
			// In the planet's own frame, its poles at the top and bottom of it as seen, not facing the eye -- or every planet
			// below the ship would show its ice cap and nothing else.
			vec3 pole;
			vec3 east;
			vec3 third;
			SkyPlanetFrame(toPlanet, pole, east, third);
			vec3 local = vec3(dot(normal, east), dot(normal, pole), dot(normal, third));
			vec4 surface = PlanetAlbedo(local, u_planetA, u_planetB, u_planetC, u_planetD, u_planetE.w, 0.0, sea);
			// The lie of the land in the light: ranges catch it on one side and shadow the other, most of all near the line
			// between day and night.
			vec3 bumped = PlanetBump(local, u_planetA, u_planetD, u_planetE.w, 1.0);
			vec3 bumpedWorld = normalize(east * bumped.x + pole * bumped.y + third * bumped.z);
			float relief = clamp(1.0 + (dot(bumpedWorld, towardsSun) - lit) * 1.3, 0.6, 1.35);
			vec3 ground = mix(surface.rgb * relief, u_planetD.rgb, surface.w);
			// Shadowed by its rings, where they come between it and the sun.
			float ringShade = 1.0 - SkyRingAt(toPlanet + normal * radius, towardsSun, toPlanet, pole, radius) * 0.7;
			vec3 planet = ground * smoothstep(-0.05, 0.4, lit) * ringShade * u_skySunColor.w * 1.6;
			planet += u_skySunColor.rgb * u_skySunColor.w * pow(max(dot(reflect(-towardsSun, normal), -ray), 0.0), 60.0) * sea *
					  (1.0 - surface.w) * step(0.0, lit) * 0.6;
			float edge = pow(1.0 - max(dot(normal, -ray), 0.0), 4.0);
			planet += air * edge * smoothstep(-0.25, 0.25, lit);
			color = planet * max(u_skySpace.w, 0.0);
			starsShow = 0.0;
		}
		else
		{
			float beyond = max(acos(clamp(along, -1.0, 1.0)) - u_skySpace.y, 0.0) / max(u_skySpace.y * 0.035, 0.0001);
			vec3 edgeDirection = normalize(ray - toPlanet * along);
			color += air * exp(-beyond) * 0.6 * max(u_skySpace.w, 0.0) * smoothstep(-0.3, 0.3, dot(edgeDirection, towardsSun));
		}
		// Its rings, in front of it or behind it, through them the stars; dark where the planet's shadow falls across them.
		if (u_skyRings.y > 0.0)
		{
			vec3 pole;
			vec3 east;
			vec3 third;
			SkyPlanetFrame(toPlanet, pole, east, third);
			float facing = dot(ray, pole);
			if (abs(facing) > 0.00001)
			{
				float t = dot(toPlanet, pole) / facing;
				float planetT = (hit > 0.0 && along > 0.0) ? along - sqrt(hit) : 1.0e9;
				if (t > 0.0 && t < planetT)
				{
					vec3 at = ray * t;
					float r = length(at - toPlanet) / radius;
					if (r > u_skyRings.x && r < u_skyRings.y)
					{
						float across = (r - u_skyRings.x) / max(u_skyRings.y - u_skyRings.x, 0.001);
						float bands = SkyFbm(vec3(across * 38.0, u_planetE.w, 0.5));
						float gaps = smoothstep(0.2, 0.3, abs(fract(across * 3.1 + 0.37) - 0.5) * 2.0);
						float alpha = smoothstep(0.0, 0.05, across) * smoothstep(1.0, 0.94, across) * (0.3 + 0.65 * bands) * mix(0.35, 1.0, gaps);
						// Lit from whichever side the sun is, and in the planet's shadow behind it.
						vec3 q = toPlanet - at;
						float sunAlong = dot(q, towardsSun);
						float shadowed = sunAlong > 0.0 && length(q - towardsSun * sunAlong) < radius ? 1.0 : 0.0;
						float light = (0.35 + 0.65 * abs(dot(pole, towardsSun))) * (1.0 - shadowed * 0.92);
						vec3 ringColour = u_skyRingColor.rgb * (0.75 + 0.5 * bands) * light * u_skySunColor.w * 1.6;
						color = mix(color, ringColour * max(u_skySpace.w, 0.0), alpha);
						starsShow *= 1.0 - alpha;
					}
				}
			}
		}
	}
	// The other planets and moons: lit discs when near enough to have a size, and otherwise what they really are from
	// across a system -- points of light, steadier and brighter than the stars, their colour, as bright as how big they are,
	// how near and how much of their lit side faces this way. Behind the near planet and in front of the stars.
	if (starsShow > 0.5 && inSpace > 0.5)
	{
		vec3 towardsLight = normalize(u_skySun.xyz);
		float pixelAngle = max(length(fwidth(ray)), 0.00005);
		for (int i = 0; i < 12; ++i)
		{
			vec4 place = u_skyBodies[i * 2];
			if (place.w <= 0.0)
			{
				continue;
			}
			vec4 look = u_skyBodies[i * 2 + 1];
			vec3 toBody = normalize(place.xyz);
			float bodyRadius = sin(place.w);
			float bodyAlong = dot(ray, toBody);
			float bodyHit = bodyAlong * bodyAlong - (1.0 - bodyRadius * bodyRadius);
			// Smaller than a pixel or two it is a point (below), not a disc that is there one frame and gone the next.
			if (bodyHit > 0.0 && bodyAlong > 0.0 && place.w > pixelAngle * 1.5)
			{
				vec3 bodyNormal = (ray * (bodyAlong - sqrt(bodyHit)) - toBody) / bodyRadius;
				float bodyLit = dot(bodyNormal, towardsLight);
				vec3 shade = look.rgb * (smoothstep(-0.08, 0.35, bodyLit) * u_skySunColor.w * 1.8 + 0.01);
				shade += vec3(0.4, 0.6, 1.0) * look.w * pow(1.0 - max(dot(bodyNormal, -ray), 0.0), 3.0) * smoothstep(-0.2, 0.3, bodyLit);
				color = shade * max(u_skySpace.w, 0.0);
				starsShow = 0.0;
			}
			else
			{
				float angle = acos(clamp(bodyAlong, -1.0, 1.0));
				float bright = length(place.xyz);
				// How much of its face is lit, seen from here: full behind the sun, a sliver towards it.
				float phase = max(dot(toBody, towardsLight) * -0.5 + 0.5, 0.12);
				vec3 tint = mix(look.rgb, vec3_splat(1.0), 0.35);
				// A point, spread over at least a pixel and a half so it never flickers, and a soft halo round it.
				float spread = max(pixelAngle * 0.9, place.w);
				float glint = exp(-angle * angle / (2.0 * spread * spread)) * (1.0 - smoothstep(1.0, 3.0, place.w / pixelAngle));
				float halo = exp(-max(angle - place.w, 0.0) / (pixelAngle * 5.0 + place.w * 1.5));
				color += tint * (glint * 2.2 + halo * 0.07) * bright * phase * max(u_skySpace.w, 0.0);
			}
		}
	}
	color += SkyStars(ray) * u_skySpace.x * starsShow * max(u_skySpace.w, 0.0);

	// For post-processing to finish, as linear light.
	if (u_skyGrade.w > 0.5)
	{
		gl_FragColor = vec4(color, 1.0);
		return;
	}

	// Through the same curve the world goes through, or the sky is the one thing on screen that was
	// not developed: colours written straight out land far darker than the same numbers do on a
	// surface, and the horizon reads as a black band where it should be haze.
	color *= max(u_skyGrade.x, 0.0);
	color = clamp((color * (2.51 * color + 0.03)) / (color * (2.43 * color + 0.59) + 0.14), 0.0, 1.0);
	color = pow(color, vec3_splat(1.0 / 2.2));
	color = clamp((color - vec3_splat(0.5)) * max(u_skyGrade.y, 0.0) + vec3_splat(0.5), 0.0, 1.0);

	// The same dither the world gets. A sky is the largest, smoothest gradient on screen and the
	// first place eight bits per channel shows as bands.
	float dither = fract(52.9829189 * fract(0.06711056 * gl_FragCoord.x + 0.00583715 * gl_FragCoord.y));
	color += vec3_splat((dither - 0.5) / 255.0);

	gl_FragColor = vec4(color, 1.0);
}
