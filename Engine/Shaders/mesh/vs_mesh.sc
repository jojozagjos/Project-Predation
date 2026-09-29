$input a_position, a_normal, a_texcoord0, a_color0
$output v_worldPos, v_normal, v_texcoord0, v_color0, v_organic

#include <bgfx_shader.sh>

uniform vec4 u_organic;     // see Material::organic
uniform vec4 u_organicBeat; // see Material::organicBeat

float organicBump(float t, float at, float width)
{
	float x = (t - at) / width;
	return x > 0.0 && x < 1.0 ? sin(x * 3.14159265) : 0.0;
}

void main()
{
	vec3 position = a_position;
	v_organic = vec2(1.0, 0.0);
	if (u_organic.x > 0.5)
	{
		// Grown: a front creeping out from the source, a metre and a bit wide; beyond it the tissue lies
		// flat into the surface under it, and past its very edge it is not drawn at all.
		float along = a_texcoord0.x;
		float height = a_texcoord0.y;
		float grown = clamp((u_organic.y - along) / 1.2, 0.0, 1.0);
		// Dead from the source outwards, and then rotted away into the surface to nothing.
		float dead = clamp((u_organic.z - along) / 1.5, 0.0, 1.0);
		float rotted = u_organic.w > 0.0 ? clamp((u_organic.z - along - 3.0) / u_organic.w, 0.0, 1.0) : 0.0;
		// And each beat, going out across it as a swell.
		float beat = fract(u_organicBeat.x - along * u_organicBeat.z);
		float swell = (organicBump(beat, 0.0, 0.14) + 0.6 * organicBump(beat, 0.2, 0.12)) * u_organicBeat.y * (1.0 - dead);
		float thickness = grown * (1.0 + 0.4 * swell) * (1.0 - rotted);
		position -= a_normal * height * (1.0 - thickness);
		v_organic = vec2(grown * (1.0 - rotted), dead);
	}
	vec4 worldPosition = mul(u_model[0], vec4(position, 1.0));

	v_worldPos = worldPosition.xyz;
	v_normal = mul(u_model[0], vec4(a_normal, 0.0)).xyz;
	v_texcoord0 = a_texcoord0;
	v_color0 = a_color0;

	gl_Position = mul(u_viewProj, worldPosition);
}
