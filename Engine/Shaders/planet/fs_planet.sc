$input v_objectPos, v_normal, v_worldPos, v_texcoord0

#include <bgfx_shader.sh>
#include "planet/planet_surface.sh"

// The system map's bodies (PlanetRenderer): a planet, a star, the glow round a star, or a planet's rings.
uniform vec4 u_planetA;          // ground colour A, sea's share
uniform vec4 u_planetB;          // ground colour B, ice
uniform vec4 u_planetC;          // sea colour, cloud
uniform vec4 u_planetD;          // cloud colour, 1 for a gas giant
uniform vec4 u_planetE;          // the air's colour, how much air
uniform vec4 u_planetLight;      // xyz towards the light, w how bright
uniform vec4 u_planetLightColor; // rgb the light's colour (a star's own, for a star), w 1 for linear light (the world's picture)
uniform vec4 u_planetMode;       // x what is drawn (0 body, 1 star, 2 glow, 3 rings, 4 body from its baked maps), y seed, z cloud drift
uniform vec4 u_planetEye;        // xyz where the camera is, w exposure
// A body's face baked (fs_planet_bake): colour (square-rooted) and open water; the ground's tip and the cloud.
SAMPLER2D(s_planetSurface, 0);
SAMPLER2D(s_planetDetail, 1);

// Where a direction on the body is in its baked maps -- longitude across, from the pole down -- taking whichever of two
// longitudes runs on smoothly here, so the line where it wraps round does not show (Tarini's trick).
vec2 BakedAt(vec3 d)
{
	float u = atan2(d.z, d.x) / 6.28318530718 + 0.5;
	float other = fract(u + 0.5) - 0.5;
	u = fwidth(u) <= fwidth(other) + 0.001 ? u : other;
	return vec2(u, acos(clamp(d.y, -1.0, 1.0)) / 3.14159265359);
}

vec3 Develop(vec3 colour)
{
	colour *= max(u_planetEye.w, 0.0);
	// Into the world's picture: light as it is, developed with everything else after.
	if (u_planetLightColor.w > 0.5)
	{
		return colour;
	}
	colour = clamp((colour * (2.51 * colour + 0.03)) / (colour * (2.43 * colour + 0.59) + 0.14), 0.0, 1.0);
	return pow(colour, vec3_splat(1.0 / 2.2));
}

