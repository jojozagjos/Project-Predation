$input v_ray

#include <bgfx_shader.sh>

uniform vec4 u_skyZenith;  // rgb; w = how thick the air is with the weather (0 to 1): the sun, the stars and the bodies dimmed behind it
uniform vec4 u_skyHorizon; // rgb
uniform vec4 u_skyGround;  // rgb
uniform vec4 u_skySun;     // xyz = direction towards the sun, w = how sharp the glow is
uniform vec4 u_skySunColor; // rgb, w = how bright
uniform vec4 u_skyGrade;    // x = exposure, y = contrast
uniform vec4 u_skySpace;       // x = stars (0 none, 1 all), y = a planet's radius on the sky (radians; 0 none), z = its air, w = brightness
uniform vec4 u_skyPlanet;      // xyz = towards the planet's middle, w = how warm its air glows (0 blue, 1 amber)
uniform vec4 u_skyAir;         // rgb = the colour of the air the sky is seen through, w = how much (0 none: space, or no air)
// How it looks (Shaders/planet/planet_surface.sh); e is the air's colour and the pattern's seed.
uniform vec4 u_planetA;
uniform vec4 u_planetB;
uniform vec4 u_planetC;
uniform vec4 u_planetD;
uniform vec4 u_planetE;
// The rest of a system, further off: per body, xyz which way (its length how bright it is as a point) and w its radius on
// the sky (0 none), then its colour and air.
uniform vec4 u_skyBodies[24];
uniform vec4 u_skyRings;     // x, y = the planet's rings, inside and outside, in its radii (0 none); z = the sun's disc, radians;
                             // w = where the system's nebula lies (a seed)
uniform vec4 u_skyRingColor; // rgb
uniform vec4 u_skyClouds;    // x = how much of the sky the clouds cover, y = how heavy, z = the clock they drift by, w = lightning in them
uniform vec4 u_skyCloudWind; // xy = how far they have drifted, kilometres (Environment::cloudDrift)
uniform vec4 u_skyBolt;      // x = which way the bolt is (yaw), y = the sine of its top's elevation, z = its seed, w = how bright
uniform vec4 u_skyNebula;    // rgb = the system's nebula's colour, w = how much of it (its seed: u_skyRings.w)
uniform vec4 u_skyTorch[4];   // the torch, as fs_mesh has it (u_lights[0..3]), its position from the eye; [1].w = how hazy the air is (0: no beam)
uniform vec4 u_skyFog;        // rgb = the fog the ground fades into, as the meshes have it
uniform vec4 u_skyOwnRings;   // xyz = the pole of the world underfoot, over the place; w = 1 when it has rings to see (their size: u_skyRings.xy)

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

