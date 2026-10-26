$input a_position, a_normal, a_texcoord0
$output v_worldPos, v_texcoord0, v_wave, v_flow

#include <bgfx_shader.sh>

uniform vec4 u_waterEye;   // w = the waves' clock, seconds
uniform vec4 u_waterKind;  // z = how far out from the middle of the mesh its lines stay a few metres apart
uniform vec4 u_waves[6];   // each: its way (xy), its length (z) and its height (w), metres (Waves)
uniform vec4 u_waveParams; // x = how many, y = how sharp their crests, zw = the middle of the mesh (x, z)

// The water's surface (WaterRenderer): laid out in the world already at its level, how deep it is under each corner passed
// on (a_texcoord0.x) for its colour and its shore -- and moved by the waves (Waves, the same sums on the CPU): each point of
// it going round as they pass, the crests sharpened. A wave too short for the mesh's lines where it is (finer near, coarser
// out to the horizon) is let go of there, and in the shallows they all die down, so none stands up through the beach.
void main()
{
	vec3 rest = a_position;
	float depth = a_texcoord0.x;
	float t = u_waterEye.w;
	vec2 off = abs(rest.xz - u_waveParams.zw);
	float away = max(off.x, off.y);
	float spacing = away < u_waterKind.z ? 3.0 : clamp(away * 0.03, 4.0, 220.0);
	vec3 moved = vec3_splat(0.0);
	vec2 slope = vec2_splat(0.0);
	float crest = 0.0;
	float total = 0.0;
	for (int i = 0; i < 6; ++i)
	{
		if (float(i) >= u_waveParams.x)
		{
			break;
		}
		vec4 wave = u_waves[i];
		float k = 6.2831853 / max(wave.z, 0.01);
		float speed = sqrt(9.81 * k);
		float phase = k * dot(wave.xy, rest.xz) - speed * t + float(i) * 1.7;
		// (None on a stream: a_texcoord0.y over fifty, its way of flowing.)
		float height = wave.w * smoothstep(4.0 * spacing, 6.0 * spacing, wave.z) * clamp(depth / max(wave.w * 3.0, 0.01), 0.0, 1.0) * step(a_texcoord0.y, 50.0);
		float sharp = u_waveParams.y / max(k * wave.w * u_waveParams.x, 1.0e-4);
		float c = cos(phase);
		float s = sin(phase);
		moved += vec3(wave.x * sharp * height * c, height * s, wave.y * sharp * height * c);
		slope += wave.xy * (k * height * c);
		crest += height * s;
		total += height;
	}
	v_worldPos = rest + moved;
	v_texcoord0 = a_texcoord0;
	// (A stream's: how much this is falling or white water, not running; how far across it from its middle and how far down it,
	// metres: a_normal, SiteTerrain::WaterMesh.)
	v_flow = a_normal;
	// (The surface's lean from them for the light, how near a crest this is, 0 to 1, and how high they are here.)
	v_wave = vec4(slope, total > 1.0e-4 ? crest / total * 0.5 + 0.5 : 0.5, total);
	gl_Position = mul(u_viewProj, vec4(v_worldPos, 1.0));
}