void main()
{
	float mode = u_planetMode.x;
	vec3 V = normalize(u_planetEye.xyz - v_worldPos);
	if (mode > 2.5 && mode < 3.5)
	{
		// Rings: banded across their width, thin at both edges, lit from whichever side the star is on.
		float t = v_texcoord0.x;
		float bands = PlanetFbm(vec3(t * 46.0, u_planetMode.y, 0.5));
		float alpha = smoothstep(0.0, 0.06, t) * smoothstep(1.0, 0.92, t) * (0.25 + 0.6 * bands);
		vec3 N = normalize(v_normal);
		float lit = abs(dot(N, normalize(u_planetLight.xyz))) * 0.75 + 0.25;
		vec3 colour = u_planetE.rgb * lit * u_planetLight.w * u_planetLightColor.rgb;
		gl_FragColor = vec4(Develop(colour), alpha);
		return;
	}
	if (mode > 1.5 && mode < 2.5)
	{
		// The corona round a star, on a square facing the camera: tight to the disc, faint streamers out from it, and a
		// soft glow beyond that falls off fast -- not a fog over the inner planets.
		vec2 p = v_texcoord0 * 2.0 - vec2_splat(1.0);
		float r = length(p);
		float angle = atan2(p.y, p.x);
		float streamers = 0.75 + 0.25 * PlanetNoise(vec3(cos(angle) * 3.0, sin(angle) * 3.0, u_planetMode.z * 0.02));
		float glow = exp(-max(r - 0.2, 0.0) * 14.0) * 0.9 * streamers + exp(-r * 7.0) * 0.12;
		glow *= smoothstep(1.0, 0.8, r);
		gl_FragColor = vec4(Develop(mix(u_planetLightColor.rgb, vec3_splat(1.0), 0.35) * glow * 0.9), 1.0);
		return;
	}
	vec3 n = normalize(v_objectPos);
	vec3 N = normalize(v_normal);
	if (mode > 0.5 && mode < 1.5)
	{
		// A star: its surface a boil of small cells, a spot or two, and its limb darker and redder than its middle --
		// the way a real one looks through a filter, and white hot in the middle.
		float mu = max(dot(N, V), 0.0);
		vec3 drift = vec3_splat(u_planetMode.z * 0.05);
		float cells = PlanetNoise(n * 70.0 + drift) * 0.6 + PlanetNoise(n * 140.0 - drift) * 0.4;
		float faculae = PlanetFbmN(n * 9.0 + drift, 4);
		float spots = smoothstep(0.7, 0.78, PlanetFbmN(n * 4.0 + vec3_splat(u_planetMode.y), 4));
		vec3 hot = mix(u_planetLightColor.rgb, vec3_splat(1.0), 0.55);
		vec3 rim = u_planetLightColor.rgb * vec3(1.0, 0.62, 0.36);
		vec3 surface = mix(rim, hot, pow(mu, 0.5)) * (0.5 + 0.5 * pow(mu, 0.6));
		surface *= 0.82 + 0.3 * cells + 0.12 * faculae;
		surface *= 1.0 - spots * 0.7;
		gl_FragColor = vec4(Develop(surface * 3.4), 1.0);
		return;
	}

	float sea = 0.0;
	vec4 surface;
	vec3 bumped;
	if (mode > 3.5)
	{
		// Read from its maps: the same as worked out below, at a fraction of the cost.
		vec4 ground = texture2D(s_planetSurface, BakedAt(n));
		vec4 tip = texture2D(s_planetDetail, BakedAt(n));
		float drift = u_planetMode.z;
		vec3 m = vec3(n.x * cos(drift) - n.z * sin(drift), n.y, n.x * sin(drift) + n.z * cos(drift));
		surface = vec4(ground.rgb * ground.rgb, texture2D(s_planetDetail, BakedAt(m)).z);
		sea = ground.a;
		vec3 t1 = normalize(cross(n, abs(n.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
		vec3 t2 = cross(n, t1);
		bumped = normalize(n + t1 * tip.x + t2 * tip.y);
		// Seen close -- from a low orbit, a texel of its maps bigger than a pixel -- finer than the maps hold, worked out here:
		// the ground's grain and its roughness in the light, and the cloud's edges broken into wisps. Nothing of it when
		// the body is small, where the maps already hold more than the screen does.
		float texels = length(fwidth(n)) * 2048.0 / 6.28318530718;
		float close = 1.0 - smoothstep(0.35, 1.2, texels);
		if (close > 0.001)
		{
			vec3 offset = vec3(u_planetMode.y * 1.7, u_planetMode.y * 0.9, u_planetMode.y * 2.3);
			float grain = PlanetFbmN(n * 170.0 + offset, 4);
			surface.rgb *= mix(1.0, 0.78 + 0.44 * grain, close * (1.0 - sea));
			float wisps = PlanetFbmN(m * 70.0 + offset * 3.0, 4);
			float broken = clamp(surface.w + (wisps - 0.5) * 0.7 * (1.0 - abs(surface.w * 2.0 - 1.0) * 0.6), 0.0, 1.0);
			surface.w = mix(surface.w, smoothstep(0.15, 0.85, broken), close);
			float rough = PlanetFbmN(n * 240.0 + offset * 0.5, 3) - 0.5;
			bumped = normalize(bumped + (t1 * rough - t2 * rough * 0.6) * 0.12 * close * (1.0 - sea));
		}
	}
	else
	{
		// Cratered as far as it has no air to speak of to wear them away; a gas giant has no ground.
		float craters = u_planetD.w > 0.5 ? 0.0 : 1.0 - smoothstep(0.02, 0.15, u_planetE.w);
		surface = PlanetAlbedo(n, u_planetA, u_planetB, u_planetC, u_planetD, u_planetMode.y, u_planetMode.z, craters, sea);
		// The lie of the land in the light (exact when the body is drawn unturned, as the globe view draws it).
		bumped = PlanetBump(n, u_planetA, u_planetD, u_planetMode.y, 1.0, craters);
	}
	vec3 L = normalize(u_planetLight.xyz);
	float facing = dot(N, L);
	// A soft edge between day and night: an atmosphere carries a little light round it.
	float lit = smoothstep(-0.1, 0.3, facing);
	vec3 bumpedN = normalize(N + (bumped - n));
	float relief = clamp(1.0 + (dot(bumpedN, L) - facing) * 1.8, 0.5, 1.45);
	vec3 albedo = mix(surface.rgb * relief, u_planetD.rgb, surface.w);
	vec3 light = u_planetLightColor.rgb * u_planetLight.w;
	// A little light on the night side, so a world seen from behind is still a world and not a hole.
	vec3 colour = albedo * (lit * light + vec3_splat(0.05));
	// A stormy world's storms flickering with lightning (thunderstorms and ash storms: their storms are over 0.6, a dust
	// storm's or a blizzard's under it), seen from space on its night side: here and there in the thick of
	// the cloud a soft flash, gone a moment later, somewhere else the next.
	float storms = clamp(-u_planetD.w, 0.0, 1.0);
	if (storms > 0.6 && surface.w > 0.4 && u_planetE.w > 0.015)
	{
		vec3 cells = n * 22.0;
		vec3 cell = floor(cells);
		float tick = floor(u_planetMode.z * 300.0);
		float strikes = step(1.0 - 0.012 * storms, PlanetHash(cell + vec3(tick * 0.13, tick * 0.71, tick * 0.37)));
		float soft = exp(-length(fract(cells) - vec3_splat(0.5)) * 5.0);
		colour += vec3(0.7, 0.75, 1.0) * strikes * soft * smoothstep(0.4, 0.8, surface.w) * (1.0 - lit) * 1.6;
	}
	// The sun off open water.
	float glint = pow(max(dot(reflect(-L, N), V), 0.0), 60.0) * sea * (1.0 - surface.w) * step(0.0, facing);
	colour += light * glint * 0.5;
	// The air at the edge, on the lit side and a little past it.
	float edge = pow(1.0 - max(dot(N, V), 0.0), 3.0);
	colour += u_planetE.rgb * u_planetE.w * edge * (smoothstep(-0.3, 0.3, facing) + 0.02) * light * 0.9;
	gl_FragColor = vec4(Develop(colour), 1.0);
}