// The night sky as it is seen from somewhere in a galaxy: most stars faint, a few bright -- far more of them crowded along
// the galaxy's plane than away from it, white mostly, some hot and blue, some cool and orange -- and the galaxy itself, a
// band of light mottled by the stars too faint to make out, split down its middle by lanes of dark dust. And, over it all,
// the system's own nebula, faint, where it lies (u_skyNebula: its colour and how much; its seed in u_skyRings.w).
vec3 SkyStars(vec3 ray)
{
	vec3 plane = normalize(vec3(0.35, 0.82, 0.45));
	float across = dot(ray, plane);
	float band = exp(-across * across * 14.0);
	vec3 p = ray * 180.0;
	vec3 light = vec3_splat(0.0);
	// A soft point, spread over at least the pixel it falls in: a star smaller than a pixel sampled at the pixel's middle
	// was there one frame and gone the next as the view moved. And looked for in the eight cells nearest the pixel, not
	// only the one it is in: a star near the edge of its cell was cut off by the pixels just over the edge, and as the
	// view moved stars broke up and flickered at their cells' edges -- the sky seen to shake.
	float pixel = length(fwidth(p));
	float spread = sqrt(0.12 * 0.12 + 0.6 * pixel * pixel);
	float threshold = mix(0.972, 0.9, band);
	vec3 base = floor(p - vec3_splat(0.5));
	for (int i = 0; i < 8; ++i)
	{
		vec3 cell = base + vec3(float(i & 1), float((i >> 1) & 1), float((i >> 2) & 1));
		float h = SkyHash(cell);
		if (h > threshold)
		{
			vec3 jitter = vec3(SkyHash(cell + vec3_splat(7.1)), SkyHash(cell + vec3_splat(3.7)), SkyHash(cell + vec3_splat(11.3)));
			// Where it is, fixed in its cell: never pushed onto the sky after, which could put it outside the cells looked in.
			vec3 centre = cell + vec3_splat(0.5) + (jitter - vec3_splat(0.5)) * 0.6;
			float b = (h - threshold) / (1.0 - threshold);
			float bright = 0.05 + 4.0 * pow(SkyHash(cell + vec3_splat(2.9)), 9.0) + 0.25 * b * b;
			float d = length(p - centre);
			float star = exp(-d * d / (2.0 * spread * spread)) * (0.12 * 0.12) / (spread * spread) * bright;
			float hot = SkyHash(cell + vec3_splat(5.3));
			vec3 colour = hot < 0.15 ? vec3(0.65, 0.75, 1.0) : hot > 0.85 ? vec3(1.0, 0.75, 0.55) : mix(vec3(0.95, 0.95, 1.0), vec3(1.0, 0.93, 0.82), hot);
			light += colour * star;
		}
	}
	// The galaxy's own light: the stars too many and too faint to see one by one, in clouds along the band, and the dust
	// lanes down its middle hiding them.
	float clouds = SkyFbm(ray * 6.0) * 0.7 + SkyFbm(ray * 15.0) * 0.3;
	float lanes = smoothstep(0.5, 0.68, SkyFbm(ray * 9.0 + vec3(4.1, 1.7, 2.2))) * exp(-across * across * 70.0);
	light += mix(vec3(0.62, 0.62, 0.72), vec3(0.85, 0.75, 0.62), band) * band * (0.35 + clouds) * (1.0 - 0.85 * lanes) * 0.045;
	// The nebula: a few great soft clouds of its colour, brighter in their hearts, a second colour through them.
	if (u_skyNebula.w > 0.001)
	{
		vec3 at = ray * 2.2 + vec3_splat(u_skyRings.w);
		float shape = SkyFbm(at + vec3_splat(SkyFbm(at * 1.7) * 1.4));
		float cloud = smoothstep(0.52, 0.78, shape);
		vec3 tint = mix(u_skyNebula.rgb, u_skyNebula.gbr * 0.8 + vec3_splat(0.1), SkyFbm(at * 3.0 + vec3_splat(7.0)));
		light += tint * cloud * (0.4 + 0.6 * SkyFbm(ray * 11.0 + vec3_splat(u_skyRings.w))) * u_skyNebula.w * 0.07;
	}
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
	// How much the sky is space rather than air: by how little air it is seen through, eased -- not switched at a threshold,
	// which going up through the air turned a fading sky into black space in a frame.
	float inSpace = step(0.001, u_skySpace.x) * (1.0 - smoothstep(0.0, 0.25, u_skyAir.w));
	// Through air at night the sky is still a sky: a faint glow of the air low along the horizon, as much as it is dark.
	color += u_skyAir.rgb * (0.035 * u_skyAir.w * u_skySpace.x) * exp(-max(up, 0.0) * 7.0) * step(-0.05, up);
	color = mix(color, vec3(0.0006, 0.0007, 0.001), inSpace);
	// Through a storm's haze, a pale smudge rather than a sun: the core lost, the glow spread and dimmed.
	float haze = clamp(u_skyZenith.w, 0.0, 1.0);
	float spread = pow(toSun, max(u_skySun.w, 1.0) * 0.08);
	color += u_skySunColor.rgb * u_skySunColor.w * mix(halo * 0.35 + core, spread * 0.12, haze) * clamp(up * 4.0 + 0.4, 0.0, 1.0) * (1.0 - inSpace);
	// In space, with no air to spread it, the sun is a small hard disc, blinding, darker at its rim; a tight corona
	// round it and a faint wide glow; and the thin rays a lens throws off anything that bright, four long and four short.
	if (inSpace > 0.001)
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
		color += (discColour * disc * 80.0 + mix(hot, u_skySunColor.rgb, 0.4) * (glare + spikes * 0.18)) * u_skySunColor.w * inSpace;
	}

	// A planet: a sphere one unit away, as big on the sky as it is asked to be, lit by the sun, with ice and cloud
	// over it, and its air glowing at the edge on the lit side -- and a little beyond the edge.
	float starsShow = 1.0;
	// Seen up through air, what lies beyond it -- a planet, a moon -- is behind the sky, not cut out of it: the light the air
	// scatters lies over it (its night side the colour of the sky, not black), and less of it comes through the more air
	// there is, the lower it is, and the thicker a storm's haze. In space it hides what is behind it.
	float transmit = mix(1.0, mix(0.3, 0.8, smoothstep(0.0, 0.35, up)), clamp(u_skyAir.w * 2.0, 0.0, 1.0)) * (1.0 - haze);
	transmit = mix(transmit, 1.0, inSpace);
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
			vec4 surface = PlanetAlbedo(local, u_planetA, u_planetB, u_planetC, u_planetD, u_planetE.w, 0.0, 0.0, sea);
			// The lie of the land in the light: ranges catch it on one side and shadow the other, most of all near the line
			// between day and night.
			vec3 bumped = PlanetBump(local, u_planetA, u_planetD, u_planetE.w, 1.0, 0.0);
			vec3 bumpedWorld = normalize(east * bumped.x + pole * bumped.y + third * bumped.z);
			float relief = clamp(1.0 + (dot(bumpedWorld, towardsSun) - lit) * 1.3, 0.6, 1.35);
			vec3 ground = mix(surface.rgb * relief, u_planetD.rgb, surface.w);
			vec3 planet = ground * smoothstep(-0.05, 0.4, lit) * u_skySunColor.w * 1.6;
			planet += u_skySunColor.rgb * u_skySunColor.w * pow(max(dot(reflect(-towardsSun, normal), -ray), 0.0), 60.0) * sea *
					  (1.0 - surface.w) * step(0.0, lit) * 0.6;
			float edge = pow(1.0 - max(dot(normal, -ray), 0.0), 4.0);
			planet += air * edge * smoothstep(-0.25, 0.25, lit);
			color = color * (1.0 - inSpace) + planet * max(u_skySpace.w, 0.0) * transmit;
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
					// No further out than most of the way to the eye: a planet as near as a moon's is big on the sky, and rings
					// two and more of its radii across reached past the eye -- half of them behind it, the rest smeared out
					// across the sky in bands. Drawn smaller, they are still a wide ring round it.
					float outer = min(u_skyRings.y, 0.8 / max(radius, 0.001));
					float inner = u_skyRings.x * outer / max(u_skyRings.y, 0.001);
					if (r > inner && r < outer)
					{
						float across = (r - inner) / max(outer - inner, 0.001);
						float bands = SkyFbm(vec3(across * 38.0, u_planetE.w, 0.5));
						float gaps = smoothstep(0.2, 0.3, abs(fract(across * 3.1 + 0.37) - 0.5) * 2.0);
						float alpha = smoothstep(0.0, 0.05, across) * smoothstep(1.0, 0.94, across) * (0.3 + 0.65 * bands) * mix(0.35, 1.0, gaps);
						// Lit from whichever side the sun is, and in the planet's shadow behind it.
						vec3 q = toPlanet - at;
						float sunAlong = dot(q, towardsSun);
						float shadowed = sunAlong > 0.0 && length(q - towardsSun * sunAlong) < radius ? 1.0 : 0.0;
						float light = (0.35 + 0.65 * abs(dot(pole, towardsSun))) * (1.0 - shadowed * 0.92);
						vec3 ringColour = u_skyRingColor.rgb * (0.75 + 0.5 * bands) * light * u_skySunColor.w * 1.6;
						// (Seen up through air, hazed and let through as the planet is: the sky's light over them.)
						alpha *= transmit;
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
	if (starsShow > 0.5 && u_skySpace.x > 0.001)
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
				// Not a flat disc: a world's face -- mottled ground, and on one without air darker plains across it; on one with
				// air, soft swirls of cloud -- its own by its colour (the same each time it is seen), darker towards its rim.
				float seed = dot(look.rgb, vec3(17.3, 31.7, 7.9));
				float mottle = SkyFbm(bodyNormal * 3.5 + vec3_splat(seed));
				float fine = SkyFbm(bodyNormal * 11.0 + vec3_splat(seed * 1.7));
				vec3 albedo = look.rgb * (0.78 + 0.5 * (mottle - 0.5) + 0.25 * (fine - 0.5));
				if (look.w < 0.05)
				{
					albedo *= mix(0.6, 1.0, smoothstep(0.42, 0.58, SkyFbm(bodyNormal * 2.2 + vec3_splat(seed * 3.1))));
				}
				else
				{
					float swirl = SkyFbm(vec3(bodyNormal.x * 4.0, bodyNormal.y * 9.0, bodyNormal.z * 4.0) + vec3_splat(seed * 0.7));
					albedo = mix(albedo, vec3_splat(0.92), smoothstep(0.55, 0.75, swirl) * min(look.w * 2.0, 0.7));
				}
				albedo *= 0.65 + 0.35 * max(dot(bodyNormal, -ray), 0.0);
				vec3 shade = albedo * (smoothstep(-0.08, 0.35, bodyLit) * u_skySunColor.w * 1.8 + 0.01);
				shade += vec3(0.4, 0.6, 1.0) * look.w * pow(1.0 - max(dot(bodyNormal, -ray), 0.0), 3.0) * smoothstep(-0.2, 0.3, bodyLit);
				color = color * (1.0 - inSpace) + shade * max(u_skySpace.w, 0.0) * transmit;
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
				color += tint * (glint * 2.2 + halo * 0.07) * bright * phase * max(u_skySpace.w, 0.0) * transmit;
			}
		}
	}
	// The rings of the world underfoot (u_skyOwnRings: its pole over the place), from its ground: a band across the sky from
	// horizon to horizon -- low and edge on near the equator, arching high nearer the poles -- lit by the sun, cut across by the
	// world's own shadow at night, the stars dimmed behind it, and fainter through the haze of a thick day sky.
	if (u_skyOwnRings.w > 0.5 && u_skyRings.y > 0.0 && up > 0.0)
	{
		vec3 pole = normalize(u_skyOwnRings.xyz);
		// The world's middle, a radius below (in its radii).
		vec3 centre = vec3(0.0, -1.0, 0.0);
		float facing = dot(ray, pole);
		float t = abs(facing) > 0.00001 ? dot(centre, pole) / facing : -1.0;
		if (t > 0.0)
		{
			vec3 at = ray * t;
			float r = length(at - centre);
			if (r > u_skyRings.x && r < u_skyRings.y)
			{
				float across = (r - u_skyRings.x) / max(u_skyRings.y - u_skyRings.x, 0.001);
				float bands = SkyFbm(vec3(across * 38.0, u_planetE.w, 0.5));
				float gaps = smoothstep(0.2, 0.3, abs(fract(across * 3.1 + 0.37) - 0.5) * 2.0);
				float alpha = smoothstep(0.0, 0.05, across) * smoothstep(1.0, 0.94, across) * (0.3 + 0.65 * bands) * mix(0.35, 1.0, gaps);
				vec3 sunWay = normalize(u_skySun.xyz);
				vec3 q = centre - at;
				float sunAlong = dot(q, sunWay);
				float shadowed = sunAlong > 0.0 && length(q - sunWay * sunAlong) < 1.0 ? 1.0 : 0.0;
				float light = (0.35 + 0.65 * abs(dot(pole, sunWay))) * (1.0 - shadowed * 0.92);
				vec3 ringColour = u_skyRingColor.rgb * (0.75 + 0.5 * bands) * light * u_skySunColor.w * 1.6;
				color += ringColour * alpha * (1.0 - 0.8 * haze) * smoothstep(0.0, 0.04, up);
				starsShow *= 1.0 - alpha;
			}
		}
	}
	// Seen through air the stars dim and go low down, where there is the most of it to look through.
	float through = mix(1.0, smoothstep(-0.02, 0.35, up), clamp(u_skyAir.w * 4.0, 0.0, 1.0));
	color += SkyStars(ray) * u_skySpace.x * starsShow * max(u_skySpace.w, 0.0) * through * (1.0 - haze) * (1.0 - smoothstep(0.7, 0.95, u_skyClouds.x));

	// The clouds over a world with air: a layer a couple of kilometres up (lower, heavier and darker in a storm), drifting on
	// the wind up there, as much of the sky as the weather has them cover. Lit by the sky, and by the sun on the side facing
	// it -- shadowed where there is more cloud between it and the sun, bright at the edges round it -- and lost into the haze
	// towards the horizon. Over the stars and the sun.
	vec3 sunDir = normalize(u_skySun.xyz);
	// Lightning lights the whole sky, most round where it struck.
	float flashHere = mix(0.6, 1.0, pow(0.5 + 0.5 * cos(atan2(ray.x, -ray.z) - u_skyBolt.x), 2.0));
	if (u_skyClouds.x > 0.001 && up > -0.02 && inSpace < 0.999)
	{
		float cover = u_skyClouds.x;
		float heavy = u_skyClouds.y;
		float base = mix(2.0, 1.1, heavy);
		// Where this ray meets the layer, in kilometres, as far on as it has drifted.
		vec2 at = ray.xz / max(up, 0.015) * base - u_skyCloudWind.xy;
		vec3 q = vec3(at * 0.35, 3.7);
		// Billowing, not smeared: the pattern pushed about by a larger one of its own, and its edges eaten into by a finer
		// one -- less of that far off, where it would only shimmer.
		vec3 warp = vec3(SkyFbm(q * 0.5 + vec3(7.1, 0.0, 1.3)), SkyFbm(q * 0.5 + vec3(2.3, 9.1, 0.0)), 0.0) - vec3(0.5, 0.5, 0.0);
		float shape = SkyFbm(q + warp * 0.9);
		shape -= ((SkyFbm(q * 3.1 + vec3(1.3, 4.4, 2.2)) - 0.5) * 0.3 + (SkyFbm(q * 8.3 + vec3(5.5, 0.7, 3.9)) - 0.5) * 0.14 * smoothstep(0.12, 0.35, up)) *
		         smoothstep(0.03, 0.2, up) * (1.0 - 0.6 * heavy);
		float threshold = mix(0.6, 0.2, cover);
		// Crisp edges on a fair day's clouds, soft ones in a grey sky.
		float density = smoothstep(threshold, threshold + mix(0.07, 0.25, max(heavy, cover * cover)), shape);
		// How thick it is here: thick cloud lets little through, grey in its middle from below, its thin edges bright.
		float depth = clamp((shape - threshold) / 0.18, 0.0, 1.0);
		float through = exp(-depth * 2.5);
		// More cloud towards the sun than here: this part is in that cloud's shade.
		float shaded = clamp((SkyFbm(q + warp * 0.9 + vec3(sunDir.x, 0.0, sunDir.z) * 0.18) - shape) * 4.0 + 0.5, 0.0, 1.0);
		float sunUp = clamp(sunDir.y * 3.0 + 0.3, 0.0, 1.0);
		vec3 skyLight = mix(u_skyHorizon.rgb, u_skyZenith.rgb, 0.5);
		vec3 sunLight = u_skySunColor.rgb * u_skySunColor.w * sunUp;
		vec3 cloud = skyLight * mix(1.15, 0.45, heavy) * mix(1.0, 0.55, depth) +
		             sunLight * mix(1.3, 0.1, heavy) * (0.25 + 0.75 * through) * (1.0 - shaded * 0.6);
		// The thin edges near the sun lit through.
		cloud += sunLight * pow(max(dot(ray, sunDir), 0.0), 8.0) * through * 2.0 * (1.0 - heavy);
		// Lightning in them, lighting them from inside: the whole layer, the thickest and the nearest the strike most.
		cloud += vec3(0.8, 0.85, 1.0) * u_skyClouds.w * flashHere * (0.35 + 1.4 * depth) * 1.6;
		cloud = mix(cloud, u_skyHorizon.rgb + vec3(0.8, 0.85, 1.0) * u_skyClouds.w * flashHere * 0.6, smoothstep(0.25, 0.0, up) * 0.75);
		// A storm's sky is one low deck, not clouds with sky between them: closed over, its underside soft and mottled in great
		// dark lumps, ragged scud driven under it lower down and faster, and towards the horizon lost in the grey of the rain
		// -- no bright gaps, no streaks.
		float storm = smoothstep(0.5, 0.85, heavy) * smoothstep(0.8, 0.95, cover);
		if (storm > 0.001)
		{
			// The grey the rain fills the air with: the fog the ground fades into at a distance (the horizon is drawn from it,
			// u_skyFog), so the far hills are lost into it rather than standing out against it.
			vec3 fogLike = u_skyFog.rgb;
			vec3 grey = mix(vec3_splat(dot(fogLike, vec3(0.2126, 0.7152, 0.0722))), fogLike, 0.5);
			float lumps = SkyFbm(vec3(at * 0.12, 1.9) + warp * 0.4);
			vec3 deck = grey * mix(1.05, 0.7, lumps);
			vec2 low = ray.xz / max(up, 0.015) * base * 0.45 - u_skyCloudWind.xy * 1.8;
			float scud = smoothstep(0.55, 0.78, SkyFbm(vec3(low * 0.9, 6.1))) * smoothstep(0.04, 0.2, up);
			deck = mix(deck, grey * 0.45, scud * 0.7);
			vec3 flash = vec3(0.8, 0.85, 1.0) * u_skyClouds.w * flashHere;
			deck += flash * (0.6 + 0.8 * lumps) * 1.6;
			vec3 rain = fogLike + flash * 0.6;
			deck = mix(deck, rain, smoothstep(0.45, 0.0, up));
			cloud = mix(cloud, deck, storm);
			density = mix(density, 1.0, storm);
		}
		float alpha = density * smoothstep(-0.02, 0.06, up) * (1.0 - inSpace) * (1.0 - 0.3 * haze);
		color = mix(color, cloud, alpha);
	}
	// A flash lights the whole sky.
	color += vec3(0.6, 0.65, 0.8) * u_skyClouds.w * flashHere * 0.35 * (1.0 - inSpace) * step(-0.05, up);
	// And the bolt itself, while it strikes: from the ground to the clouds somewhere round the horizon, crooked at two sizes,
	// a fork off it part way, a hard white core in a glow -- dimmed through the rain.
	if (u_skyBolt.w > 0.001 && up > -0.01 && up < u_skyBolt.y)
	{
		float yaw = atan2(ray.x, -ray.z) - u_skyBolt.x;
		yaw -= 6.2831853 * floor((yaw + 3.14159265) / 6.2831853);
		float seed = u_skyBolt.z;
		float path = (SkyNoise(vec3(up * 60.0, seed, 1.0)) - 0.5) * 0.012 + (SkyNoise(vec3(up * 14.0, seed, 7.0)) - 0.5) * 0.03;
		float width = max(length(fwidth(ray)), 0.0003) * 1.2;
		float off = abs(yaw - path);
		float bolt = exp(-off / width) * 6.0 + exp(-off / (width * 12.0)) * 0.25;
		float forkFrom = u_skyBolt.y * 0.55;
		if (up < forkFrom && up > forkFrom * 0.35)
		{
			float fork = path + (forkFrom - up) * (fract(seed * 7.13) < 0.5 ? 0.5 : -0.5) + (SkyNoise(vec3(up * 80.0, seed, 3.0)) - 0.5) * 0.01;
			bolt += exp(-abs(yaw - fork) / width) * 3.0 * smoothstep(forkFrom * 0.35, forkFrom * 0.6, up);
		}
		color += vec3(0.85, 0.9, 1.0) * bolt * u_skyBolt.w * smoothstep(u_skyBolt.y, u_skyBolt.y * 0.85, up) * (1.0 - 0.6 * haze) *
		         (1.0 - inSpace);
	}

	// The torch's beam in the air it shines through, as fs_mesh draws it in front of everything else: here too, or it stops
	// where the ground meets the sky and looks like a light shone on whatever is furthest off.
	if (u_skyTorch[1].w > 0.001)
	{
		float reach = min(u_skyTorch[0].w, 30.0);
		vec3 aim = normalize(u_skyTorch[2].xyz);
		float lit = 0.0;
		for (int i = 0; i < 16; ++i)
		{
			float t = 0.5 + (float(i) + 0.5) / 16.0 * max(reach - 0.5, 0.0);
			vec3 fromLight = ray * t - u_skyTorch[0].xyz;
			float d = length(fromLight);
			float cone = clamp((dot(fromLight / max(d, 1e-3), aim) - u_skyTorch[3].x) / max(u_skyTorch[2].w - u_skyTorch[3].x, 1e-4), 0.0, 1.0);
			float window = clamp(1.0 - pow(d / max(u_skyTorch[0].w, 1e-3), 4.0), 0.0, 1.0);
			lit += cone * cone * window / max(d * d, 2.5);
		}
		color += u_skyTorch[1].rgb * lit * (max(reach - 0.5, 0.0) / 16.0) * u_skyTorch[1].w * 0.006 * 0.5;
	}

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
