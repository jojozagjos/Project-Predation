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
uniform vec4 u_planetLightColor; // rgb the light's colour (a star's own, for a star)
uniform vec4 u_planetMode;       // x what is drawn (0 body, 1 star, 2 glow, 3 rings), y seed, z cloud drift, w highlight
uniform vec4 u_planetEye;        // xyz where the camera is, w exposure

vec3 Develop(vec3 colour)
{
	colour *= max(u_planetEye.w, 0.0);
	colour = clamp((colour * (2.51 * colour + 0.03)) / (colour * (2.43 * colour + 0.59) + 0.14), 0.0, 1.0);
	return pow(colour, vec3_splat(1.0 / 2.2));
}

void main()
{
	float mode = u_planetMode.x;
	vec3 V = normalize(u_planetEye.xyz - v_worldPos);
	if (mode > 2.5)
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
	if (mode > 1.5)
	{
		// The glow round a star, on a square facing the camera: a bright core and a wide soft halo.
		vec2 p = v_texcoord0 * 2.0 - vec2_splat(1.0);
		float r = length(p);
		float glow = exp(-r * r * 9.0) * 0.8 + exp(-r * 4.0) * 0.18;
		glow *= smoothstep(1.0, 0.85, r);
		gl_FragColor = vec4(Develop(u_planetLightColor.rgb * glow * 0.8), 1.0);
		return;
	}
	vec3 n = normalize(v_objectPos);
	vec3 N = normalize(v_normal);
	if (mode > 0.5)
	{
		// A star: blinding, mottled, darker at its limb.
		float mottle = PlanetFbm(n * 10.0 + vec3_splat(u_planetMode.z * 0.05));
		float limb = 0.55 + 0.45 * max(dot(N, V), 0.0);
		gl_FragColor = vec4(Develop(u_planetLightColor.rgb * (2.6 + 1.6 * mottle) * limb), 1.0);
		return;
	}

	float sea = 0.0;
	vec4 surface = PlanetAlbedo(n, u_planetA, u_planetB, u_planetC, u_planetD, u_planetMode.y, u_planetMode.z, sea);
	vec3 L = normalize(u_planetLight.xyz);
	float facing = dot(N, L);
	// A soft edge between day and night: an atmosphere carries a little light round it.
	float lit = smoothstep(-0.1, 0.3, facing);
	vec3 albedo = mix(surface.rgb, u_planetD.rgb, surface.w);
	vec3 light = u_planetLightColor.rgb * u_planetLight.w;
	// A little light on the night side, so a world seen from behind is still a world and not a hole.
	vec3 colour = albedo * (lit * light + vec3_splat(0.05));
	// The sun off open water.
	float glint = pow(max(dot(reflect(-L, N), V), 0.0), 60.0) * sea * (1.0 - surface.w) * step(0.0, facing);
	colour += light * glint * 0.5;
	// The air at the edge, on the lit side and a little past it.
	float edge = pow(1.0 - max(dot(N, V), 0.0), 3.0);
	colour += u_planetE.rgb * u_planetE.w * edge * (smoothstep(-0.3, 0.3, facing) + 0.02) * light * 0.9;
	// Picked out: an amber rim.
	colour += vec3(1.0, 0.6, 0.22) * pow(1.0 - max(dot(N, V), 0.0), 2.0) * u_planetMode.w * 0.9;
	gl_FragColor = vec4(Develop(colour), 1.0);
}
