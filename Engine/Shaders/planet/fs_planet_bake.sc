$input v_texcoord0

#include <bgfx_shader.sh>
#include "planet/planet_surface.sh"

// A planet's face worked out once, into two maps of its whole surface (longitude across, from the pole down), for
// fs_planet to read instead of working it out again for every pixel every frame (PlanetRenderer::Body). The first holds
// its colour under the cloud (square-rooted, for the dark end's sake) and how much of it is open water; the second how
// its ground tips the light (the bumped normal against the sphere's, in the same two directions across it fs_planet
// uses), and its cloud as it stands with no drift -- the drift is a turn, so fs_planet turns where it reads.
uniform vec4 u_planetA; // ground colour A, sea's share
uniform vec4 u_planetB; // ground colour B, ice
uniform vec4 u_planetC; // sea colour, cloud
uniform vec4 u_planetD; // cloud colour, 1 for a gas giant
uniform vec4 u_planetE; // the air's colour, how much air
uniform vec4 u_planetMode; // y seed

void main()
{
	float longitude = (v_texcoord0.x - 0.5) * 6.28318530718;
	float colatitude = v_texcoord0.y * 3.14159265359;
	vec3 n = vec3(sin(colatitude) * cos(longitude), cos(colatitude), sin(colatitude) * sin(longitude));
	float craters = u_planetD.w > 0.5 ? 0.0 : 1.0 - smoothstep(0.02, 0.15, u_planetE.w);
	float sea = 0.0;
	vec4 surface = PlanetAlbedo(n, u_planetA, u_planetB, u_planetC, u_planetD, u_planetMode.y, 0.0, craters, sea);
	vec3 bumped = PlanetBump(n, u_planetA, u_planetD, u_planetMode.y, 1.0, craters);
	vec3 t1 = normalize(cross(n, abs(n.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
	vec3 t2 = cross(n, t1);
	vec3 tip = bumped - n;
	gl_FragData[0] = vec4(sqrt(max(surface.rgb, vec3_splat(0.0))), sea);
	gl_FragData[1] = vec4(dot(tip, t1), dot(tip, t2), surface.w, 1.0);
}
