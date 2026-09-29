$input v_ray

#include <bgfx_shader.sh>

uniform vec4 u_skyZenith;  // rgb
uniform vec4 u_skyHorizon; // rgb
uniform vec4 u_skyGround;  // rgb
uniform vec4 u_skySun;     // xyz = direction towards the sun, w = how sharp the glow is
uniform vec4 u_skySunColor; // rgb, w = how bright
uniform vec4 u_skyGrade;    // x = exposure, y = contrast
uniform vec4 u_skySpace;       // x = stars (0 none, 1 all), y = a planet's radius on the sky (radians; 0 none), z = its air, w = brightness
uniform vec4 u_skyPlanet;      // xyz = towards the planet's middle
uniform vec4 u_skyPlanetColor; // rgb = its ground from orbit

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
	vec3 cell = floor(p);
	float h = SkyHash(cell);
	vec3 light = vec3_splat(0.0);
	if (h > 0.97)
	{
		vec3 jitter = vec3(SkyHash(cell + vec3_splat(7.1)), SkyHash(cell + vec3_splat(3.7)), SkyHash(cell + vec3_splat(11.3)));
		vec3 centre = normalize(cell + vec3_splat(0.5) + (jitter - vec3_splat(0.5)) * 0.6) * 180.0;
		float bright = (h - 0.97) / 0.03;
		float star = smoothstep(0.3, 0.0, length(p - centre)) * (0.12 + 3.2 * bright * bright * bright);
		light = mix(vec3(0.72, 0.8, 1.0), vec3(1.0, 0.86, 0.7), SkyHash(cell + vec3_splat(5.3))) * star;
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
	color += u_skySunColor.rgb * u_skySunColor.w * (halo * mix(0.35, 0.06, inSpace) + core) * mix(clamp(up * 4.0 + 0.4, 0.0, 1.0), 1.0, inSpace);

	// A planet: a sphere one unit away, as big on the sky as it is asked to be, lit by the sun, with ice and cloud
	// over it, and its air glowing at the edge on the lit side -- and a little beyond the edge.
	float starsShow = 1.0;
	if (u_skySpace.y > 0.0)
	{
		vec3 toPlanet = normalize(u_skyPlanet.xyz);
		vec3 towardsSun = normalize(u_skySun.xyz);
		vec3 air = vec3(0.3, 0.5, 0.95) * u_skySpace.z;
		float radius = sin(u_skySpace.y);
		float along = dot(ray, toPlanet);
		float hit = along * along - (1.0 - radius * radius);
		if (hit > 0.0 && along > 0.0)
		{
			vec3 normal = (ray * (along - sqrt(hit)) - toPlanet) / radius;
			float lit = dot(normal, towardsSun);
			float land = SkyFbm(normal * 2.5 + vec3(3.1, 1.7, 5.3));
			float cloud = smoothstep(0.5, 0.78, SkyFbm(normal * 4.5 + vec3(9.1, 2.2, 4.4)));
			vec3 ground = mix(u_skyPlanetColor.rgb * (0.55 + 0.7 * land), vec3(0.95, 0.97, 1.0), cloud * 0.8);
			vec3 planet = ground * smoothstep(-0.05, 0.4, lit) * u_skySunColor.w * 1.6;
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
